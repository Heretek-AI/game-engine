// forward_renderer.cpp — P1-06: tri pipeline build + triangle record.
//
// Real Vulkan path: creates 2 shader modules from the checked-in tri .inc
// blobs and builds 1 graphics pipeline (swapchain UNORM target, dynamic
// rendering, no vertex input, dynamic viewport/scissor, no culling, no
// depth, no blending) with an empty pipeline layout. record_triangle()
// opens/closes its own dynamic-rendering pass (CLEAR black, STORE) and
// draws 3 vertices (positions/colors from gl_VertexIndex constant arrays).
// Entry points load via VulkanDevice::proc()/instance_proc() (same pattern
// as the GI frame graph: device-level vk* through proc(), instance-level
// through instance_proc()).
#include "engine/renderer/forward_renderer.hpp"

#include "engine/renderer/shaders/mesh_frag_spv.inc"
#include "engine/renderer/shaders/mesh_skinned_vert_spv.inc"
#include "engine/renderer/shaders/mesh_vert_spv.inc"
#include "engine/renderer/shaders/pick_frag_spv.inc"
#include "engine/renderer/shaders/pick_vert_spv.inc"
#include "engine/renderer/shaders/tri_frag_spv.inc"
#include "engine/renderer/shaders/tri_vert_spv.inc"

#if defined(ENGINE_RENDERER_HAS_VULKAN)
// volk-first include order (same rule as vulkan_device.cpp): volk defines
// VK_NO_PROTOTYPES and hard-errors if vulkan.h came first.
#include <volk.h>
#endif

#include "engine/renderer/gpu_mesh.hpp"
#include "engine/renderer/mesh_registry.hpp"
#include "engine/renderer/vulkan_device.hpp"
#include "flecs.h"

namespace engine::renderer {

ForwardRenderer::~ForwardRenderer() noexcept { destroy(); }

#if defined(ENGINE_RENDERER_HAS_VULKAN)

namespace {

// Per-draw skinned descriptor pool budget: one fresh set per
// record_mesh_skinned call, so no draw ever rewrites a set that is already
// bound in the same command buffer or still in flight (CodeRabbit Major on
// the single-set R1 design). Exhaustion fails closed with kOutOfMemory and
// the pool is drained on destroy(); recycling across frame boundaries is
// P5-03 scope (per-frame reset), same rule as the legacy vertex/index
// upload ring.
inline constexpr std::uint32_t kSkinPoolSets = 16;

auto valid_spv(const std::uint32_t* w, std::size_t n) noexcept -> bool {
  return w != nullptr && n > 5 && w[0] == 0x07230203u && w[1] >= 0x00010300u;
}

// P5-02: shared fixed-function mesh state (input assembly, viewport,
// raster, multisample, depth, blend, dynamics, rendering). create_mesh,
// create_mesh_skinned and create_pick spell the identical state; one
// helper keeps the three mirrors from tripping the Sonar duplication
// gate. Behavior-identical by construction: same struct literals.
struct MeshFixedState {
  VkPipelineInputAssemblyStateCreateInfo input_assembly{};
  VkPipelineViewportStateCreateInfo viewport{};
  VkPipelineRasterizationStateCreateInfo raster{};
  VkPipelineMultisampleStateCreateInfo msaa{};
  VkPipelineDepthStencilStateCreateInfo depth{};
  VkPipelineColorBlendAttachmentState blend_att{};
  VkPipelineColorBlendStateCreateInfo blend{};
  VkDynamicState dyn_states[2]{};
  VkPipelineDynamicStateCreateInfo dyn{};
  VkPipelineRenderingCreateInfo rendering{};
};

inline auto make_mesh_fixed_state() noexcept -> MeshFixedState {
  MeshFixedState s{};
  s.input_assembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  s.input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  s.viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  s.viewport.viewportCount = 1;
  s.viewport.scissorCount = 1;
  s.raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  s.raster.polygonMode = VK_POLYGON_MODE_FILL;
  s.raster.cullMode = VK_CULL_MODE_NONE;
  s.raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  s.raster.lineWidth = 1.0F;
  s.msaa.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  s.msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  s.depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  s.depth.depthTestEnable = VK_TRUE;
  s.depth.depthWriteEnable = VK_TRUE;
  s.depth.depthCompareOp = VK_COMPARE_OP_LESS;
  s.blend_att.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  s.blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  s.blend.attachmentCount = 1;
  s.blend.pAttachments = &s.blend_att;
  s.dyn_states[0] = VK_DYNAMIC_STATE_VIEWPORT;
  s.dyn_states[1] = VK_DYNAMIC_STATE_SCISSOR;
  s.dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  s.dyn.dynamicStateCount = 2;
  s.dyn.pDynamicStates = s.dyn_states;
  s.rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  s.rendering.colorAttachmentCount = 1;
  // NB: pColorAttachmentFormats / depthAttachmentFormat are filled by the
  // caller (they point at the caller's formats) — the struct must stay
  // alive only for the vkCreateGraphicsPipelines call.
  s.rendering.pColorAttachmentFormats = nullptr;
  s.rendering.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
  return s;
}

}  // namespace

auto ForwardRenderer::create(VulkanDevice& dev, VkFormat color_format,
                             VkFormat depth_format,
                             const std::uint32_t* vert_spv,
                             std::size_t vert_words,
                             const std::uint32_t* frag_spv,
                             std::size_t frag_words) noexcept -> Status {
  if (ready_) {
    return MakeFail(StatusCode::kAlreadyInitialized, "forward built");
  }
  // P1-06 renders with no depth attachment: only UNDEFINED is legal today
  // (P1-07 wires D32_SFLOAT through here for the lit cube).
  if (depth_format != VK_FORMAT_UNDEFINED) {
    return MakeFail(StatusCode::kInvalidArgument, "forward depth unsupported");
  }
  // Default blobs: the checked-in tri shaders.
  const std::uint32_t* vert = (vert_spv != nullptr) ? vert_spv : kTri_vertSpv;
  const std::size_t vert_n =
      (vert_spv != nullptr) ? vert_words : kTri_vertSpvWords;
  const std::uint32_t* frag = (frag_spv != nullptr) ? frag_spv : kTri_fragSpv;
  const std::size_t frag_n =
      (frag_spv != nullptr) ? frag_words : kTri_fragSpvWords;
  if (!valid_spv(vert, vert_n) || !valid_spv(frag, frag_n)) {
    return MakeFail(StatusCode::kInvalidArgument, "forward bad blob");
  }
  if (color_format != VK_FORMAT_B8G8R8A8_UNORM &&
      color_format != VK_FORMAT_R8G8B8A8_UNORM) {
    return MakeFail(StatusCode::kInvalidArgument, "forward bad format");
  }
  VkDevice device = dev.device();
  if (device == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "forward null device");
  }
  auto p_vkCreateShaderModule = reinterpret_cast<PFN_vkCreateShaderModule>(
      dev.instance_proc("vkCreateShaderModule"));
  auto p_vkDestroyShaderModule = reinterpret_cast<PFN_vkDestroyShaderModule>(
      dev.instance_proc("vkDestroyShaderModule"));
  auto p_vkCreateGraphicsPipelines =
      reinterpret_cast<PFN_vkCreateGraphicsPipelines>(
          dev.proc("vkCreateGraphicsPipelines"));
  auto p_vkDestroyPipeline = reinterpret_cast<PFN_vkDestroyPipeline>(
      dev.proc("vkDestroyPipeline"));
  auto p_vkCreatePipelineLayout = reinterpret_cast<PFN_vkCreatePipelineLayout>(
      dev.instance_proc("vkCreatePipelineLayout"));
  auto p_vkDestroyPipelineLayout =
      reinterpret_cast<PFN_vkDestroyPipelineLayout>(
          dev.instance_proc("vkDestroyPipelineLayout"));
  auto p_vkCmdBindPipeline = reinterpret_cast<PFN_vkCmdBindPipeline>(
      dev.proc("vkCmdBindPipeline"));
  auto p_vkCmdSetViewport =
      reinterpret_cast<PFN_vkCmdSetViewport>(dev.proc("vkCmdSetViewport"));
  auto p_vkCmdSetScissor =
      reinterpret_cast<PFN_vkCmdSetScissor>(dev.proc("vkCmdSetScissor"));
  auto p_vkCmdDraw = reinterpret_cast<PFN_vkCmdDraw>(dev.proc("vkCmdDraw"));
  auto p_vkCmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(
      dev.proc("vkCmdBeginRendering"));
  auto p_vkCmdEndRendering =
      reinterpret_cast<PFN_vkCmdEndRendering>(dev.proc("vkCmdEndRendering"));
  if (p_vkCreateShaderModule == nullptr || p_vkDestroyShaderModule == nullptr ||
      p_vkCreateGraphicsPipelines == nullptr || p_vkDestroyPipeline == nullptr ||
      p_vkCreatePipelineLayout == nullptr || p_vkDestroyPipelineLayout == nullptr ||
      p_vkCmdBindPipeline == nullptr || p_vkCmdSetViewport == nullptr ||
      p_vkCmdSetScissor == nullptr || p_vkCmdDraw == nullptr ||
      p_vkCmdBeginRendering == nullptr || p_vkCmdEndRendering == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "forward no entry");
  }
  const std::uint32_t* blobs[2] = {vert, frag};
  const std::size_t words[2] = {vert_n, frag_n};
  for (int i = 0; i < 2; ++i) {
    VkShaderModuleCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sci.codeSize = words[i] * 4;
    sci.pCode = blobs[i];
    if (p_vkCreateShaderModule(device, &sci, nullptr, &modules_[i]) !=
        VK_SUCCESS) {
      for (int j = 0; j < i; ++j) {
        p_vkDestroyShaderModule(device, modules_[j], nullptr);
        modules_[j] = nullptr;
      }
      return MakeFail(StatusCode::kDeviceCreateFailed, "forward module");
    }
    ++module_count_;
  }
  // Empty pipeline layout (no descriptor sets, no push constants).
  VkPipelineLayout pipe_layout = nullptr;
  {
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    if (p_vkCreatePipelineLayout(device, &pli, nullptr, &pipe_layout) !=
        VK_SUCCESS) {
      for (int j = 0; j < 2; ++j) {
        p_vkDestroyShaderModule(device, modules_[j], nullptr);
        modules_[j] = nullptr;
      }
      module_count_ = 0;
      return MakeFail(StatusCode::kDeviceCreateFailed, "forward layout");
    }
  }
  VkPipeline pipe = nullptr;
  {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = modules_[0];
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = modules_[1];
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0F;

    VkPipelineMultisampleStateCreateInfo msaa{};
    msaa.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

    VkPipelineColorBlendAttachmentState blend_att{};
    blend_att.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_att;

    const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                          VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;

    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &color_format;

    VkGraphicsPipelineCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pci.pNext = &rendering;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vertex_input;
    pci.pInputAssemblyState = &input_assembly;
    pci.pViewportState = &viewport;
    pci.pRasterizationState = &raster;
    pci.pMultisampleState = &msaa;
    pci.pDepthStencilState = &depth;
    pci.pColorBlendState = &blend;
    pci.pDynamicState = &dyn;
    pci.layout = pipe_layout;

    if (p_vkCreateGraphicsPipelines(device, nullptr, 1, &pci, nullptr, &pipe) !=
        VK_SUCCESS) {
      p_vkDestroyPipelineLayout(device, pipe_layout, nullptr);
      for (int j = 0; j < 2; ++j) {
        p_vkDestroyShaderModule(device, modules_[j], nullptr);
        modules_[j] = nullptr;
      }
      module_count_ = 0;
      return MakeFail(StatusCode::kDeviceCreateFailed, "forward pipe");
    }
  }
  pipeline_ = pipe;
  pipe_layout_ = pipe_layout;
  device_ = device;
  proc_create_module_ = reinterpret_cast<void*>(p_vkCreateShaderModule);
  proc_destroy_module_ = reinterpret_cast<void*>(p_vkDestroyShaderModule);
  proc_create_pipelines_ = reinterpret_cast<void*>(p_vkCreateGraphicsPipelines);
  proc_destroy_pipeline_ = reinterpret_cast<void*>(p_vkDestroyPipeline);
  proc_create_layout_ = reinterpret_cast<void*>(p_vkCreatePipelineLayout);
  proc_destroy_layout_ = reinterpret_cast<void*>(p_vkDestroyPipelineLayout);
  proc_bind_pipeline_ = reinterpret_cast<void*>(p_vkCmdBindPipeline);
  proc_cmd_viewport_ = reinterpret_cast<void*>(p_vkCmdSetViewport);
  proc_cmd_scissor_ = reinterpret_cast<void*>(p_vkCmdSetScissor);
  proc_cmd_draw_ = reinterpret_cast<void*>(p_vkCmdDraw);
  proc_begin_rendering_ = reinterpret_cast<void*>(p_vkCmdBeginRendering);
  proc_end_rendering_ = reinterpret_cast<void*>(p_vkCmdEndRendering);
  ready_ = true;
  return MakeOk();
}

auto ForwardRenderer::record_triangle(VkCommandBuffer cmd,
                                      VkImageView color_view,
                                      VkExtent2D extent) noexcept -> Status {
  if (!ready_ || pipeline_ == nullptr || pipe_layout_ == nullptr) {
    return MakeFail(StatusCode::kNotInitialized, "forward not built");
  }
  if (cmd == nullptr || color_view == nullptr || extent.width == 0 ||
      extent.height == 0) {
    return MakeFail(StatusCode::kInvalidArgument, "forward bad record arg");
  }
  auto p_vkCmdBindPipeline =
      reinterpret_cast<PFN_vkCmdBindPipeline>(proc_bind_pipeline_);
  auto p_vkCmdSetViewport =
      reinterpret_cast<PFN_vkCmdSetViewport>(proc_cmd_viewport_);
  auto p_vkCmdSetScissor =
      reinterpret_cast<PFN_vkCmdSetScissor>(proc_cmd_scissor_);
  auto p_vkCmdDraw = reinterpret_cast<PFN_vkCmdDraw>(proc_cmd_draw_);
  auto p_vkCmdBeginRendering =
      reinterpret_cast<PFN_vkCmdBeginRendering>(proc_begin_rendering_);
  auto p_vkCmdEndRendering =
      reinterpret_cast<PFN_vkCmdEndRendering>(proc_end_rendering_);
  if (p_vkCmdBindPipeline == nullptr || p_vkCmdSetViewport == nullptr ||
      p_vkCmdSetScissor == nullptr || p_vkCmdDraw == nullptr ||
      p_vkCmdBeginRendering == nullptr || p_vkCmdEndRendering == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "forward no record");
  }
  VkRenderingAttachmentInfo color{};
  color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  color.imageView = color_view;
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.clearValue.color.float32[0] = 0.0F;
  color.clearValue.color.float32[1] = 0.0F;
  color.clearValue.color.float32[2] = 0.0F;
  color.clearValue.color.float32[3] = 1.0F;
  VkRenderingInfo ri{};
  ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  ri.renderArea.offset.x = 0;
  ri.renderArea.offset.y = 0;
  ri.renderArea.extent = extent;
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &color;
  p_vkCmdBeginRendering(cmd, &ri);

  VkViewport vp{};
  vp.x = 0.0F;
  vp.y = 0.0F;
  vp.width = static_cast<float>(extent.width);
  vp.height = static_cast<float>(extent.height);
  vp.minDepth = 0.0F;
  vp.maxDepth = 1.0F;
  p_vkCmdSetViewport(cmd, 0, 1, &vp);
  VkRect2D scissor{};
  scissor.offset.x = 0;
  scissor.offset.y = 0;
  scissor.extent = extent;
  p_vkCmdSetScissor(cmd, 0, 1, &scissor);

  p_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
  p_vkCmdDraw(cmd, 3, 1, 0, 0);
  p_vkCmdEndRendering(cmd);
  return MakeOk();
}

// P1-07: mesh pipeline (mesh.vert/mesh.frag) — indexed draws with a
// 160-byte push constant, D32_SFLOAT depth test, dynamic rendering.
auto ForwardRenderer::create_mesh(
    VulkanDevice& dev, VkFormat color_format, VkFormat depth_format,
    const std::uint32_t* vert_spv, std::size_t vert_words,
    const std::uint32_t* frag_spv, std::size_t frag_words) noexcept
    -> Status {
  if (mesh_pipeline_ != nullptr || mesh_layout_ != nullptr) {
    return MakeFail(StatusCode::kAlreadyInitialized, "mesh built");
  }
  if (depth_format != VK_FORMAT_D32_SFLOAT) {
    return MakeFail(StatusCode::kInvalidArgument, "mesh needs D32 depth");
  }
  static_assert(sizeof(MeshPush) == 160, "mesh push is 160 bytes");
  static_assert(sizeof(MeshVertex) == 32, "mesh vertex is 32 bytes");
  const std::uint32_t* vert =
      (vert_spv != nullptr) ? vert_spv : kMesh_vertSpv;
  const std::size_t vert_n =
      (vert_spv != nullptr) ? vert_words : kMesh_vertSpvWords;
  const std::uint32_t* frag =
      (frag_spv != nullptr) ? frag_spv : kMesh_fragSpv;
  const std::size_t frag_n =
      (frag_spv != nullptr) ? frag_words : kMesh_fragSpvWords;
  if (!valid_spv(vert, vert_n) || !valid_spv(frag, frag_n)) {
    return MakeFail(StatusCode::kInvalidArgument, "mesh bad blob");
  }
  if (color_format != VK_FORMAT_B8G8R8A8_UNORM &&
      color_format != VK_FORMAT_R8G8B8A8_UNORM) {
    return MakeFail(StatusCode::kInvalidArgument, "mesh bad format");
  }
  VkDevice device = dev.device();
  if (device == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "mesh null device");
  }
  auto p_vkCreateShaderModule = reinterpret_cast<PFN_vkCreateShaderModule>(
      dev.instance_proc("vkCreateShaderModule"));
  auto p_vkDestroyShaderModule = reinterpret_cast<PFN_vkDestroyShaderModule>(
      dev.instance_proc("vkDestroyShaderModule"));
  auto p_vkCreateGraphicsPipelines =
      reinterpret_cast<PFN_vkCreateGraphicsPipelines>(
          dev.proc("vkCreateGraphicsPipelines"));
  auto p_vkDestroyPipeline = reinterpret_cast<PFN_vkDestroyPipeline>(
      dev.proc("vkDestroyPipeline"));
  auto p_vkCreatePipelineLayout = reinterpret_cast<PFN_vkCreatePipelineLayout>(
      dev.instance_proc("vkCreatePipelineLayout"));
  auto p_vkDestroyPipelineLayout =
      reinterpret_cast<PFN_vkDestroyPipelineLayout>(
          dev.instance_proc("vkDestroyPipelineLayout"));
  auto p_vkCmdBindPipeline = reinterpret_cast<PFN_vkCmdBindPipeline>(
      dev.proc("vkCmdBindPipeline"));
  auto p_vkCmdSetViewport =
      reinterpret_cast<PFN_vkCmdSetViewport>(dev.proc("vkCmdSetViewport"));
  auto p_vkCmdSetScissor =
      reinterpret_cast<PFN_vkCmdSetScissor>(dev.proc("vkCmdSetScissor"));
  auto p_vkCmdDrawIndexed = reinterpret_cast<PFN_vkCmdDrawIndexed>(
      dev.proc("vkCmdDrawIndexed"));
  auto p_vkCmdBindVertexBuffers = reinterpret_cast<PFN_vkCmdBindVertexBuffers>(
      dev.proc("vkCmdBindVertexBuffers"));
  auto p_vkCmdBindIndexBuffer = reinterpret_cast<PFN_vkCmdBindIndexBuffer>(
      dev.proc("vkCmdBindIndexBuffer"));
  auto p_vkCmdPushConstants = reinterpret_cast<PFN_vkCmdPushConstants>(
      dev.proc("vkCmdPushConstants"));
  auto p_vkCmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(
      dev.proc("vkCmdBeginRendering"));
  auto p_vkCmdEndRendering =
      reinterpret_cast<PFN_vkCmdEndRendering>(dev.proc("vkCmdEndRendering"));
  if (p_vkCreateShaderModule == nullptr || p_vkDestroyShaderModule == nullptr ||
      p_vkCreateGraphicsPipelines == nullptr || p_vkDestroyPipeline == nullptr ||
      p_vkCreatePipelineLayout == nullptr || p_vkDestroyPipelineLayout == nullptr ||
      p_vkCmdBindPipeline == nullptr || p_vkCmdSetViewport == nullptr ||
      p_vkCmdSetScissor == nullptr || p_vkCmdDrawIndexed == nullptr ||
      p_vkCmdBindVertexBuffers == nullptr || p_vkCmdBindIndexBuffer == nullptr ||
      p_vkCmdPushConstants == nullptr || p_vkCmdBeginRendering == nullptr ||
      p_vkCmdEndRendering == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "mesh no entry");
  }
  const std::uint32_t* blobs[2] = {vert, frag};
  const std::size_t words[2] = {vert_n, frag_n};
  for (int i = 0; i < 2; ++i) {
    VkShaderModuleCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sci.codeSize = words[i] * 4;
    sci.pCode = blobs[i];
    if (p_vkCreateShaderModule(device, &sci, nullptr, &mesh_modules_[i]) !=
        VK_SUCCESS) {
      for (int j = 0; j < i; ++j) {
        p_vkDestroyShaderModule(device, mesh_modules_[j], nullptr);
        mesh_modules_[j] = nullptr;
      }
      return MakeFail(StatusCode::kDeviceCreateFailed, "mesh module");
    }
    ++mesh_module_count_;
  }
  VkPipelineLayout mesh_layout = nullptr;
  {
    VkPushConstantRange push{};
    push.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset = 0;
    push.size = static_cast<std::uint32_t>(sizeof(MeshPush));
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    if (p_vkCreatePipelineLayout(device, &pli, nullptr, &mesh_layout) !=
        VK_SUCCESS) {
      for (int j = 0; j < 2; ++j) {
        p_vkDestroyShaderModule(device, mesh_modules_[j], nullptr);
        mesh_modules_[j] = nullptr;
      }
      mesh_module_count_ = 0;
      return MakeFail(StatusCode::kDeviceCreateFailed, "mesh layout");
    }
  }
  VkPipeline pipe = nullptr;
  {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = mesh_modules_[0];
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = mesh_modules_[1];
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = static_cast<std::uint32_t>(sizeof(MeshVertex));
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0].location = 0;  // pos
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset = 0;
    attrs[1].location = 1;  // normal
    attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[1].offset = 12;
    attrs[2].location = 2;  // uv
    attrs[2].binding = 0;
    attrs[2].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[2].offset = 24;
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = 3;
    vertex_input.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    const MeshFixedState fixed = make_mesh_fixed_state();

    VkPipelineViewportStateCreateInfo viewport = fixed.viewport;
    VkPipelineRasterizationStateCreateInfo raster = fixed.raster;
    VkPipelineMultisampleStateCreateInfo msaa = fixed.msaa;
    VkPipelineDepthStencilStateCreateInfo depth = fixed.depth;
    VkPipelineColorBlendAttachmentState blend_att = fixed.blend_att;
    VkPipelineColorBlendStateCreateInfo blend = fixed.blend;
    blend.pAttachments = &blend_att;
    VkPipelineDynamicStateCreateInfo dyn = fixed.dyn;
    VkDynamicState dyn_states[2] = {fixed.dyn_states[0], fixed.dyn_states[1]};
    dyn.pDynamicStates = dyn_states;
    VkPipelineRenderingCreateInfo rendering = fixed.rendering;
    rendering.pColorAttachmentFormats = &color_format;
    rendering.depthAttachmentFormat = depth_format;

    VkGraphicsPipelineCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pci.pNext = &rendering;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vertex_input;
    pci.pInputAssemblyState = &input_assembly;
    pci.pViewportState = &viewport;
    pci.pRasterizationState = &raster;
    pci.pMultisampleState = &msaa;
    pci.pDepthStencilState = &depth;
    pci.pColorBlendState = &blend;
    pci.pDynamicState = &dyn;
    pci.layout = mesh_layout;

    if (p_vkCreateGraphicsPipelines(device, nullptr, 1, &pci, nullptr, &pipe) !=
        VK_SUCCESS) {
      p_vkDestroyPipelineLayout(device, mesh_layout, nullptr);
      for (int j = 0; j < 2; ++j) {
        p_vkDestroyShaderModule(device, mesh_modules_[j], nullptr);
        mesh_modules_[j] = nullptr;
      }
      mesh_module_count_ = 0;
      return MakeFail(StatusCode::kDeviceCreateFailed, "mesh pipe");
    }
  }
  mesh_pipeline_ = pipe;
  mesh_layout_ = mesh_layout;
  if (device_ == nullptr) {
    // First pipeline built on this renderer owns destroy()'s device +
    // entry points (the tri path fills these in create(); whichever path
    // runs first records them so teardown after shutdown stays safe).
    device_ = device;
    proc_create_module_ = reinterpret_cast<void*>(p_vkCreateShaderModule);
    proc_destroy_module_ = reinterpret_cast<void*>(p_vkDestroyShaderModule);
    proc_create_pipelines_ =
        reinterpret_cast<void*>(p_vkCreateGraphicsPipelines);
    proc_destroy_pipeline_ = reinterpret_cast<void*>(p_vkDestroyPipeline);
    proc_create_layout_ = reinterpret_cast<void*>(p_vkCreatePipelineLayout);
    proc_destroy_layout_ = reinterpret_cast<void*>(p_vkDestroyPipelineLayout);
    proc_bind_pipeline_ = reinterpret_cast<void*>(p_vkCmdBindPipeline);
    proc_cmd_viewport_ = reinterpret_cast<void*>(p_vkCmdSetViewport);
    proc_cmd_scissor_ = reinterpret_cast<void*>(p_vkCmdSetScissor);
    proc_begin_rendering_ = reinterpret_cast<void*>(p_vkCmdBeginRendering);
    proc_end_rendering_ = reinterpret_cast<void*>(p_vkCmdEndRendering);
  }
  proc_cmd_draw_indexed_ = reinterpret_cast<void*>(p_vkCmdDrawIndexed);
  proc_cmd_bind_vb_ = reinterpret_cast<void*>(p_vkCmdBindVertexBuffers);
  proc_cmd_bind_ib_ = reinterpret_cast<void*>(p_vkCmdBindIndexBuffer);
  proc_cmd_push_ = reinterpret_cast<void*>(p_vkCmdPushConstants);
  return MakeOk();
}

auto ForwardRenderer::begin_mesh_pass(VkCommandBuffer cmd,
                                      VkImageView color_view,
                                      VkImageView depth_view,
                                      VkExtent2D extent) noexcept -> Status {
  return open_mesh_pass(cmd, color_view, depth_view, extent,
                        VK_ATTACHMENT_LOAD_OP_CLEAR);
}

auto ForwardRenderer::continue_mesh_pass(VkCommandBuffer cmd,
                                         VkImageView color_view,
                                         VkImageView depth_view,
                                         VkExtent2D extent) noexcept
    -> Status {
  return open_mesh_pass(cmd, color_view, depth_view, extent,
                        VK_ATTACHMENT_LOAD_OP_LOAD);
}

auto ForwardRenderer::open_mesh_pass(VkCommandBuffer cmd,
                                     VkImageView color_view,
                                     VkImageView depth_view,
                                     VkExtent2D extent,
                                     VkAttachmentLoadOp load_op) noexcept
    -> Status {
  if (mesh_pipeline_ == nullptr || mesh_layout_ == nullptr) {
    return MakeFail(StatusCode::kNotInitialized, "mesh not built");
  }
  if (cmd == nullptr || color_view == nullptr || depth_view == nullptr ||
      extent.width == 0 || extent.height == 0) {
    return MakeFail(StatusCode::kInvalidArgument, "mesh bad pass arg");
  }
  auto p_vkCmdBeginRendering =
      reinterpret_cast<PFN_vkCmdBeginRendering>(proc_begin_rendering_);
  auto p_vkCmdSetViewport =
      reinterpret_cast<PFN_vkCmdSetViewport>(proc_cmd_viewport_);
  auto p_vkCmdSetScissor =
      reinterpret_cast<PFN_vkCmdSetScissor>(proc_cmd_scissor_);
  auto p_vkCmdBindPipeline =
      reinterpret_cast<PFN_vkCmdBindPipeline>(proc_bind_pipeline_);
  if (p_vkCmdBeginRendering == nullptr || p_vkCmdSetViewport == nullptr ||
      p_vkCmdSetScissor == nullptr || p_vkCmdBindPipeline == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "mesh no pass entry");
  }
  VkRenderingAttachmentInfo color{};
  color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  color.imageView = color_view;
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = load_op;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.clearValue.color.float32[0] = 0.0F;
  color.clearValue.color.float32[1] = 0.0F;
  color.clearValue.color.float32[2] = 0.0F;
  color.clearValue.color.float32[3] = 1.0F;
  VkRenderingAttachmentInfo depth_att{};
  depth_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  depth_att.imageView = depth_view;
  depth_att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  depth_att.loadOp = load_op;
  depth_att.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth_att.clearValue.depthStencil.depth = 1.0F;
  depth_att.clearValue.depthStencil.stencil = 0;
  VkRenderingInfo ri{};
  ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  ri.renderArea.offset.x = 0;
  ri.renderArea.offset.y = 0;
  ri.renderArea.extent = extent;
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &color;
  ri.pDepthAttachment = &depth_att;
  p_vkCmdBeginRendering(cmd, &ri);
  VkViewport vp{};
  vp.x = 0.0F;
  vp.y = 0.0F;
  vp.width = static_cast<float>(extent.width);
  vp.height = static_cast<float>(extent.height);
  vp.minDepth = 0.0F;
  vp.maxDepth = 1.0F;
  p_vkCmdSetViewport(cmd, 0, 1, &vp);
  VkRect2D scissor{};
  scissor.offset.x = 0;
  scissor.offset.y = 0;
  scissor.extent = extent;
  p_vkCmdSetScissor(cmd, 0, 1, &scissor);
  p_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_pipeline_);
  return MakeOk();
}

auto ForwardRenderer::end_mesh_pass(VkCommandBuffer cmd) noexcept -> Status {
  if (mesh_pipeline_ == nullptr || mesh_layout_ == nullptr) {
    return MakeFail(StatusCode::kNotInitialized, "mesh not built");
  }
  if (cmd == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "mesh bad end arg");
  }
  auto p_vkCmdEndRendering =
      reinterpret_cast<PFN_vkCmdEndRendering>(proc_end_rendering_);
  if (p_vkCmdEndRendering == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "mesh no end entry");
  }
  p_vkCmdEndRendering(cmd);
  return MakeOk();
}

auto ForwardRenderer::record_mesh(VkCommandBuffer cmd, const GpuMesh& mesh,
                                  const MeshPush& push) noexcept -> Status {
  if (mesh_pipeline_ == nullptr || mesh_layout_ == nullptr) {
    return MakeFail(StatusCode::kNotInitialized, "mesh not built");
  }
  if (cmd == nullptr || !mesh.ready() || mesh.index_count() == 0 ||
      mesh.vertex_buffer_bits() == 0 || mesh.index_buffer_bits() == 0) {
    return MakeFail(StatusCode::kInvalidArgument, "mesh bad record arg");
  }
  auto p_vkCmdBindVertexBuffers =
      reinterpret_cast<PFN_vkCmdBindVertexBuffers>(proc_cmd_bind_vb_);
  auto p_vkCmdBindIndexBuffer =
      reinterpret_cast<PFN_vkCmdBindIndexBuffer>(proc_cmd_bind_ib_);
  auto p_vkCmdPushConstants =
      reinterpret_cast<PFN_vkCmdPushConstants>(proc_cmd_push_);
  auto p_vkCmdDrawIndexed =
      reinterpret_cast<PFN_vkCmdDrawIndexed>(proc_cmd_draw_indexed_);
  if (p_vkCmdBindVertexBuffers == nullptr || p_vkCmdBindIndexBuffer == nullptr ||
      p_vkCmdPushConstants == nullptr || p_vkCmdDrawIndexed == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "mesh no record entry");
  }
  VkBuffer vb = reinterpret_cast<VkBuffer>(
      static_cast<std::uintptr_t>(mesh.vertex_buffer_bits()));
  VkBuffer ib = reinterpret_cast<VkBuffer>(
      static_cast<std::uintptr_t>(mesh.index_buffer_bits()));
  const VkDeviceSize offsets[1] = {0};
  p_vkCmdBindVertexBuffers(cmd, 0, 1, &vb, offsets);
  p_vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
  p_vkCmdPushConstants(cmd, mesh_layout_,
                     VK_SHADER_STAGE_VERTEX_BIT |
                         VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, static_cast<std::uint32_t>(sizeof(MeshPush)), &push);
  p_vkCmdDrawIndexed(cmd, mesh.index_count(), 1, 0, 0, 0);
  return MakeOk();
}

// P5-02: skinned draw. Binds the skin extra buffer on binding 1 next to
// the position/normal/uv buffer on binding 0, allocates one fresh joint
// STORAGE descriptor from the skinned pool per draw (no draw rewrites a
// set that is already bound or still in flight — CodeRabbit Major on the
// R1 single-set design), pushes the legacy 160-byte MeshPush, and draws
// mesh.index_count() indices.
// The record-time joint_count must match create_mesh_skinned's count;
// joint_count == 0 / > kSkinMaxJoints is rejected. Pool exhaustion
// (kSkinPoolSets draws) fails closed with kOutOfMemory.
auto ForwardRenderer::record_mesh_skinned(VkCommandBuffer cmd,
                                          const GpuMesh& mesh,
                                          std::uint64_t skin_buffer_bits,
                                          const MeshPush& push,
                                          std::uint64_t joint_ubo_bits,
                                          std::uint64_t joint_ubo_bytes,
                                          std::uint32_t joint_count) noexcept
    -> Status {
  if (mesh_skinned_pipeline_ == nullptr || mesh_skinned_layout_ == nullptr) {
    return MakeFail(StatusCode::kNotInitialized, "mesh skinned not built");
  }
  if (cmd == nullptr || !mesh.ready() || mesh.index_count() == 0 ||
      mesh.vertex_buffer_bits() == 0 || mesh.index_buffer_bits() == 0 ||
      skin_buffer_bits == 0) {
    return MakeFail(StatusCode::kInvalidArgument, "mesh skinned bad arg");
  }
  if (joint_count == 0 || joint_count > kSkinMaxJoints ||
      joint_count != mesh_skinned_joints_) {
    return MakeFail(StatusCode::kInvalidArgument, "mesh skinned bad joints");
  }
  if (joint_ubo_bits == 0 ||
      joint_ubo_bytes < static_cast<std::uint64_t>(joint_count) * 64) {
    return MakeFail(StatusCode::kInvalidArgument, "mesh skinned bad ubo");
  }
  auto p_vkCmdBindVertexBuffers =
      reinterpret_cast<PFN_vkCmdBindVertexBuffers>(proc_cmd_bind_vb_);
  auto p_vkCmdBindIndexBuffer =
      reinterpret_cast<PFN_vkCmdBindIndexBuffer>(proc_cmd_bind_ib_);
  auto p_vkCmdPushConstants =
      reinterpret_cast<PFN_vkCmdPushConstants>(proc_cmd_push_);
  auto p_vkCmdDrawIndexed =
      reinterpret_cast<PFN_vkCmdDrawIndexed>(proc_cmd_draw_indexed_);
  auto p_vkCmdBindPipeline =
      reinterpret_cast<PFN_vkCmdBindPipeline>(proc_bind_pipeline_);
  auto p_vkUpdateDescriptorSets =
      reinterpret_cast<PFN_vkUpdateDescriptorSets>(proc_update_ds_);
  auto p_vkCmdBindDescriptorSets =
      reinterpret_cast<PFN_vkCmdBindDescriptorSets>(proc_bind_ds_);
  auto p_vkAllocateDescriptorSets =
      reinterpret_cast<PFN_vkAllocateDescriptorSets>(proc_alloc_ds_);
  if (p_vkCmdBindVertexBuffers == nullptr || p_vkCmdBindIndexBuffer == nullptr ||
      p_vkCmdPushConstants == nullptr || p_vkCmdDrawIndexed == nullptr ||
      p_vkCmdBindPipeline == nullptr ||
      p_vkUpdateDescriptorSets == nullptr ||
      p_vkCmdBindDescriptorSets == nullptr ||
      p_vkAllocateDescriptorSets == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed,
                    "mesh skinned no record entry");
  }
  if (mesh_skinned_set_ == nullptr) {
    return MakeFail(StatusCode::kNotInitialized, "mesh skinned no set");
  }
  static_assert(sizeof(SkinnedVertexExtra) == 32, "skin extra is 32 bytes");
  static_assert(sizeof(MeshPush) == 160, "mesh push is 160 bytes");
  p_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      mesh_skinned_pipeline_);
  VkBuffer vb = reinterpret_cast<VkBuffer>(
      static_cast<std::uintptr_t>(mesh.vertex_buffer_bits()));
  VkBuffer sb = reinterpret_cast<VkBuffer>(
      static_cast<std::uintptr_t>(skin_buffer_bits));
  VkBuffer bufs[2] = {vb, sb};
  const VkDeviceSize offsets[2] = {0, 0};
  p_vkCmdBindVertexBuffers(cmd, 0, 2, bufs, offsets);
  VkBuffer ib = reinterpret_cast<VkBuffer>(
      static_cast<std::uintptr_t>(mesh.index_buffer_bits()));
  p_vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
  // Per-draw set: allocate one fresh storage descriptor from the skinned
  // pool per draw, so a second draw in the same command buffer (or a frame
  // in flight) can never disturb an already-bound set.
  VkDescriptorSetLayout set_layout = reinterpret_cast<VkDescriptorSetLayout>(
      mesh_skinned_set_layout_);
  VkDescriptorPool pool =
      reinterpret_cast<VkDescriptorPool>(mesh_skinned_pool_);
  VkDescriptorSetAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  ai.descriptorPool = pool;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &set_layout;
  VkDescriptorSet draw_set = nullptr;
  if (p_vkAllocateDescriptorSets(device_, &ai, &draw_set) != VK_SUCCESS) {
    return MakeFail(StatusCode::kOutOfMemory, "mesh skinned pool empty");
  }
  VkBuffer ubo = reinterpret_cast<VkBuffer>(
      static_cast<std::uintptr_t>(joint_ubo_bits));
  VkDescriptorBufferInfo bi{};
  bi.buffer = ubo;
  bi.offset = 0;
  bi.range = static_cast<VkDeviceSize>(joint_count) * 64u;
  VkWriteDescriptorSet wr{};
  wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  wr.dstSet = draw_set;
  wr.dstBinding = kSkinBindJointUbo;
  wr.dstArrayElement = 0;
  wr.descriptorCount = 1;
  wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  wr.pBufferInfo = &bi;
  p_vkUpdateDescriptorSets(device_, 1, &wr, 0, nullptr);
  p_vkCmdBindDescriptorSets(
      cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_skinned_layout_,
      kSkinSetJointUbo, 1, &draw_set, 0, nullptr);
  p_vkCmdPushConstants(cmd, mesh_skinned_layout_,
                       VK_SHADER_STAGE_VERTEX_BIT |
                           VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, static_cast<std::uint32_t>(sizeof(MeshPush)), &push);
  p_vkCmdDrawIndexed(cmd, mesh.index_count(), 1, 0, 0, 0);
  return MakeOk();
}

// P5-02: skinned mesh pipeline. Same pass/fixed-function state as
// create_mesh, plus: vertex binding 1 (32-byte SkinnedVertexExtra:
// uvec4 joints @ location 3, vec4 weights @ location 4), a set-0
// joint-palette STORAGE buffer (descriptor set layout + pool owned by the
// renderer; storage/device address space — vertex-stage UBO + dynamic
// per-vertex index miscompiles on MoltenVK), and a 160-byte push range
// (legacy MeshPush, unchanged). No specialization constants: the loop
// guards corrupt indices against SKIN_MAX_JOINTS and the CPU side
// validates joint_count per draw.
// `joint_ubo_bits/bytes` validate the caller's palette buffer
// (first joint_count x 64B must fit); record_mesh_skinned allocates one
// fresh set per draw from the pool, so CPU pose uploads need no rebuild
// and no draw rewrites another draw's set.
auto ForwardRenderer::create_mesh_skinned(
    VulkanDevice& dev, VkFormat color_format, VkFormat depth_format,
    std::uint32_t joint_count, std::uint64_t joint_ubo_bits,
    std::uint64_t joint_ubo_bytes, const std::uint32_t* vert_spv,
    std::size_t vert_words, const std::uint32_t* frag_spv,
    std::size_t frag_words) noexcept -> Status {
  if (mesh_skinned_pipeline_ != nullptr || mesh_skinned_layout_ != nullptr) {
    return MakeFail(StatusCode::kAlreadyInitialized, "mesh skinned built");
  }
  if (joint_count == 0 || joint_count > kSkinMaxJoints) {
    return MakeFail(StatusCode::kInvalidArgument, "mesh skinned bad joints");
  }
  if (joint_ubo_bits == 0 ||
      joint_ubo_bytes < static_cast<std::uint64_t>(joint_count) * 64) {
    return MakeFail(StatusCode::kInvalidArgument, "mesh skinned bad ubo");
  }
  if (depth_format != VK_FORMAT_D32_SFLOAT) {
    return MakeFail(StatusCode::kInvalidArgument, "mesh skinned needs D32");
  }
  static_assert(sizeof(MeshPush) == 160, "mesh push is 160 bytes");
  static_assert(sizeof(SkinnedVertexExtra) == 32, "skin extra is 32 bytes");
  const std::uint32_t* vert =
      (vert_spv != nullptr) ? vert_spv : kMesh_skinned_vertSpv;
  const std::size_t vert_n =
      (vert_spv != nullptr) ? vert_words : kMesh_skinned_vertSpvWords;
  const std::uint32_t* frag =
      (frag_spv != nullptr) ? frag_spv : kMesh_fragSpv;
  const std::size_t frag_n =
      (frag_spv != nullptr) ? frag_words : kMesh_fragSpvWords;
  if (!valid_spv(vert, vert_n) || !valid_spv(frag, frag_n)) {
    return MakeFail(StatusCode::kInvalidArgument, "mesh skinned bad blob");
  }
  if (color_format != VK_FORMAT_B8G8R8A8_UNORM &&
      color_format != VK_FORMAT_R8G8B8A8_UNORM) {
    return MakeFail(StatusCode::kInvalidArgument, "mesh skinned bad format");
  }
  VkDevice device = dev.device();
  if (device == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "mesh skinned null device");
  }
  auto p_vkCreateShaderModule = reinterpret_cast<PFN_vkCreateShaderModule>(
      dev.instance_proc("vkCreateShaderModule"));
  auto p_vkDestroyShaderModule = reinterpret_cast<PFN_vkDestroyShaderModule>(
      dev.instance_proc("vkDestroyShaderModule"));
  auto p_vkCreateGraphicsPipelines =
      reinterpret_cast<PFN_vkCreateGraphicsPipelines>(
          dev.proc("vkCreateGraphicsPipelines"));
  auto p_vkDestroyPipeline = reinterpret_cast<PFN_vkDestroyPipeline>(
      dev.proc("vkDestroyPipeline"));
  auto p_vkCreatePipelineLayout = reinterpret_cast<PFN_vkCreatePipelineLayout>(
      dev.instance_proc("vkCreatePipelineLayout"));
  auto p_vkDestroyPipelineLayout =
      reinterpret_cast<PFN_vkDestroyPipelineLayout>(
          dev.instance_proc("vkDestroyPipelineLayout"));
  auto p_vkCreateDescriptorSetLayout =
      reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(
          dev.instance_proc("vkCreateDescriptorSetLayout"));
  auto p_vkDestroyDescriptorSetLayout =
      reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(
          dev.instance_proc("vkDestroyDescriptorSetLayout"));
  auto p_vkCreateDescriptorPool =
      reinterpret_cast<PFN_vkCreateDescriptorPool>(
          dev.instance_proc("vkCreateDescriptorPool"));
  auto p_vkDestroyDescriptorPool =
      reinterpret_cast<PFN_vkDestroyDescriptorPool>(
          dev.instance_proc("vkDestroyDescriptorPool"));
  auto p_vkAllocateDescriptorSets =
      reinterpret_cast<PFN_vkAllocateDescriptorSets>(
          dev.proc("vkAllocateDescriptorSets"));
  auto p_vkUpdateDescriptorSets = reinterpret_cast<PFN_vkUpdateDescriptorSets>(
      dev.proc("vkUpdateDescriptorSets"));
  auto p_vkCmdBindPipeline = reinterpret_cast<PFN_vkCmdBindPipeline>(
      dev.proc("vkCmdBindPipeline"));
  auto p_vkCmdSetViewport =
      reinterpret_cast<PFN_vkCmdSetViewport>(dev.proc("vkCmdSetViewport"));
  auto p_vkCmdSetScissor =
      reinterpret_cast<PFN_vkCmdSetScissor>(dev.proc("vkCmdSetScissor"));
  auto p_vkCmdDrawIndexed = reinterpret_cast<PFN_vkCmdDrawIndexed>(
      dev.proc("vkCmdDrawIndexed"));
  auto p_vkCmdBindVertexBuffers = reinterpret_cast<PFN_vkCmdBindVertexBuffers>(
      dev.proc("vkCmdBindVertexBuffers"));
  auto p_vkCmdBindIndexBuffer = reinterpret_cast<PFN_vkCmdBindIndexBuffer>(
      dev.proc("vkCmdBindIndexBuffer"));
  auto p_vkCmdPushConstants = reinterpret_cast<PFN_vkCmdPushConstants>(
      dev.proc("vkCmdPushConstants"));
  auto p_vkCmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(
      dev.proc("vkCmdBeginRendering"));
  auto p_vkCmdEndRendering =
      reinterpret_cast<PFN_vkCmdEndRendering>(dev.proc("vkCmdEndRendering"));
  auto p_vkCmdBindDescriptorSets =
      reinterpret_cast<PFN_vkCmdBindDescriptorSets>(
          dev.proc("vkCmdBindDescriptorSets"));
  if (p_vkCreateShaderModule == nullptr || p_vkDestroyShaderModule == nullptr ||
      p_vkCreateGraphicsPipelines == nullptr || p_vkDestroyPipeline == nullptr ||
      p_vkCreatePipelineLayout == nullptr || p_vkDestroyPipelineLayout == nullptr ||
      p_vkCreateDescriptorSetLayout == nullptr ||
      p_vkDestroyDescriptorSetLayout == nullptr ||
      p_vkCreateDescriptorPool == nullptr ||
      p_vkDestroyDescriptorPool == nullptr ||
      p_vkAllocateDescriptorSets == nullptr ||
      p_vkUpdateDescriptorSets == nullptr ||
      p_vkCmdBindDescriptorSets == nullptr ||
      p_vkCmdBindPipeline == nullptr || p_vkCmdSetViewport == nullptr ||
      p_vkCmdSetScissor == nullptr || p_vkCmdDrawIndexed == nullptr ||
      p_vkCmdBindVertexBuffers == nullptr || p_vkCmdBindIndexBuffer == nullptr ||
      p_vkCmdPushConstants == nullptr || p_vkCmdBeginRendering == nullptr ||
      p_vkCmdEndRendering == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed,
                    "mesh skinned no entry");
  }
  const std::uint32_t* blobs[2] = {vert, frag};
  const std::size_t words[2] = {vert_n, frag_n};
  for (int i = 0; i < 2; ++i) {
    VkShaderModuleCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sci.codeSize = words[i] * 4;
    sci.pCode = blobs[i];
    if (p_vkCreateShaderModule(device, &sci, nullptr,
                               &mesh_skinned_modules_[i]) != VK_SUCCESS) {
      for (int j = 0; j < i; ++j) {
        p_vkDestroyShaderModule(device, mesh_skinned_modules_[j], nullptr);
        mesh_skinned_modules_[j] = nullptr;
      }
      return MakeFail(StatusCode::kDeviceCreateFailed, "mesh skinned module");
    }
    ++mesh_skinned_module_count_;
  }
  auto fail_modules = [&]() noexcept -> void {
    for (int j = 0; j < 2; ++j) {
      if (mesh_skinned_modules_[j] != nullptr) {
        p_vkDestroyShaderModule(device, mesh_skinned_modules_[j], nullptr);
        mesh_skinned_modules_[j] = nullptr;
      }
    }
    mesh_skinned_module_count_ = 0;
  };
  // Descriptor set layout: set 0, binding 0 = joint-palette STORAGE
  // buffer, visible to the vertex stage only.
  VkDescriptorSetLayout set_layout = nullptr;
  {
    VkDescriptorSetLayoutBinding bind{};
    bind.binding = kSkinBindJointUbo;
    bind.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bind.descriptorCount = 1;
    bind.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = 1;
    lci.pBindings = &bind;
    if (p_vkCreateDescriptorSetLayout(device, &lci, nullptr, &set_layout) !=
        VK_SUCCESS) {
      fail_modules();
      return MakeFail(StatusCode::kDeviceCreateFailed,
                      "mesh skinned set layout");
    }
  }
  // Per-draw descriptor pool (kSkinPoolSets sets) + one initial set to
  // prove the layout/pool path. Record allocates one fresh STORAGE set
  // per draw (no rewrite of a bound/in-flight set); the joint BUFFER
  // varies per draw so create only needs the layout + one valid initial
  // write.
  void* pool = nullptr;
  void* set = nullptr;
  {
    VkDescriptorPoolSize ps{};
    ps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps.descriptorCount = kSkinPoolSets;
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = kSkinPoolSets;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &ps;
    VkDescriptorPool vk_pool = nullptr;
    if (p_vkCreateDescriptorPool(device, &dpci, nullptr, &vk_pool) !=
        VK_SUCCESS) {
      p_vkDestroyDescriptorSetLayout(
          device, set_layout, nullptr);
      fail_modules();
      return MakeFail(StatusCode::kDeviceCreateFailed, "mesh skinned pool");
    }
    pool = reinterpret_cast<void*>(vk_pool);
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = vk_pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &set_layout;
    VkDescriptorSet vk_set = nullptr;
    if (p_vkAllocateDescriptorSets(device, &ai, &vk_set) != VK_SUCCESS) {
      p_vkDestroyDescriptorPool(device, vk_pool, nullptr);
      p_vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
      fail_modules();
      return MakeFail(StatusCode::kDeviceCreateFailed,
                      "mesh skinned alloc set");
    }
    set = reinterpret_cast<void*>(vk_set);
    VkBuffer ubo = reinterpret_cast<VkBuffer>(
        static_cast<std::uintptr_t>(joint_ubo_bits));
    VkDescriptorBufferInfo bi{};
    bi.buffer = ubo;
    bi.offset = 0;
    bi.range = static_cast<VkDeviceSize>(joint_count) * 64u;
    VkWriteDescriptorSet wr{};
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = vk_set;
    wr.dstBinding = kSkinBindJointUbo;
    wr.dstArrayElement = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wr.pBufferInfo = &bi;
    p_vkUpdateDescriptorSets(device, 1, &wr, 0, nullptr);
  }
  VkPipelineLayout layout = nullptr;
  {
    VkPushConstantRange push{};
    push.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset = 0;
    push.size = static_cast<std::uint32_t>(sizeof(MeshPush));
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    if (p_vkCreatePipelineLayout(device, &pli, nullptr, &layout) !=
        VK_SUCCESS) {
      p_vkDestroyDescriptorPool(
          device, reinterpret_cast<VkDescriptorPool>(pool), nullptr);
      p_vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
      fail_modules();
      return MakeFail(StatusCode::kDeviceCreateFailed, "mesh skinned layout");
    }
  }
  VkPipeline pipe = nullptr;
  {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = mesh_skinned_modules_[0];
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = mesh_skinned_modules_[1];
    stages[1].pName = "main";

    VkVertexInputBindingDescription bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].stride = static_cast<std::uint32_t>(sizeof(MeshVertex));
    bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    bindings[1].binding = 1;
    bindings[1].stride =
        static_cast<std::uint32_t>(sizeof(SkinnedVertexExtra));
    bindings[1].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[5]{};
    attrs[0].location = 0;  // pos
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset = 0;
    attrs[1].location = 1;  // normal
    attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[1].offset = 12;
    attrs[2].location = 2;  // uv
    attrs[2].binding = 0;
    attrs[2].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[2].offset = 24;
    attrs[3].location = 3;  // joints
    attrs[3].binding = 1;
    attrs[3].format = VK_FORMAT_R32G32B32A32_UINT;
    attrs[3].offset = 0;
    attrs[4].location = 4;  // weights
    attrs[4].binding = 1;
    attrs[4].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    attrs[4].offset = 16;
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 2;
    vertex_input.pVertexBindingDescriptions = bindings;
    vertex_input.vertexAttributeDescriptionCount = 5;
    vertex_input.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    const MeshFixedState fixed = make_mesh_fixed_state();

    VkPipelineViewportStateCreateInfo viewport = fixed.viewport;
    VkPipelineRasterizationStateCreateInfo raster = fixed.raster;
    VkPipelineMultisampleStateCreateInfo msaa = fixed.msaa;
    VkPipelineDepthStencilStateCreateInfo depth = fixed.depth;
    VkPipelineColorBlendAttachmentState blend_att = fixed.blend_att;
    VkPipelineColorBlendStateCreateInfo blend = fixed.blend;
    blend.pAttachments = &blend_att;
    VkPipelineDynamicStateCreateInfo dyn = fixed.dyn;
    VkDynamicState dyn_states[2] = {fixed.dyn_states[0], fixed.dyn_states[1]};
    dyn.pDynamicStates = dyn_states;
    VkPipelineRenderingCreateInfo rendering = fixed.rendering;
    rendering.pColorAttachmentFormats = &color_format;
    rendering.depthAttachmentFormat = depth_format;

    VkGraphicsPipelineCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pci.pNext = &rendering;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vertex_input;
    pci.pInputAssemblyState = &input_assembly;
    pci.pViewportState = &viewport;
    pci.pRasterizationState = &raster;
    pci.pMultisampleState = &msaa;
    pci.pDepthStencilState = &depth;
    pci.pColorBlendState = &blend;
    pci.pDynamicState = &dyn;
    pci.layout = layout;

    if (p_vkCreateGraphicsPipelines(device, nullptr, 1, &pci, nullptr, &pipe) !=
        VK_SUCCESS) {
      p_vkDestroyPipelineLayout(device, layout, nullptr);
      p_vkDestroyDescriptorPool(
          device, reinterpret_cast<VkDescriptorPool>(pool), nullptr);
      p_vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
      fail_modules();
      return MakeFail(StatusCode::kDeviceCreateFailed, "mesh skinned pipe");
    }
  }
  mesh_skinned_pipeline_ = pipe;
  mesh_skinned_layout_ = layout;
  mesh_skinned_joints_ = joint_count;
  mesh_skinned_set_layout_ = reinterpret_cast<void*>(set_layout);
  mesh_skinned_pool_ = pool;
  mesh_skinned_set_ = set;
  if (device_ == nullptr) {
    device_ = device;
    proc_create_module_ = reinterpret_cast<void*>(p_vkCreateShaderModule);
    proc_destroy_module_ = reinterpret_cast<void*>(p_vkDestroyShaderModule);
    proc_create_pipelines_ =
        reinterpret_cast<void*>(p_vkCreateGraphicsPipelines);
    proc_destroy_pipeline_ = reinterpret_cast<void*>(p_vkDestroyPipeline);
    proc_create_layout_ = reinterpret_cast<void*>(p_vkCreatePipelineLayout);
    proc_destroy_layout_ = reinterpret_cast<void*>(p_vkDestroyPipelineLayout);
    proc_bind_pipeline_ = reinterpret_cast<void*>(p_vkCmdBindPipeline);
    proc_cmd_viewport_ = reinterpret_cast<void*>(p_vkCmdSetViewport);
    proc_cmd_scissor_ = reinterpret_cast<void*>(p_vkCmdSetScissor);
    proc_begin_rendering_ = reinterpret_cast<void*>(p_vkCmdBeginRendering);
    proc_end_rendering_ = reinterpret_cast<void*>(p_vkCmdEndRendering);
  }
  // Descriptor procs are stored unconditionally: create_mesh/create_pick
  // may run first (same first-builder-wins rule as the other proc slots
  // above would otherwise leave destroy() without the pool/layout
  // entry points). Re-storing identical proc addresses is idempotent.
  proc_create_ds_layout_ =
      reinterpret_cast<void*>(p_vkCreateDescriptorSetLayout);
  proc_destroy_ds_layout_ =
      reinterpret_cast<void*>(p_vkDestroyDescriptorSetLayout);
  proc_create_pool_ = reinterpret_cast<void*>(p_vkCreateDescriptorPool);
  proc_destroy_pool_ = reinterpret_cast<void*>(p_vkDestroyDescriptorPool);
  proc_alloc_ds_ = reinterpret_cast<void*>(p_vkAllocateDescriptorSets);
  proc_cmd_draw_indexed_ = reinterpret_cast<void*>(p_vkCmdDrawIndexed);
  proc_cmd_bind_vb_ = reinterpret_cast<void*>(p_vkCmdBindVertexBuffers);
  proc_cmd_bind_ib_ = reinterpret_cast<void*>(p_vkCmdBindIndexBuffer);
  proc_cmd_push_ = reinterpret_cast<void*>(p_vkCmdPushConstants);
  proc_update_ds_ = reinterpret_cast<void*>(p_vkUpdateDescriptorSets);
  proc_bind_ds_ = reinterpret_cast<void*>(p_vkCmdBindDescriptorSets);
  return MakeOk();
}

// P3-06 pick pipeline + id draw. Mirrors create_mesh() structurally:
// same proc resolution, same module/pipeline/layout build, but the
// R32G32_UINT id target replaces the UNORM color target (no blending on a
// UINT target — blend_att stays default-disabled), depth stays
// D32_SFLOAT (depth test hides back faces so the topmost entity wins the
// pixel), and the vertex input still matches the 32-byte MeshVertex
// (pick.vert consumes pos and ignores normal/uv, like mesh.vert ignores
// uv). render_ids() mirrors render_world()'s query + ColMat camera/model
// math, pushing one PickPush per drawn entity (id split lo/hi).
auto ForwardRenderer::create_pick(
    VulkanDevice& dev, VkFormat id_format, VkFormat depth_format,
    const std::uint32_t* vert_spv, std::size_t vert_words,
    const std::uint32_t* frag_spv, std::size_t frag_words) noexcept
    -> Status {
  if (pick_pipeline_ != nullptr || pick_layout_ != nullptr) {
    return MakeFail(StatusCode::kAlreadyInitialized, "pick built");
  }
  if (depth_format != VK_FORMAT_D32_SFLOAT) {
    return MakeFail(StatusCode::kInvalidArgument, "pick needs D32 depth");
  }
  if (id_format != VK_FORMAT_R32G32_UINT) {
    return MakeFail(StatusCode::kInvalidArgument, "pick needs R32G32_UINT id");
  }
  static_assert(sizeof(PickPush) == 96, "pick push is 96 bytes");
  static_assert(sizeof(MeshVertex) == 32, "pick vertex is 32 bytes");
  const std::uint32_t* vert =
      (vert_spv != nullptr) ? vert_spv : kPick_vertSpv;
  const std::size_t vert_n =
      (vert_spv != nullptr) ? vert_words : kPick_vertSpvWords;
  const std::uint32_t* frag =
      (frag_spv != nullptr) ? frag_spv : kPick_fragSpv;
  const std::size_t frag_n =
      (frag_spv != nullptr) ? frag_words : kPick_fragSpvWords;
  if (!valid_spv(vert, vert_n) || !valid_spv(frag, frag_n)) {
    return MakeFail(StatusCode::kInvalidArgument, "pick bad blob");
  }
  VkDevice device = dev.device();
  if (device == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "pick null device");
  }
  auto p_vkCreateShaderModule = reinterpret_cast<PFN_vkCreateShaderModule>(
      dev.instance_proc("vkCreateShaderModule"));
  auto p_vkDestroyShaderModule = reinterpret_cast<PFN_vkDestroyShaderModule>(
      dev.instance_proc("vkDestroyShaderModule"));
  auto p_vkCreateGraphicsPipelines =
      reinterpret_cast<PFN_vkCreateGraphicsPipelines>(
          dev.proc("vkCreateGraphicsPipelines"));
  auto p_vkDestroyPipeline = reinterpret_cast<PFN_vkDestroyPipeline>(
      dev.proc("vkDestroyPipeline"));
  auto p_vkCreatePipelineLayout = reinterpret_cast<PFN_vkCreatePipelineLayout>(
      dev.instance_proc("vkCreatePipelineLayout"));
  auto p_vkDestroyPipelineLayout =
      reinterpret_cast<PFN_vkDestroyPipelineLayout>(
          dev.instance_proc("vkDestroyPipelineLayout"));
  auto p_vkCmdBindPipeline = reinterpret_cast<PFN_vkCmdBindPipeline>(
      dev.proc("vkCmdBindPipeline"));
  auto p_vkCmdSetViewport =
      reinterpret_cast<PFN_vkCmdSetViewport>(dev.proc("vkCmdSetViewport"));
  auto p_vkCmdSetScissor =
      reinterpret_cast<PFN_vkCmdSetScissor>(dev.proc("vkCmdSetScissor"));
  auto p_vkCmdDrawIndexed = reinterpret_cast<PFN_vkCmdDrawIndexed>(
      dev.proc("vkCmdDrawIndexed"));
  auto p_vkCmdBindVertexBuffers = reinterpret_cast<PFN_vkCmdBindVertexBuffers>(
      dev.proc("vkCmdBindVertexBuffers"));
  auto p_vkCmdBindIndexBuffer = reinterpret_cast<PFN_vkCmdBindIndexBuffer>(
      dev.proc("vkCmdBindIndexBuffer"));
  auto p_vkCmdPushConstants = reinterpret_cast<PFN_vkCmdPushConstants>(
      dev.proc("vkCmdPushConstants"));
  auto p_vkCmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(
      dev.proc("vkCmdBeginRendering"));
  auto p_vkCmdEndRendering =
      reinterpret_cast<PFN_vkCmdEndRendering>(dev.proc("vkCmdEndRendering"));
  if (p_vkCreateShaderModule == nullptr || p_vkDestroyShaderModule == nullptr ||
      p_vkCreateGraphicsPipelines == nullptr || p_vkDestroyPipeline == nullptr ||
      p_vkCreatePipelineLayout == nullptr || p_vkDestroyPipelineLayout == nullptr ||
      p_vkCmdBindPipeline == nullptr || p_vkCmdSetViewport == nullptr ||
      p_vkCmdSetScissor == nullptr || p_vkCmdDrawIndexed == nullptr ||
      p_vkCmdBindVertexBuffers == nullptr || p_vkCmdBindIndexBuffer == nullptr ||
      p_vkCmdPushConstants == nullptr || p_vkCmdBeginRendering == nullptr ||
      p_vkCmdEndRendering == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "pick no entry");
  }
  const std::uint32_t* blobs[2] = {vert, frag};
  const std::size_t words[2] = {vert_n, frag_n};
  for (int i = 0; i < 2; ++i) {
    VkShaderModuleCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sci.codeSize = words[i] * 4;
    sci.pCode = blobs[i];
    if (p_vkCreateShaderModule(device, &sci, nullptr, &pick_modules_[i]) !=
        VK_SUCCESS) {
      for (int j = 0; j < i; ++j) {
        p_vkDestroyShaderModule(device, pick_modules_[j], nullptr);
        pick_modules_[j] = nullptr;
      }
      return MakeFail(StatusCode::kDeviceCreateFailed, "pick module");
    }
    ++pick_module_count_;
  }
  VkPipelineLayout pick_layout = nullptr;
  {
    VkPushConstantRange push{};
    push.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset = 0;
    push.size = static_cast<std::uint32_t>(sizeof(PickPush));
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    if (p_vkCreatePipelineLayout(device, &pli, nullptr, &pick_layout) !=
        VK_SUCCESS) {
      for (int j = 0; j < 2; ++j) {
        p_vkDestroyShaderModule(device, pick_modules_[j], nullptr);
        pick_modules_[j] = nullptr;
      }
      pick_module_count_ = 0;
      return MakeFail(StatusCode::kDeviceCreateFailed, "pick layout");
    }
  }
  VkPipeline pipe = nullptr;
  {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = pick_modules_[0];
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = pick_modules_[1];
    stages[1].pName = "main";

    VkVertexInputBindingDescription binding{};
    binding.binding = 0;
    binding.stride = static_cast<std::uint32_t>(sizeof(MeshVertex));
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    VkVertexInputAttributeDescription attrs[3]{};
    attrs[0].location = 0;  // pos
    attrs[0].binding = 0;
    attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[0].offset = 0;
    attrs[1].location = 1;  // normal (consumed, position only matters)
    attrs[1].binding = 0;
    attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrs[1].offset = 12;
    attrs[2].location = 2;  // uv (consumed)
    attrs[2].binding = 0;
    attrs[2].format = VK_FORMAT_R32G32_SFLOAT;
    attrs[2].offset = 24;
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = 3;
    vertex_input.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0F;

    VkPipelineMultisampleStateCreateInfo msaa{};
    msaa.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;

    // UINT targets cannot blend: blending stays disabled (blendEnable
    // defaults FALSE) with a full R|G write mask so both id lanes land
    // (attachmentCount MUST equal the rendering colorAttachmentCount —
    // VUID-renderPass-06055).
    VkPipelineColorBlendAttachmentState blend_att{};
    blend_att.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_att;

    const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                          VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;

    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &id_format;
    rendering.depthAttachmentFormat = depth_format;

    VkGraphicsPipelineCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pci.pNext = &rendering;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vertex_input;
    pci.pInputAssemblyState = &input_assembly;
    pci.pViewportState = &viewport;
    pci.pRasterizationState = &raster;
    pci.pMultisampleState = &msaa;
    pci.pDepthStencilState = &depth;
    pci.pColorBlendState = &blend;
    pci.pDynamicState = &dyn;
    pci.layout = pick_layout;

    if (p_vkCreateGraphicsPipelines(device, nullptr, 1, &pci, nullptr, &pipe) !=
        VK_SUCCESS) {
      p_vkDestroyPipelineLayout(device, pick_layout, nullptr);
      for (int j = 0; j < 2; ++j) {
        p_vkDestroyShaderModule(device, pick_modules_[j], nullptr);
        pick_modules_[j] = nullptr;
      }
      pick_module_count_ = 0;
      return MakeFail(StatusCode::kDeviceCreateFailed, "pick pipe");
    }
  }
  pick_pipeline_ = pipe;
  pick_layout_ = pick_layout;
  if (device_ == nullptr) {
    // First pipeline built on this renderer owns destroy()'s device +
    // entry points (same rule as create_mesh()).
    device_ = device;
    proc_create_module_ = reinterpret_cast<void*>(p_vkCreateShaderModule);
    proc_destroy_module_ = reinterpret_cast<void*>(p_vkDestroyShaderModule);
    proc_create_pipelines_ =
        reinterpret_cast<void*>(p_vkCreateGraphicsPipelines);
    proc_destroy_pipeline_ = reinterpret_cast<void*>(p_vkDestroyPipeline);
    proc_create_layout_ = reinterpret_cast<void*>(p_vkCreatePipelineLayout);
    proc_destroy_layout_ = reinterpret_cast<void*>(p_vkDestroyPipelineLayout);
    proc_bind_pipeline_ = reinterpret_cast<void*>(p_vkCmdBindPipeline);
    proc_cmd_viewport_ = reinterpret_cast<void*>(p_vkCmdSetViewport);
    proc_cmd_scissor_ = reinterpret_cast<void*>(p_vkCmdSetScissor);
    proc_begin_rendering_ = reinterpret_cast<void*>(p_vkCmdBeginRendering);
    proc_end_rendering_ = reinterpret_cast<void*>(p_vkCmdEndRendering);
  }
  proc_cmd_draw_indexed_ = reinterpret_cast<void*>(p_vkCmdDrawIndexed);
  proc_cmd_bind_vb_ = reinterpret_cast<void*>(p_vkCmdBindVertexBuffers);
  proc_cmd_bind_ib_ = reinterpret_cast<void*>(p_vkCmdBindIndexBuffer);
  proc_cmd_push_ = reinterpret_cast<void*>(p_vkCmdPushConstants);
  return MakeOk();
}

auto ForwardRenderer::record_pick(VkCommandBuffer cmd, const GpuMesh& mesh,
                                  const PickPush& push) noexcept -> Status {
  if (pick_pipeline_ == nullptr || pick_layout_ == nullptr) {
    return MakeFail(StatusCode::kNotInitialized, "pick not built");
  }
  if (cmd == nullptr || !mesh.ready() || mesh.index_count() == 0 ||
      mesh.vertex_buffer_bits() == 0 || mesh.index_buffer_bits() == 0) {
    return MakeFail(StatusCode::kInvalidArgument, "pick bad record arg");
  }
  auto p_vkCmdBindVertexBuffers =
      reinterpret_cast<PFN_vkCmdBindVertexBuffers>(proc_cmd_bind_vb_);
  auto p_vkCmdBindIndexBuffer =
      reinterpret_cast<PFN_vkCmdBindIndexBuffer>(proc_cmd_bind_ib_);
  auto p_vkCmdPushConstants =
      reinterpret_cast<PFN_vkCmdPushConstants>(proc_cmd_push_);
  auto p_vkCmdDrawIndexed =
      reinterpret_cast<PFN_vkCmdDrawIndexed>(proc_cmd_draw_indexed_);
  if (p_vkCmdBindVertexBuffers == nullptr || p_vkCmdBindIndexBuffer == nullptr ||
      p_vkCmdPushConstants == nullptr || p_vkCmdDrawIndexed == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "pick no record entry");
  }
  VkBuffer vb = reinterpret_cast<VkBuffer>(
      static_cast<std::uintptr_t>(mesh.vertex_buffer_bits()));
  VkBuffer ib = reinterpret_cast<VkBuffer>(
      static_cast<std::uintptr_t>(mesh.index_buffer_bits()));
  const VkDeviceSize offsets[1] = {0};
  p_vkCmdBindVertexBuffers(cmd, 0, 1, &vb, offsets);
  p_vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
  p_vkCmdPushConstants(cmd, pick_layout_,
                     VK_SHADER_STAGE_VERTEX_BIT |
                         VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, static_cast<std::uint32_t>(sizeof(PickPush)), &push);
  p_vkCmdDrawIndexed(cmd, mesh.index_count(), 1, 0, 0, 0);
  return MakeOk();
}

auto ForwardRenderer::begin_pick_pass(VkCommandBuffer cmd,
                                      VkImageView id_view,
                                      VkImageView depth_view,
                                      VkExtent2D extent) noexcept -> Status {
  if (pick_pipeline_ == nullptr || pick_layout_ == nullptr) {
    return MakeFail(StatusCode::kNotInitialized, "pick not built");
  }
  if (cmd == nullptr || id_view == nullptr || depth_view == nullptr ||
      extent.width == 0 || extent.height == 0) {
    return MakeFail(StatusCode::kInvalidArgument, "pick bad pass arg");
  }
  auto p_vkCmdBeginRendering =
      reinterpret_cast<PFN_vkCmdBeginRendering>(proc_begin_rendering_);
  auto p_vkCmdSetViewport =
      reinterpret_cast<PFN_vkCmdSetViewport>(proc_cmd_viewport_);
  auto p_vkCmdSetScissor =
      reinterpret_cast<PFN_vkCmdSetScissor>(proc_cmd_scissor_);
  auto p_vkCmdBindPipeline =
      reinterpret_cast<PFN_vkCmdBindPipeline>(proc_bind_pipeline_);
  if (p_vkCmdBeginRendering == nullptr || p_vkCmdSetViewport == nullptr ||
      p_vkCmdSetScissor == nullptr || p_vkCmdBindPipeline == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "pick no pass entry");
  }
  VkRenderingAttachmentInfo id_att{};
  id_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  id_att.imageView = id_view;
  id_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  id_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  id_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  id_att.clearValue.color.uint32[0] = 0;
  id_att.clearValue.color.uint32[1] = 0;
  id_att.clearValue.color.uint32[2] = 0;
  id_att.clearValue.color.uint32[3] = 0;
  VkRenderingAttachmentInfo depth_att{};
  depth_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  depth_att.imageView = depth_view;
  depth_att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  depth_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  depth_att.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  depth_att.clearValue.depthStencil.depth = 1.0F;
  depth_att.clearValue.depthStencil.stencil = 0;
  VkRenderingInfo ri{};
  ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  ri.renderArea.offset.x = 0;
  ri.renderArea.offset.y = 0;
  ri.renderArea.extent = extent;
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &id_att;
  ri.pDepthAttachment = &depth_att;
  p_vkCmdBeginRendering(cmd, &ri);
  VkViewport vp{};
  vp.x = 0.0F;
  vp.y = 0.0F;
  vp.width = static_cast<float>(extent.width);
  vp.height = static_cast<float>(extent.height);
  vp.minDepth = 0.0F;
  vp.maxDepth = 1.0F;
  p_vkCmdSetViewport(cmd, 0, 1, &vp);
  VkRect2D scissor{};
  scissor.offset.x = 0;
  scissor.offset.y = 0;
  scissor.extent = extent;
  p_vkCmdSetScissor(cmd, 0, 1, &scissor);
  p_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pick_pipeline_);
  return MakeOk();
}

auto ForwardRenderer::end_pick_pass(VkCommandBuffer cmd) noexcept -> Status {
  if (pick_pipeline_ == nullptr || pick_layout_ == nullptr) {
    return MakeFail(StatusCode::kNotInitialized, "pick not built");
  }
  if (cmd == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "pick bad end arg");
  }
  auto p_vkCmdEndRendering =
      reinterpret_cast<PFN_vkCmdEndRendering>(proc_end_rendering_);
  if (p_vkCmdEndRendering == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "pick no end entry");
  }
  p_vkCmdEndRendering(cmd);
  return MakeOk();
}

auto ForwardRenderer::destroy() noexcept -> void {
  // Reference the create-time-only procs so Clang -Wunused-private-field
  // stays clean under -Werror (same pattern as GiFrameGraph).
  (void)proc_create_module_;
  (void)proc_create_pipelines_;
  (void)proc_create_layout_;
  (void)proc_create_ds_layout_;
  (void)proc_destroy_ds_layout_;
  (void)proc_create_pool_;
  (void)proc_destroy_pool_;
  (void)proc_alloc_ds_;
  (void)proc_update_ds_;
  (void)proc_bind_ds_;
  (void)proc_bind_pipeline_;
  (void)proc_cmd_viewport_;
  (void)proc_cmd_scissor_;
  (void)proc_cmd_draw_;
  (void)proc_cmd_draw_indexed_;
  (void)proc_cmd_bind_vb_;
  (void)proc_cmd_bind_ib_;
  (void)proc_cmd_push_;
  (void)proc_begin_rendering_;
  (void)proc_end_rendering_;
  (void)proc_limits_;
  if (device_ == nullptr) {
    // Never created, or the device is already gone (same rule as
    // GiFrameGraph: never vkDestroy* on a dead device — the owning
    // VulkanDevice::shutdown() reclaims driver objects out from under
    // every live handle; see M3-02 Lavapipe exit segfault).
    modules_[0] = modules_[1] = nullptr;
    pipeline_ = nullptr;
    pipe_layout_ = nullptr;
    mesh_modules_[0] = mesh_modules_[1] = nullptr;
    mesh_pipeline_ = nullptr;
    mesh_layout_ = nullptr;
    module_count_ = 0;
    mesh_module_count_ = 0;
    mesh_skinned_modules_[0] = mesh_skinned_modules_[1] = nullptr;
    mesh_skinned_pipeline_ = nullptr;
    mesh_skinned_layout_ = nullptr;
    mesh_skinned_module_count_ = 0;
    mesh_skinned_joints_ = 0;
    mesh_skinned_set_layout_ = nullptr;
    mesh_skinned_pool_ = nullptr;
    mesh_skinned_set_ = nullptr;
    pick_modules_[0] = pick_modules_[1] = nullptr;
    pick_pipeline_ = nullptr;
    pick_layout_ = nullptr;
    pick_module_count_ = 0;
    ready_ = false;
    device_ = nullptr;
    return;
  }
  auto p_vkDestroyShaderModule = reinterpret_cast<PFN_vkDestroyShaderModule>(
      proc_destroy_module_);
  auto p_vkDestroyPipeline =
      reinterpret_cast<PFN_vkDestroyPipeline>(proc_destroy_pipeline_);
  auto p_vkDestroyPipelineLayout =
      reinterpret_cast<PFN_vkDestroyPipelineLayout>(proc_destroy_layout_);
  auto p_vkDestroyDescriptorSetLayout =
      reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(
          proc_destroy_ds_layout_);
  auto p_vkDestroyDescriptorPool =
      reinterpret_cast<PFN_vkDestroyDescriptorPool>(proc_destroy_pool_);
  if (p_vkDestroyShaderModule != nullptr) {
    for (int i = 0; i < 2; ++i) {
      if (modules_[i] != nullptr) {
        p_vkDestroyShaderModule(device_, modules_[i], nullptr);
        modules_[i] = nullptr;
      }
    }
  }
  if (p_vkDestroyPipeline != nullptr && pipeline_ != nullptr) {
    p_vkDestroyPipeline(device_, pipeline_, nullptr);
    pipeline_ = nullptr;
  }
  if (p_vkDestroyPipelineLayout != nullptr && pipe_layout_ != nullptr) {
    p_vkDestroyPipelineLayout(device_, pipe_layout_, nullptr);
    pipe_layout_ = nullptr;
  }
  if (p_vkDestroyShaderModule != nullptr) {
    for (int i = 0; i < 2; ++i) {
      if (mesh_modules_[i] != nullptr) {
        p_vkDestroyShaderModule(device_, mesh_modules_[i], nullptr);
        mesh_modules_[i] = nullptr;
      }
    }
  }
  if (p_vkDestroyPipeline != nullptr && mesh_pipeline_ != nullptr) {
    p_vkDestroyPipeline(device_, mesh_pipeline_, nullptr);
    mesh_pipeline_ = nullptr;
  }
  if (p_vkDestroyPipelineLayout != nullptr && mesh_layout_ != nullptr) {
    p_vkDestroyPipelineLayout(device_, mesh_layout_, nullptr);
    mesh_layout_ = nullptr;
  }
  // P5-02 skinned pipeline teardown (same dead-device rule as above).
  if (p_vkDestroyShaderModule != nullptr) {
    for (int i = 0; i < 2; ++i) {
      if (mesh_skinned_modules_[i] != nullptr) {
        p_vkDestroyShaderModule(device_, mesh_skinned_modules_[i], nullptr);
        mesh_skinned_modules_[i] = nullptr;
      }
    }
  }
  if (p_vkDestroyPipeline != nullptr && mesh_skinned_pipeline_ != nullptr) {
    p_vkDestroyPipeline(device_, mesh_skinned_pipeline_, nullptr);
    mesh_skinned_pipeline_ = nullptr;
  }
  if (p_vkDestroyPipelineLayout != nullptr &&
      mesh_skinned_layout_ != nullptr) {
    p_vkDestroyPipelineLayout(device_, mesh_skinned_layout_, nullptr);
    mesh_skinned_layout_ = nullptr;
  }
  if (p_vkDestroyDescriptorPool != nullptr && mesh_skinned_pool_ != nullptr) {
    p_vkDestroyDescriptorPool(
        device_,
        reinterpret_cast<VkDescriptorPool>(mesh_skinned_pool_), nullptr);
    mesh_skinned_pool_ = nullptr;
    mesh_skinned_set_ = nullptr;
  }
  if (p_vkDestroyDescriptorSetLayout != nullptr &&
      mesh_skinned_set_layout_ != nullptr) {
    p_vkDestroyDescriptorSetLayout(
        device_,
        reinterpret_cast<VkDescriptorSetLayout>(mesh_skinned_set_layout_),
        nullptr);
    mesh_skinned_set_layout_ = nullptr;
  }
  // P3-06 pick pipeline teardown (same dead-device rule as above).
  if (p_vkDestroyShaderModule != nullptr) {
    for (int i = 0; i < 2; ++i) {
      if (pick_modules_[i] != nullptr) {
        p_vkDestroyShaderModule(device_, pick_modules_[i], nullptr);
        pick_modules_[i] = nullptr;
      }
    }
  }
  if (p_vkDestroyPipeline != nullptr && pick_pipeline_ != nullptr) {
    p_vkDestroyPipeline(device_, pick_pipeline_, nullptr);
    pick_pipeline_ = nullptr;
  }
  if (p_vkDestroyPipelineLayout != nullptr && pick_layout_ != nullptr) {
    p_vkDestroyPipelineLayout(device_, pick_layout_, nullptr);
    pick_layout_ = nullptr;
  }
  module_count_ = 0;
  mesh_module_count_ = 0;
  mesh_skinned_module_count_ = 0;
  mesh_skinned_joints_ = 0;
  mesh_skinned_set_layout_ = nullptr;
  mesh_skinned_pool_ = nullptr;
  mesh_skinned_set_ = nullptr;
  pick_module_count_ = 0;
  ready_ = false;
  device_ = nullptr;
}

#else

auto ForwardRenderer::create(VulkanDevice& /*dev*/, VkFormat /*color_format*/,
                             VkFormat /*depth_format*/,
                             const std::uint32_t* /*vert_spv*/,
                             std::size_t /*vert_words*/,
                             const std::uint32_t* /*frag_spv*/,
                             std::size_t /*frag_words*/) noexcept -> Status {
  return MakeFail(StatusCode::kVulkanLoaderFailed, "no Vulkan SDK");
}

auto ForwardRenderer::record_triangle(VkCommandBuffer /*cmd*/,
                                      VkImageView /*color_view*/,
                                      VkExtent2D /*extent*/) noexcept
    -> Status {
  return MakeFail(StatusCode::kNotInitialized, "no Vulkan SDK");
}

auto ForwardRenderer::create_mesh(VulkanDevice& /*dev*/,
                                  VkFormat /*color_format*/,
                                  VkFormat /*depth_format*/,
                                  const std::uint32_t* /*vert_spv*/,
                                  std::size_t /*vert_words*/,
                                  const std::uint32_t* /*frag_spv*/,
                                  std::size_t /*frag_words*/) noexcept
    -> Status {
  return MakeFail(StatusCode::kVulkanLoaderFailed, "no Vulkan SDK");
}

auto ForwardRenderer::record_mesh(VkCommandBuffer /*cmd*/,
                                  const GpuMesh& /*mesh*/,
                                  const MeshPush& /*push*/) noexcept
    -> Status {
  return MakeFail(StatusCode::kNotInitialized, "no Vulkan SDK");
}

auto ForwardRenderer::create_mesh_skinned(VulkanDevice& /*dev*/,
                                          VkFormat /*color_format*/,
                                          VkFormat /*depth_format*/,
                                          std::uint32_t /*joint_count*/,
                                          std::uint64_t /*joint_ubo_bits*/,
                                          std::uint64_t /*joint_ubo_bytes*/,
                                          const std::uint32_t* /*vert_spv*/,
                                          std::size_t /*vert_words*/,
                                          const std::uint32_t* /*frag_spv*/,
                                          std::size_t /*frag_words*/) noexcept
    -> Status {
  return MakeFail(StatusCode::kVulkanLoaderFailed, "no Vulkan SDK");
}

auto ForwardRenderer::record_mesh_skinned(VkCommandBuffer /*cmd*/,
                                          const GpuMesh& /*mesh*/,
                                          std::uint64_t /*skin_bits*/,
                                          const MeshPush& /*push*/,
                                          std::uint64_t /*joint_ubo_bits*/,
                                          std::uint64_t /*joint_ubo_bytes*/,
                                          std::uint32_t /*joint_count*/) noexcept
    -> Status {
  return MakeFail(StatusCode::kNotInitialized, "no Vulkan SDK");
}

auto ForwardRenderer::begin_mesh_pass(VkCommandBuffer /*cmd*/,
                                      VkImageView /*color_view*/,
                                      VkImageView /*depth_view*/,
                                      VkExtent2D /*extent*/) noexcept
    -> Status {
  return MakeFail(StatusCode::kNotInitialized, "no Vulkan SDK");
}

auto ForwardRenderer::continue_mesh_pass(VkCommandBuffer /*cmd*/,
                                         VkImageView /*color_view*/,
                                         VkImageView /*depth_view*/,
                                         VkExtent2D /*extent*/) noexcept
    -> Status {
  return MakeFail(StatusCode::kNotInitialized, "no Vulkan SDK");
}

auto ForwardRenderer::end_mesh_pass(VkCommandBuffer /*cmd*/) noexcept
    -> Status {
  return MakeFail(StatusCode::kNotInitialized, "no Vulkan SDK");
}

auto ForwardRenderer::destroy() noexcept -> void {
  modules_[0] = modules_[1] = nullptr;
  pipeline_ = nullptr;
  pipe_layout_ = nullptr;
  mesh_modules_[0] = mesh_modules_[1] = nullptr;
  mesh_pipeline_ = nullptr;
  mesh_layout_ = nullptr;
  mesh_module_count_ = 0;
  mesh_skinned_modules_[0] = mesh_skinned_modules_[1] = nullptr;
  mesh_skinned_pipeline_ = nullptr;
  mesh_skinned_layout_ = nullptr;
  mesh_skinned_module_count_ = 0;
  mesh_skinned_joints_ = 0;
  mesh_skinned_set_layout_ = nullptr;
  mesh_skinned_pool_ = nullptr;
  mesh_skinned_set_ = nullptr;
  pick_modules_[0] = pick_modules_[1] = nullptr;
  pick_pipeline_ = nullptr;
  pick_layout_ = nullptr;
  pick_module_count_ = 0;
  module_count_ = 0;
  ready_ = false;
  }

  auto ForwardRenderer::render_world(void* /*world*/,
                                   const MeshRegistry& /*registry*/,
                                   VkCommandBuffer /*cmd*/,
                                   RenderWorldView* out_view,
                                   RenderWorldStats* out_stats,
                                   float /*aspect*/) noexcept
    -> Status {
  if (out_stats != nullptr) {
    *out_stats = RenderWorldStats{};
  }
  if (out_view != nullptr) {
    *out_view = RenderWorldView{};
  }
  return MakeFail(StatusCode::kNotInitialized, "no Vulkan SDK");
}

auto ForwardRenderer::create_pick(VulkanDevice& /*dev*/,
                                  VkFormat /*id_format*/,
                                  VkFormat /*depth_format*/,
                                  const std::uint32_t* /*vert_spv*/,
                                  std::size_t /*vert_words*/,
                                  const std::uint32_t* /*frag_spv*/,
                                  std::size_t /*frag_words*/) noexcept
    -> Status {
  return MakeFail(StatusCode::kVulkanLoaderFailed, "no Vulkan SDK");
}

auto ForwardRenderer::record_pick(VkCommandBuffer /*cmd*/,
                                  const GpuMesh& /*mesh*/,
                                  const PickPush& /*push*/) noexcept
    -> Status {
  return MakeFail(StatusCode::kNotInitialized, "no Vulkan SDK");
}

auto ForwardRenderer::begin_pick_pass(VkCommandBuffer /*cmd*/,
                                      VkImageView /*id_view*/,
                                      VkImageView /*depth_view*/,
                                      VkExtent2D /*extent*/) noexcept
    -> Status {
  return MakeFail(StatusCode::kNotInitialized, "no Vulkan SDK");
}

auto ForwardRenderer::end_pick_pass(VkCommandBuffer /*cmd*/) noexcept
    -> Status {
  return MakeFail(StatusCode::kNotInitialized, "no Vulkan SDK");
}

auto ForwardRenderer::render_ids(void* /*world*/,
                                 const MeshRegistry& /*registry*/,
                                 VkCommandBuffer /*cmd*/,
                                 VkExtent2D /*extent*/,
                                 RenderWorldStats* out_stats,
                                 float /*aspect*/,
                                 const float* /*view_proj_override*/) noexcept
    -> Status {
  if (out_stats != nullptr) {
    *out_stats = RenderWorldStats{};
  }
  return MakeFail(StatusCode::kNotInitialized, "no Vulkan SDK");
}

#endif

}  // namespace engine::renderer

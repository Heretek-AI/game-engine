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
#include "engine/ecs/ecs_render_components.hpp"
#include "engine/ecs/transform.hpp"
#include "flecs.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace engine::renderer {

ForwardRenderer::~ForwardRenderer() noexcept { destroy(); }

#if defined(ENGINE_RENDERER_HAS_VULKAN)

namespace {

auto valid_spv(const std::uint32_t* w, std::size_t n) noexcept -> bool {
  return w != nullptr && n > 5 && w[0] == 0x07230203u && w[1] >= 0x00010300u;
}

// IEEE-754 binary32 -> binary16 (round-to-nearest-even lite with mantissa
// carry propagation — same contract as the part-1 Cornell test baker).
// Pure integer ops; used to pack baked probes into the RGBA16F volume.
auto FloatToHalf(float f) noexcept -> std::uint16_t {
  std::uint32_t u = 0;
  static_assert(sizeof(u) == sizeof(f), "u32/f32 size");
  std::memcpy(&u, &f, sizeof(u));
  const std::uint32_t sign = (u >> 16) & 0x8000u;
  const std::uint32_t exp = (u >> 23) & 0xffu;
  const std::uint32_t mant = u & 0x7fffffu;
  if (exp == 0xffu) {
    return static_cast<std::uint16_t>(sign | 0x7e00u);
  }
  const int new_exp = static_cast<int>(exp) - 127 + 15;
  if (new_exp >= 31) {
    return static_cast<std::uint16_t>(sign | 0x7bffu);
  }
  if (new_exp <= 0) {
    return static_cast<std::uint16_t>(sign);
  }
  const std::uint32_t half_mant = mant >> 13;
  const std::uint32_t round_bit = (mant >> 12) & 1u;
  const std::uint32_t rest = mant & 0xfffu;
  const bool round_up = (round_bit & (rest | (half_mant & 1u))) != 0u;
  std::uint32_t out_mant = half_mant + (round_up ? 1u : 0u);
  std::uint32_t out_exp = static_cast<std::uint32_t>(new_exp);
  if (out_mant > 0x3ffu) {
    out_mant = 0u;
    ++out_exp;
    if (out_exp >= 31) {
      return static_cast<std::uint16_t>(sign | 0x7bffu);
    }
  }
  return static_cast<std::uint16_t>(sign | (out_exp << 10) | out_mant);
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
  // Descriptor-set destroyer at function scope: the pipeline-failure
  // cleanups below run outside the layout block that creates the set
  // layout (declared here so all probe_layout exits share it).
  auto p_vkDestroySetLayoutAtTop =
      reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(
          dev.proc("vkDestroyDescriptorSetLayout"));
  if (p_vkDestroySetLayoutAtTop == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "mesh no set entry");
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
  VkDescriptorSetLayout probe_layout = nullptr;
  {
    auto p_vkCreateSetLayout =
        reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(
            dev.proc("vkCreateDescriptorSetLayout"));
    if (p_vkCreateSetLayout == nullptr ||
        p_vkDestroySetLayoutAtTop == nullptr) {
      for (int j = 0; j < 2; ++j) {
        p_vkDestroyShaderModule(device, mesh_modules_[j], nullptr);
        mesh_modules_[j] = nullptr;
      }
      mesh_module_count_ = 0;
      return MakeFail(StatusCode::kVulkanLoaderFailed, "mesh no set entry");
    }
    // P4-07 part 2 GI set 0: binding 0 = probe volume (combined, frag),
    // binding 1 = probe box UBO (frag). Built before the pipe layout.
    VkDescriptorSetLayoutBinding set_binds[2]{};
    set_binds[0].binding = 0;
    set_binds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    set_binds[0].descriptorCount = 1;
    set_binds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    set_binds[1].binding = 1;
    set_binds[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    set_binds[1].descriptorCount = 1;
    set_binds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo set_info{};
    set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_info.bindingCount = 2;
    set_info.pBindings = set_binds;
    if (p_vkCreateSetLayout(device, &set_info, nullptr, &probe_layout) !=
        VK_SUCCESS) {
      for (int j = 0; j < 2; ++j) {
        p_vkDestroyShaderModule(device, mesh_modules_[j], nullptr);
        mesh_modules_[j] = nullptr;
      }
      mesh_module_count_ = 0;
      return MakeFail(StatusCode::kDeviceCreateFailed, "mesh probe layout");
    }
    VkPushConstantRange push{};
    push.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset = 0;
    push.size = static_cast<std::uint32_t>(sizeof(MeshPush));
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &probe_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    if (p_vkCreatePipelineLayout(device, &pli, nullptr, &mesh_layout) !=
        VK_SUCCESS) {
      p_vkDestroySetLayoutAtTop(device, probe_layout, nullptr);
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
      p_vkDestroySetLayoutAtTop(device, probe_layout, nullptr);
      for (int j = 0; j < 2; ++j) {
        p_vkDestroyShaderModule(device, mesh_modules_[j], nullptr);
        mesh_modules_[j] = nullptr;
      }
      mesh_module_count_ = 0;
      return MakeFail(StatusCode::kDeviceCreateFailed, "mesh pipe");
    }
  }
  // P4-07 part 2 GI probe resources (image/view/sampler/pool/set + box
  // UBO + staging). Written once here; render_world uploads per frame.
  {
    probe_set_layout_ = probe_layout;
    const Status pst = create_probe_resources(dev, device);
    if (!pst.ok()) {
      p_vkDestroyPipeline(device, pipe, nullptr);
      p_vkDestroyPipelineLayout(device, mesh_layout, nullptr);
      p_vkDestroySetLayoutAtTop(device, probe_layout, nullptr);
      probe_set_layout_ = nullptr;
      for (int j = 0; j < 2; ++j) {
        p_vkDestroyShaderModule(device, mesh_modules_[j], nullptr);
        mesh_modules_[j] = nullptr;
      }
      mesh_module_count_ = 0;
      return pst;
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
  proc_bind_sets_ =
      reinterpret_cast<void*>(dev.proc("vkCmdBindDescriptorSets"));
  return MakeOk();
}

// P4-07 part 2 GI probe volume resources (fixed 8^3 RGBA16F + 32B box
// UBO + linear sampler + one descriptor set). The image starts UNDEFINED
// and is uploaded per frame by render_world; the box UBO starts zeroed
// (gain 0 = GI off, bit-identical frame).
inline constexpr std::uint32_t kGiProbeRes = 8;
inline constexpr std::uint64_t kGiProbeBytes = 8u * 8u * 8u * 8u;
inline constexpr std::uint32_t kGiBoxBytes = 32;

auto ForwardRenderer::create_probe_resources(const VulkanDevice& dev,
                                             VkDevice device) noexcept
    -> Status {
  auto p_vkCreateImage = reinterpret_cast<PFN_vkCreateImage>(
      dev.proc("vkCreateImage"));
  auto p_vkGetImageReqs = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(
      dev.proc("vkGetImageMemoryRequirements"));
  auto p_vkGetMemProps =
      reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
          dev.instance_proc("vkGetPhysicalDeviceMemoryProperties"));
  auto p_vkAllocMem = reinterpret_cast<PFN_vkAllocateMemory>(
      dev.proc("vkAllocateMemory"));
  auto p_vkBindImageMem = reinterpret_cast<PFN_vkBindImageMemory>(
      dev.proc("vkBindImageMemory"));
  auto p_vkCreateView = reinterpret_cast<PFN_vkCreateImageView>(
      dev.proc("vkCreateImageView"));
  auto p_vkCreateSampler = reinterpret_cast<PFN_vkCreateSampler>(
      dev.proc("vkCreateSampler"));
  auto p_vkCreateBuffer = reinterpret_cast<PFN_vkCreateBuffer>(
      dev.proc("vkCreateBuffer"));
  auto p_vkGetBufferReqs =
      reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(
          dev.proc("vkGetBufferMemoryRequirements"));
  auto p_vkBindBufferMem = reinterpret_cast<PFN_vkBindBufferMemory>(
      dev.proc("vkBindBufferMemory"));
  auto p_vkMapMem = reinterpret_cast<PFN_vkMapMemory>(dev.proc("vkMapMemory"));
  auto p_vkUnmapMem =
      reinterpret_cast<PFN_vkUnmapMemory>(dev.proc("vkUnmapMemory"));
  auto p_vkCreatePool = reinterpret_cast<PFN_vkCreateDescriptorPool>(
      dev.proc("vkCreateDescriptorPool"));
  auto p_vkAllocSets = reinterpret_cast<PFN_vkAllocateDescriptorSets>(
      dev.proc("vkAllocateDescriptorSets"));
  auto p_vkUpdateSets = reinterpret_cast<PFN_vkUpdateDescriptorSets>(
      dev.proc("vkUpdateDescriptorSets"));
  if (p_vkCreateImage == nullptr || p_vkGetImageReqs == nullptr ||
      p_vkGetMemProps == nullptr || p_vkAllocMem == nullptr ||
      p_vkBindImageMem == nullptr || p_vkCreateView == nullptr ||
      p_vkCreateSampler == nullptr || p_vkCreateBuffer == nullptr ||
      p_vkGetBufferReqs == nullptr || p_vkBindBufferMem == nullptr ||
      p_vkMapMem == nullptr || p_vkUnmapMem == nullptr ||
      p_vkCreatePool == nullptr || p_vkAllocSets == nullptr ||
      p_vkUpdateSets == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "probe no entry");
  }
  auto fail = [&] { destroy_probe_resources(); };
  // 3D probe image, device-local.
  {
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_3D;
    ii.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    ii.extent = {kGiProbeRes, kGiProbeRes, kGiProbeRes};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (p_vkCreateImage(device, &ii, nullptr, &probe_image_) != VK_SUCCESS) {
      return MakeFail(StatusCode::kDeviceCreateFailed, "probe image");
    }
    VkMemoryRequirements req{};
    p_vkGetImageReqs(device, probe_image_, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    p_vkGetMemProps(dev.physical_device(), &mp);
    std::uint32_t mi = 0xFFFFFFFFu;
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
      if ((req.memoryTypeBits & (1u << i)) != 0u &&
          (mp.memoryTypes[i].propertyFlags &
           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0u) {
        mi = i;
        break;
      }
    }
    if (mi == 0xFFFFFFFFu) {
      for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) != 0u) {
          mi = i;
          break;
        }
      }
    }
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mi;
    if (mi == 0xFFFFFFFFu ||
        p_vkAllocMem(device, &ai, nullptr, &probe_image_mem_) != VK_SUCCESS) {
      fail();
      return MakeFail(StatusCode::kDeviceCreateFailed, "probe image mem");
    }
    if (p_vkBindImageMem(device, probe_image_, probe_image_mem_, 0) !=
        VK_SUCCESS) {
      fail();
      return MakeFail(StatusCode::kDeviceCreateFailed, "probe image bind");
    }
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = probe_image_;
    vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vi.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (p_vkCreateView(device, &vi, nullptr, &probe_image_view_) !=
        VK_SUCCESS) {
      fail();
      return MakeFail(StatusCode::kDeviceCreateFailed, "probe view");
    }
  }
  // Host-visible staging (4 KB) + box UBO (32 B, zeroed = GI off).
  auto alloc_host = [&](std::uint64_t bytes, VkBufferUsageFlags usage,
                        VkBuffer* buf, VkDeviceMemory* mem) -> bool {
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (p_vkCreateBuffer(device, &bi, nullptr, buf) != VK_SUCCESS) {
      return false;
    }
    VkMemoryRequirements req{};
    p_vkGetBufferReqs(device, *buf, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    p_vkGetMemProps(dev.physical_device(), &mp);
    std::uint32_t mi = 0xFFFFFFFFu;
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
      if ((req.memoryTypeBits & (1u << i)) != 0u &&
          (mp.memoryTypes[i].propertyFlags &
           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0u &&
          (mp.memoryTypes[i].propertyFlags &
           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0u) {
        mi = i;
        break;
      }
    }
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mi;
    if (mi == 0xFFFFFFFFu ||
        p_vkAllocMem(device, &ai, nullptr, mem) != VK_SUCCESS) {
      return false;
    }
    return p_vkBindBufferMem(device, *buf, *mem, 0) == VK_SUCCESS;
  };
  if (!alloc_host(kGiProbeBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  &probe_stage_, &probe_stage_mem_)) {
    fail();
    return MakeFail(StatusCode::kDeviceCreateFailed, "probe stage");
  }
  if (!alloc_host(kGiBoxBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &probe_box_,
                  &probe_box_mem_)) {
    fail();
    return MakeFail(StatusCode::kDeviceCreateFailed, "probe box");
  }
  {
    void* mapped = nullptr;
    if (p_vkMapMem(device, probe_box_mem_, 0, kGiBoxBytes, 0, &mapped) !=
        VK_SUCCESS) {
      fail();
      return MakeFail(StatusCode::kDeviceCreateFailed, "probe box map");
    }
    std::memset(mapped, 0, static_cast<std::size_t>(kGiBoxBytes));
    p_vkUnmapMem(device, probe_box_mem_);
  }
  // Linear clamp sampler (deterministic trilinear like the part-1 test).
  {
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    if (p_vkCreateSampler(device, &si, nullptr, &probe_sampler_) !=
        VK_SUCCESS) {
      fail();
      return MakeFail(StatusCode::kDeviceCreateFailed, "probe sampler");
    }
  }
  // Pool + set + write (image layout recorded as SHADER_READ_ONLY: the
  // first upload transitions the image there before any shade reads).
  {
    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[0].descriptorCount = 1;
    sizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[1].descriptorCount = 1;
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 1;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    if (p_vkCreatePool(device, &pi, nullptr, &probe_pool_) != VK_SUCCESS) {
      fail();
      return MakeFail(StatusCode::kDeviceCreateFailed, "probe pool");
    }
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = probe_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &probe_set_layout_;
    if (p_vkAllocSets(device, &ai, &probe_set_) != VK_SUCCESS) {
      fail();
      return MakeFail(StatusCode::kDeviceCreateFailed, "probe alloc");
    }
    VkDescriptorImageInfo img_info{};
    img_info.sampler = probe_sampler_;
    img_info.imageView = probe_image_view_;
    img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorBufferInfo box_info{};
    box_info.buffer = probe_box_;
    box_info.offset = 0;
    box_info.range = kGiBoxBytes;
    VkWriteDescriptorSet w[2]{};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet = probe_set_;
    w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = &img_info;
    w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[1].dstSet = probe_set_;
    w[1].dstBinding = 1;
    w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[1].pBufferInfo = &box_info;
    p_vkUpdateSets(device, 2, w, 0, nullptr);
  }
  probe_ready_ = true;
  probe_layout_dirty_ = true;
  // Destroy-path + bind entry points (stored so teardown after shutdown
  // stays safe, same rule as the other procs).
  proc_destroy_pool_ =
      reinterpret_cast<void*>(dev.proc("vkDestroyDescriptorPool"));
  proc_destroy_set_layout_ = reinterpret_cast<void*>(
      dev.proc("vkDestroyDescriptorSetLayout"));
  proc_free_mem_ =
      reinterpret_cast<void*>(dev.proc("vkFreeMemory"));
  proc_destroy_image_ =
      reinterpret_cast<void*>(dev.proc("vkDestroyImage"));
  proc_destroy_view_ =
      reinterpret_cast<void*>(dev.proc("vkDestroyImageView"));
  proc_destroy_buffer_ =
      reinterpret_cast<void*>(dev.proc("vkDestroyBuffer"));
  proc_destroy_sampler_ =
      reinterpret_cast<void*>(dev.proc("vkDestroySampler"));
  proc_map_mem_ =
      reinterpret_cast<void*>(dev.proc("vkMapMemory"));
  proc_unmap_mem_ =
      reinterpret_cast<void*>(dev.proc("vkUnmapMemory"));
  proc_barrier2_ =
      reinterpret_cast<void*>(dev.proc("vkCmdPipelineBarrier2"));
  proc_copy_buf_img_ =
      reinterpret_cast<void*>(dev.proc("vkCmdCopyBufferToImage"));
  return MakeOk();
}

auto ForwardRenderer::destroy_probe_resources() noexcept -> void {
  auto p_vkDestroyPool = reinterpret_cast<PFN_vkDestroyDescriptorPool>(
      proc_destroy_pool_);
  auto p_vkDestroySetLayout =
      reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(
          proc_destroy_set_layout_);
  auto p_vkFreeMem =
      reinterpret_cast<PFN_vkFreeMemory>(proc_free_mem_);
  auto p_vkDestroyImage =
      reinterpret_cast<PFN_vkDestroyImage>(proc_destroy_image_);
  auto p_vkDestroyView = reinterpret_cast<PFN_vkDestroyImageView>(
      proc_destroy_view_);
  auto p_vkDestroyBuffer =
      reinterpret_cast<PFN_vkDestroyBuffer>(proc_destroy_buffer_);
  auto p_vkDestroySampler =
      reinterpret_cast<PFN_vkDestroySampler>(proc_destroy_sampler_);
  if (device_ == nullptr) {
    probe_pool_ = nullptr;
    probe_set_ = nullptr;
    probe_image_ = nullptr;
    probe_image_mem_ = nullptr;
    probe_image_view_ = nullptr;
    probe_stage_ = nullptr;
    probe_stage_mem_ = nullptr;
    probe_box_ = nullptr;
    probe_box_mem_ = nullptr;
    probe_sampler_ = nullptr;
    probe_ready_ = false;
    return;
  }
  // device_ is live here (destroy() runs this before nulling it).
  if (p_vkDestroyPool != nullptr && probe_pool_ != nullptr) {
    p_vkDestroyPool(device_, probe_pool_, nullptr);
    probe_pool_ = nullptr;
  }
  probe_set_ = nullptr;
  if (p_vkDestroySampler != nullptr && probe_sampler_ != nullptr) {
    p_vkDestroySampler(device_, probe_sampler_, nullptr);
    probe_sampler_ = nullptr;
  }
  if (p_vkDestroyView != nullptr && probe_image_view_ != nullptr) {
    p_vkDestroyView(device_, probe_image_view_, nullptr);
    probe_image_view_ = nullptr;
  }
  if (p_vkDestroyImage != nullptr && probe_image_ != nullptr) {
    p_vkDestroyImage(device_, probe_image_, nullptr);
    probe_image_ = nullptr;
  }
  if (p_vkDestroyBuffer != nullptr && probe_stage_ != nullptr) {
    p_vkDestroyBuffer(device_, probe_stage_, nullptr);
    probe_stage_ = nullptr;
  }
  if (p_vkDestroyBuffer != nullptr && probe_box_ != nullptr) {
    p_vkDestroyBuffer(device_, probe_box_, nullptr);
    probe_box_ = nullptr;
  }
  if (p_vkFreeMem != nullptr) {
    if (probe_image_mem_ != nullptr) {
      p_vkFreeMem(device_, probe_image_mem_, nullptr);
      probe_image_mem_ = nullptr;
    }
    if (probe_stage_mem_ != nullptr) {
      p_vkFreeMem(device_, probe_stage_mem_, nullptr);
      probe_stage_mem_ = nullptr;
    }
    if (probe_box_mem_ != nullptr) {
      p_vkFreeMem(device_, probe_box_mem_, nullptr);
      probe_box_mem_ = nullptr;
    }
  }
  probe_ready_ = false;
  probe_layout_dirty_ = true;
  // The set layout itself (owned by create_mesh alongside the pipe
  // layout).
  if (p_vkDestroySetLayout != nullptr && probe_set_layout_ != nullptr) {
    p_vkDestroySetLayout(device_, probe_set_layout_, nullptr);
    probe_set_layout_ = nullptr;
  }
}

// P4-07 part 2 GI gather+bake: second entity pass over the same
// (WorldTransform, MeshRenderer) pairs the draw loop uses, collecting
// SDF bricks + world matrices + albedos into the frame baker; bakes
// under the directional light; uploads probes + box UBO. Failures are
// non-fatal (render_world draws anyway with the previous gain).
auto ForwardRenderer::bake_gi_probes(VkCommandBuffer cmd, void* world,
                                     const MeshRegistry& registry,
                                     const float* light_dir) noexcept
    -> Status {
  if (!probe_ready_ || world == nullptr || light_dir == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "probe bake bad arg");
  }
  auto* ew = static_cast<ecs_world_t*>(world);
  // Same pair query as the draw loop, via the C API (render_world owns
  // only a raw ecs_world_t*; there is no flecs::world here).
  const ecs_entity_t mesh_comp = ecs_lookup_path_w_sep(
      ew, 0, "Engine.Runtime.MeshRenderer", "", "", false);
  const ecs_entity_t world_tf = ecs_lookup_path_w_sep(
      ew, 0, "WorldTransform", "", "", false);
  if (mesh_comp == 0 || world_tf == 0) {
    return MakeFail(StatusCode::kInvalidArgument, "probe bake no comps");
  }
  struct Row {
    float m[16]{};
    float albedo[3]{1.0f, 1.0f, 1.0f};
    const SdfBrick* bricks{nullptr};
    std::uint32_t count{0};
  };
  std::vector<Row> rows;
  {
    ecs_query_desc_t qd{};
    qd.terms[0].id = world_tf;
    qd.terms[0].src.id = EcsSelf;
    qd.terms[1].id = mesh_comp;
    qd.terms[1].src.id = EcsSelf;
    qd.cache_kind = EcsQueryCacheAuto;
    ecs_query_t* q = ecs_query_init(ew, &qd);
    if (q == nullptr) {
      return MakeFail(StatusCode::kInvalidArgument, "probe bake query");
    }
    ecs_iter_t it = ecs_query_iter(ew, q);
    while (ecs_query_next(&it)) {
      const auto* wcol = static_cast<const ecs::WorldTransform*>(
          ecs_field_w_size(&it, sizeof(ecs::WorldTransform), 0));
      const auto* mcol = static_cast<const ecs::MeshRenderer*>(
          ecs_field_w_size(&it, sizeof(ecs::MeshRenderer), 1));
      if (wcol == nullptr || mcol == nullptr) {
        break;
      }
      for (int r = 0; r < it.count; ++r) {
        const GpuMesh* mesh = registry.find(mcol[r].mesh_id);
        std::uint32_t n = 0;
        const SdfBrick* b = (mesh != nullptr) ? mesh->gi_bricks(&n) : nullptr;
        if (n == 0 || b == nullptr) {
          continue;  // mesh without SDF shell: no GI contribution
        }
        Row row;
        std::memcpy(row.m, wcol[r].m, sizeof(row.m));
        // Entity albedo drives the bounce color (same value the draw
        // pass pushes), not the mesh upload default.
        row.albedo[0] = mcol[r].base_color[0];
        row.albedo[1] = mcol[r].base_color[1];
        row.albedo[2] = mcol[r].base_color[2];
        row.bricks = b;
        row.count = n;
        rows.push_back(row);
      }
    }
    ecs_query_fini(q);
  }
  if (std::getenv("ENGINE_GI_BOX_ONLY") != nullptr) {
    rows.clear();  // debug bisect: exercise the box-UBO write only
  }
  if (rows.empty()) {
    // No GI geometry: keep the frame lit by switching the term off.
    if (gi_active_) {
      const float z[3] = {0.0f, 0.0f, 0.0f};
      const float one[3] = {1.0f, 1.0f, 1.0f};
      const Status st = upload_probes(cmd, nullptr, z, one, 0.0f);
      if (st.ok()) {
        gi_active_ = false;
      }
    }
    return MakeOk();
  }
  // Scene AABB from brick extents (mesh-local -> model -> world).
  float lo[3] = {1e30f, 1e30f, 1e30f};
  float hi[3] = {-1e30f, -1e30f, -1e30f};
  for (const Row& r : rows) {
    for (std::uint32_t bi = 0; bi < r.count; ++bi) {
      const SdfBrick& b = r.bricks[bi];
      const float ext = 8.0f * b.texel_size;
      for (int c = 0; c < 8; ++c) {
        const float lx = b.origin[0] + ((c & 1) != 0 ? ext : 0.0f);
        const float ly = b.origin[1] + ((c & 2) != 0 ? ext : 0.0f);
        const float lz = b.origin[2] + ((c & 4) != 0 ? ext : 0.0f);
        const float* m = r.m;
        const float wx =
            m[0] * lx + m[4] * ly + m[8] * lz + m[12];
        const float wy =
            m[1] * lx + m[5] * ly + m[9] * lz + m[13];
        const float wz =
            m[2] * lx + m[6] * ly + m[10] * lz + m[14];
        if (wx < lo[0]) lo[0] = wx;
        if (wy < lo[1]) lo[1] = wy;
        if (wz < lo[2]) lo[2] = wz;
        if (wx > hi[0]) hi[0] = wx;
        if (wy > hi[1]) hi[1] = wy;
        if (wz > hi[2]) hi[2] = wz;
      }
    }
  }
  float span[3] = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
  for (int i = 0; i < 3; ++i) {
    if (!(span[i] > 1e-6f)) {
      span[i] = 1.0f;  // degenerate axis: keep the box valid
      hi[i] = lo[i] + 1.0f;
    } else {
      const float pad = 0.05f * span[i];
      lo[i] -= pad;
      hi[i] += pad;
      span[i] += 2.0f * pad;
    }
  }
  GiBakeLight light{};
  {
    float len = std::sqrt(light_dir[0] * light_dir[0] +
                          light_dir[1] * light_dir[1] +
                          light_dir[2] * light_dir[2]);
    if (!(len > 1e-6f)) {
      len = 1.0f;
    }
    light.direction[0] = light_dir[0] / len;
    light.direction[1] = light_dir[1] / len;
    light.direction[2] = light_dir[2] / len;
  }
  light.color[0] = 1.0f;
  light.color[1] = 1.0f;
  light.color[2] = 1.0f;
  light.intensity = 1.0f;
  GiBakeParams params{};
  params.box_min[0] = lo[0];
  params.box_min[1] = lo[1];
  params.box_min[2] = lo[2];
  params.box_size[0] = span[0];
  params.box_size[1] = span[1];
  params.box_size[2] = span[2];
  std::fprintf(stderr, "GIDIAG rows=%zu box=(%.1f,%.1f,%.1f)+(%.1f,%.1f,%.1f)\n",
               rows.size(), lo[0], lo[1], lo[2], span[0], span[1], span[2]);
  const Status bs = gi_baker_.configure(params);
  if (!bs.ok()) {
    std::fprintf(stderr, "GIDIAG configure fail %d\n",
                 static_cast<int>(bs.code));
    return bs;
  }
  for (const Row& r : rows) {
    const Status as =
        gi_baker_.add_bricks(r.bricks, r.count, r.m, r.albedo);
    if (!as.ok()) {
      return as;
    }
  }
  std::vector<float> probes(static_cast<std::size_t>(kGiProbeRes) *
                            kGiProbeRes * kGiProbeRes * 3u);
  const Status bk = gi_baker_.bake(light, probes.data());
  if (!bk.ok()) {
    std::fprintf(stderr, "GIDIAG bake fail %d\n",
                 static_cast<int>(bk.code));
    return bk;
  }
  {
    float mx = 0.0f;
    std::uint32_t n_nan = 0;
    std::uint32_t n_inf = 0;
    for (float v : probes) {
      if (v > mx) mx = v;
      if (v != v) ++n_nan;
      else if (v > 1e30f) ++n_inf;
    }
    std::fprintf(stderr, "GIDIAG nan=%u inf=%u\n", n_nan, n_inf);
    std::fprintf(stderr, "GIDIAG bake ok solid=%u probemax=%.4f\n",
                 gi_baker_.solid_voxels(), mx);
  }
  // UBO gain: the baker writes unit irradiance (gain 1); the UBO carries
  // the frame intensity knob. 1.0 matches the direct term's unit scale.
  const Status us = upload_probes(cmd, probes.data(), lo, span, 1.0f);
  if (!us.ok()) {
    return us;
  }
  gi_active_ = true;
  gi_frame_baked_ = true;
  return MakeOk();
}

auto ForwardRenderer::upload_probes(VkCommandBuffer cmd, const float* probes,
                                    const float* box_min,
                                    const float* box_size,
                                    float gain) noexcept -> Status {
  auto p_vkMapMem = reinterpret_cast<PFN_vkMapMemory>(proc_map_mem_);
  auto p_vkUnmapMem = reinterpret_cast<PFN_vkUnmapMemory>(proc_unmap_mem_);
  auto p_vkBarrier2 =
      reinterpret_cast<PFN_vkCmdPipelineBarrier2>(proc_barrier2_);
  auto p_vkCopyBufImg =
      reinterpret_cast<PFN_vkCmdCopyBufferToImage>(proc_copy_buf_img_);
  if (!probe_ready_ || cmd == nullptr || box_min == nullptr ||
      box_size == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "probe upload bad arg");
  }
  if (p_vkMapMem == nullptr || p_vkUnmapMem == nullptr ||
      p_vkBarrier2 == nullptr || p_vkCopyBufImg == nullptr) {
    std::fprintf(stderr, "GIDIAG upload no entry %p %p %p %p\n",
                 proc_map_mem_, proc_unmap_mem_, proc_barrier2_,
                 proc_copy_buf_img_);
    return MakeFail(StatusCode::kVulkanLoaderFailed, "probe no upload entry");
  }
  std::fprintf(stderr, "GIDIAG upload probes=%p gain=%.2f ready=%d\n",
               static_cast<const void*>(probes), gain,
               static_cast<int>(probe_ready_));
  // Box UBO first (gain 0 when probes == null keeps the term off).
  {
    void* mapped = nullptr;
    if (p_vkMapMem(device_, probe_box_mem_, 0, kGiBoxBytes, 0, &mapped) !=
        VK_SUCCESS) {
      return MakeFail(StatusCode::kDeviceCreateFailed, "probe box map");
    }
    float* f = static_cast<float*>(mapped);
    if (probes == nullptr) {
      f[0] = 0.0f;
      f[1] = 0.0f;
      f[2] = 0.0f;
      f[3] = 0.0f;
      f[4] = 1.0f;
      f[5] = 1.0f;
      f[6] = 1.0f;
      f[7] = 0.0f;  // gain 0 = GI off
    } else {
      f[0] = box_min[0];
      f[1] = box_min[1];
      f[2] = box_min[2];
      f[3] = 0.0f;
      f[4] = box_size[0];
      f[5] = box_size[1];
      f[6] = box_size[2];
      f[7] = gain;
    }
    p_vkUnmapMem(device_, probe_box_mem_);
    if (probes == nullptr) {
      return MakeOk();
    }
  }
  // Halves into staging.
  {
    void* mapped = nullptr;
    if (p_vkMapMem(device_, probe_stage_mem_, 0, kGiProbeBytes, 0, &mapped) !=
        VK_SUCCESS) {
      return MakeFail(StatusCode::kDeviceCreateFailed, "probe stage map");
    }
    auto* out = static_cast<std::uint16_t*>(mapped);
    const std::uint64_t n = static_cast<std::uint64_t>(kGiProbeRes) *
                            kGiProbeRes * kGiProbeRes;
    for (std::uint64_t i = 0; i < n; ++i) {
      out[i * 4] = FloatToHalf(probes[i * 3]);
      out[i * 4 + 1] = FloatToHalf(probes[i * 3 + 1]);
      out[i * 4 + 2] = FloatToHalf(probes[i * 3 + 2]);
      out[i * 4 + 3] = FloatToHalf(1.0f);
    }
    p_vkUnmapMem(device_, probe_stage_mem_);
  }
  // UNDEFINED/TRANSFER_DST -> TRANSFER_DST_OPTIMAL, copy, -> SHADER_READ.
  const bool first = probe_layout_dirty_;
  {
    VkImageMemoryBarrier2 bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    if (first) {
      bar.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
      bar.srcAccessMask = VK_ACCESS_2_NONE;
      bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    } else {
      bar.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      bar.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
      bar.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    bar.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    bar.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.image = probe_image_;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &bar;
    p_vkBarrier2(cmd, &di);
  }
  {
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {kGiProbeRes, kGiProbeRes, kGiProbeRes};
    p_vkCopyBufImg(cmd, probe_stage_, probe_image_,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  }
  {
    VkImageMemoryBarrier2 bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    bar.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    bar.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    bar.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    bar.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.image = probe_image_;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &bar;
    p_vkBarrier2(cmd, &di);
  }
  probe_layout_dirty_ = false;
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
  auto p_vkBindSets = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(
      proc_bind_sets_);
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
  // P4-07 part 2 GI probe set (set 0, fragment). Always bound: the layout
  // requires it, and the box gain 0 makes the term a no-op when GI is off.
  // Missing bind entry -> fail closed (a UB read is worse than no draw).
  if (p_vkBindSets == nullptr || probe_set_ == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "mesh no set entry");
  }
  p_vkBindSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_layout_, 0, 1,
               &probe_set_, 0, nullptr);
  p_vkCmdPushConstants(cmd, mesh_layout_,
                     VK_SHADER_STAGE_VERTEX_BIT |
                         VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, static_cast<std::uint32_t>(sizeof(MeshPush)), &push);
  p_vkCmdDrawIndexed(cmd, mesh.index_count(), 1, 0, 0, 0);
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

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
#include "engine/renderer/shaders/tri_frag_spv.inc"
#include "engine/renderer/shaders/tri_vert_spv.inc"

#if defined(ENGINE_RENDERER_HAS_VULKAN)
// volk-first include order (same rule as vulkan_device.cpp): volk defines
// VK_NO_PROTOTYPES and hard-errors if vulkan.h came first.
#include <volk.h>
#endif

#include "engine/renderer/gpu_mesh.hpp"
#include "engine/renderer/vulkan_device.hpp"

namespace engine::renderer {

ForwardRenderer::~ForwardRenderer() noexcept { destroy(); }

#if defined(ENGINE_RENDERER_HAS_VULKAN)

namespace {

auto valid_spv(const std::uint32_t* w, std::size_t n) noexcept -> bool {
  return w != nullptr && n > 5 && w[0] == 0x07230203u && w[1] >= 0x00010300u;
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
  auto vkCreateShaderModule = reinterpret_cast<PFN_vkCreateShaderModule>(
      dev.instance_proc("vkCreateShaderModule"));
  auto vkDestroyShaderModule = reinterpret_cast<PFN_vkDestroyShaderModule>(
      dev.instance_proc("vkDestroyShaderModule"));
  auto vkCreateGraphicsPipelines =
      reinterpret_cast<PFN_vkCreateGraphicsPipelines>(
          dev.proc("vkCreateGraphicsPipelines"));
  auto vkDestroyPipeline = reinterpret_cast<PFN_vkDestroyPipeline>(
      dev.proc("vkDestroyPipeline"));
  auto vkCreatePipelineLayout = reinterpret_cast<PFN_vkCreatePipelineLayout>(
      dev.instance_proc("vkCreatePipelineLayout"));
  auto vkDestroyPipelineLayout =
      reinterpret_cast<PFN_vkDestroyPipelineLayout>(
          dev.instance_proc("vkDestroyPipelineLayout"));
  auto vkCmdBindPipeline = reinterpret_cast<PFN_vkCmdBindPipeline>(
      dev.proc("vkCmdBindPipeline"));
  auto vkCmdSetViewport =
      reinterpret_cast<PFN_vkCmdSetViewport>(dev.proc("vkCmdSetViewport"));
  auto vkCmdSetScissor =
      reinterpret_cast<PFN_vkCmdSetScissor>(dev.proc("vkCmdSetScissor"));
  auto vkCmdDraw = reinterpret_cast<PFN_vkCmdDraw>(dev.proc("vkCmdDraw"));
  auto vkCmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(
      dev.proc("vkCmdBeginRendering"));
  auto vkCmdEndRendering =
      reinterpret_cast<PFN_vkCmdEndRendering>(dev.proc("vkCmdEndRendering"));
  if (vkCreateShaderModule == nullptr || vkDestroyShaderModule == nullptr ||
      vkCreateGraphicsPipelines == nullptr || vkDestroyPipeline == nullptr ||
      vkCreatePipelineLayout == nullptr || vkDestroyPipelineLayout == nullptr ||
      vkCmdBindPipeline == nullptr || vkCmdSetViewport == nullptr ||
      vkCmdSetScissor == nullptr || vkCmdDraw == nullptr ||
      vkCmdBeginRendering == nullptr || vkCmdEndRendering == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "forward no entry");
  }
  const std::uint32_t* blobs[2] = {vert, frag};
  const std::size_t words[2] = {vert_n, frag_n};
  for (int i = 0; i < 2; ++i) {
    VkShaderModuleCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sci.codeSize = words[i] * 4;
    sci.pCode = blobs[i];
    if (vkCreateShaderModule(device, &sci, nullptr, &modules_[i]) !=
        VK_SUCCESS) {
      for (int j = 0; j < i; ++j) {
        vkDestroyShaderModule(device, modules_[j], nullptr);
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
    if (vkCreatePipelineLayout(device, &pli, nullptr, &pipe_layout) !=
        VK_SUCCESS) {
      for (int j = 0; j < 2; ++j) {
        vkDestroyShaderModule(device, modules_[j], nullptr);
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

    if (vkCreateGraphicsPipelines(device, nullptr, 1, &pci, nullptr, &pipe) !=
        VK_SUCCESS) {
      vkDestroyPipelineLayout(device, pipe_layout, nullptr);
      for (int j = 0; j < 2; ++j) {
        vkDestroyShaderModule(device, modules_[j], nullptr);
        modules_[j] = nullptr;
      }
      module_count_ = 0;
      return MakeFail(StatusCode::kDeviceCreateFailed, "forward pipe");
    }
  }
  pipeline_ = pipe;
  pipe_layout_ = pipe_layout;
  device_ = device;
  proc_create_module_ = reinterpret_cast<void*>(vkCreateShaderModule);
  proc_destroy_module_ = reinterpret_cast<void*>(vkDestroyShaderModule);
  proc_create_pipelines_ = reinterpret_cast<void*>(vkCreateGraphicsPipelines);
  proc_destroy_pipeline_ = reinterpret_cast<void*>(vkDestroyPipeline);
  proc_create_layout_ = reinterpret_cast<void*>(vkCreatePipelineLayout);
  proc_destroy_layout_ = reinterpret_cast<void*>(vkDestroyPipelineLayout);
  proc_bind_pipeline_ = reinterpret_cast<void*>(vkCmdBindPipeline);
  proc_cmd_viewport_ = reinterpret_cast<void*>(vkCmdSetViewport);
  proc_cmd_scissor_ = reinterpret_cast<void*>(vkCmdSetScissor);
  proc_cmd_draw_ = reinterpret_cast<void*>(vkCmdDraw);
  proc_begin_rendering_ = reinterpret_cast<void*>(vkCmdBeginRendering);
  proc_end_rendering_ = reinterpret_cast<void*>(vkCmdEndRendering);
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
  auto vkCmdBindPipeline =
      reinterpret_cast<PFN_vkCmdBindPipeline>(proc_bind_pipeline_);
  auto vkCmdSetViewport =
      reinterpret_cast<PFN_vkCmdSetViewport>(proc_cmd_viewport_);
  auto vkCmdSetScissor =
      reinterpret_cast<PFN_vkCmdSetScissor>(proc_cmd_scissor_);
  auto vkCmdDraw = reinterpret_cast<PFN_vkCmdDraw>(proc_cmd_draw_);
  auto vkCmdBeginRendering =
      reinterpret_cast<PFN_vkCmdBeginRendering>(proc_begin_rendering_);
  auto vkCmdEndRendering =
      reinterpret_cast<PFN_vkCmdEndRendering>(proc_end_rendering_);
  if (vkCmdBindPipeline == nullptr || vkCmdSetViewport == nullptr ||
      vkCmdSetScissor == nullptr || vkCmdDraw == nullptr ||
      vkCmdBeginRendering == nullptr || vkCmdEndRendering == nullptr) {
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
  vkCmdBeginRendering(cmd, &ri);

  VkViewport vp{};
  vp.x = 0.0F;
  vp.y = 0.0F;
  vp.width = static_cast<float>(extent.width);
  vp.height = static_cast<float>(extent.height);
  vp.minDepth = 0.0F;
  vp.maxDepth = 1.0F;
  vkCmdSetViewport(cmd, 0, 1, &vp);
  VkRect2D scissor{};
  scissor.offset.x = 0;
  scissor.offset.y = 0;
  scissor.extent = extent;
  vkCmdSetScissor(cmd, 0, 1, &scissor);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRendering(cmd);
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
  auto vkCreateShaderModule = reinterpret_cast<PFN_vkCreateShaderModule>(
      dev.instance_proc("vkCreateShaderModule"));
  auto vkDestroyShaderModule = reinterpret_cast<PFN_vkDestroyShaderModule>(
      dev.instance_proc("vkDestroyShaderModule"));
  auto vkCreateGraphicsPipelines =
      reinterpret_cast<PFN_vkCreateGraphicsPipelines>(
          dev.proc("vkCreateGraphicsPipelines"));
  auto vkDestroyPipeline = reinterpret_cast<PFN_vkDestroyPipeline>(
      dev.proc("vkDestroyPipeline"));
  auto vkCreatePipelineLayout = reinterpret_cast<PFN_vkCreatePipelineLayout>(
      dev.instance_proc("vkCreatePipelineLayout"));
  auto vkDestroyPipelineLayout =
      reinterpret_cast<PFN_vkDestroyPipelineLayout>(
          dev.instance_proc("vkDestroyPipelineLayout"));
  auto vkCmdBindPipeline = reinterpret_cast<PFN_vkCmdBindPipeline>(
      dev.proc("vkCmdBindPipeline"));
  auto vkCmdSetViewport =
      reinterpret_cast<PFN_vkCmdSetViewport>(dev.proc("vkCmdSetViewport"));
  auto vkCmdSetScissor =
      reinterpret_cast<PFN_vkCmdSetScissor>(dev.proc("vkCmdSetScissor"));
  auto vkCmdDrawIndexed = reinterpret_cast<PFN_vkCmdDrawIndexed>(
      dev.proc("vkCmdDrawIndexed"));
  auto vkCmdBindVertexBuffers = reinterpret_cast<PFN_vkCmdBindVertexBuffers>(
      dev.proc("vkCmdBindVertexBuffers"));
  auto vkCmdBindIndexBuffer = reinterpret_cast<PFN_vkCmdBindIndexBuffer>(
      dev.proc("vkCmdBindIndexBuffer"));
  auto vkCmdPushConstants = reinterpret_cast<PFN_vkCmdPushConstants>(
      dev.proc("vkCmdPushConstants"));
  auto vkCmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(
      dev.proc("vkCmdBeginRendering"));
  auto vkCmdEndRendering =
      reinterpret_cast<PFN_vkCmdEndRendering>(dev.proc("vkCmdEndRendering"));
  if (vkCreateShaderModule == nullptr || vkDestroyShaderModule == nullptr ||
      vkCreateGraphicsPipelines == nullptr || vkDestroyPipeline == nullptr ||
      vkCreatePipelineLayout == nullptr || vkDestroyPipelineLayout == nullptr ||
      vkCmdBindPipeline == nullptr || vkCmdSetViewport == nullptr ||
      vkCmdSetScissor == nullptr || vkCmdDrawIndexed == nullptr ||
      vkCmdBindVertexBuffers == nullptr || vkCmdBindIndexBuffer == nullptr ||
      vkCmdPushConstants == nullptr || vkCmdBeginRendering == nullptr ||
      vkCmdEndRendering == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "mesh no entry");
  }
  const std::uint32_t* blobs[2] = {vert, frag};
  const std::size_t words[2] = {vert_n, frag_n};
  for (int i = 0; i < 2; ++i) {
    VkShaderModuleCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sci.codeSize = words[i] * 4;
    sci.pCode = blobs[i];
    if (vkCreateShaderModule(device, &sci, nullptr, &mesh_modules_[i]) !=
        VK_SUCCESS) {
      for (int j = 0; j < i; ++j) {
        vkDestroyShaderModule(device, mesh_modules_[j], nullptr);
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
    if (vkCreatePipelineLayout(device, &pli, nullptr, &mesh_layout) !=
        VK_SUCCESS) {
      for (int j = 0; j < 2; ++j) {
        vkDestroyShaderModule(device, mesh_modules_[j], nullptr);
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

    if (vkCreateGraphicsPipelines(device, nullptr, 1, &pci, nullptr, &pipe) !=
        VK_SUCCESS) {
      vkDestroyPipelineLayout(device, mesh_layout, nullptr);
      for (int j = 0; j < 2; ++j) {
        vkDestroyShaderModule(device, mesh_modules_[j], nullptr);
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
    proc_create_module_ = reinterpret_cast<void*>(vkCreateShaderModule);
    proc_destroy_module_ = reinterpret_cast<void*>(vkDestroyShaderModule);
    proc_create_pipelines_ =
        reinterpret_cast<void*>(vkCreateGraphicsPipelines);
    proc_destroy_pipeline_ = reinterpret_cast<void*>(vkDestroyPipeline);
    proc_create_layout_ = reinterpret_cast<void*>(vkCreatePipelineLayout);
    proc_destroy_layout_ = reinterpret_cast<void*>(vkDestroyPipelineLayout);
    proc_bind_pipeline_ = reinterpret_cast<void*>(vkCmdBindPipeline);
    proc_cmd_viewport_ = reinterpret_cast<void*>(vkCmdSetViewport);
    proc_cmd_scissor_ = reinterpret_cast<void*>(vkCmdSetScissor);
    proc_begin_rendering_ = reinterpret_cast<void*>(vkCmdBeginRendering);
    proc_end_rendering_ = reinterpret_cast<void*>(vkCmdEndRendering);
  }
  proc_cmd_draw_indexed_ = reinterpret_cast<void*>(vkCmdDrawIndexed);
  proc_cmd_bind_vb_ = reinterpret_cast<void*>(vkCmdBindVertexBuffers);
  proc_cmd_bind_ib_ = reinterpret_cast<void*>(vkCmdBindIndexBuffer);
  proc_cmd_push_ = reinterpret_cast<void*>(vkCmdPushConstants);
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
  auto vkCmdBeginRendering =
      reinterpret_cast<PFN_vkCmdBeginRendering>(proc_begin_rendering_);
  auto vkCmdSetViewport =
      reinterpret_cast<PFN_vkCmdSetViewport>(proc_cmd_viewport_);
  auto vkCmdSetScissor =
      reinterpret_cast<PFN_vkCmdSetScissor>(proc_cmd_scissor_);
  auto vkCmdBindPipeline =
      reinterpret_cast<PFN_vkCmdBindPipeline>(proc_bind_pipeline_);
  if (vkCmdBeginRendering == nullptr || vkCmdSetViewport == nullptr ||
      vkCmdSetScissor == nullptr || vkCmdBindPipeline == nullptr) {
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
  vkCmdBeginRendering(cmd, &ri);
  VkViewport vp{};
  vp.x = 0.0F;
  vp.y = 0.0F;
  vp.width = static_cast<float>(extent.width);
  vp.height = static_cast<float>(extent.height);
  vp.minDepth = 0.0F;
  vp.maxDepth = 1.0F;
  vkCmdSetViewport(cmd, 0, 1, &vp);
  VkRect2D scissor{};
  scissor.offset.x = 0;
  scissor.offset.y = 0;
  scissor.extent = extent;
  vkCmdSetScissor(cmd, 0, 1, &scissor);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mesh_pipeline_);
  return MakeOk();
}

auto ForwardRenderer::end_mesh_pass(VkCommandBuffer cmd) noexcept -> Status {
  if (mesh_pipeline_ == nullptr || mesh_layout_ == nullptr) {
    return MakeFail(StatusCode::kNotInitialized, "mesh not built");
  }
  if (cmd == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "mesh bad end arg");
  }
  auto vkCmdEndRendering =
      reinterpret_cast<PFN_vkCmdEndRendering>(proc_end_rendering_);
  if (vkCmdEndRendering == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "mesh no end entry");
  }
  vkCmdEndRendering(cmd);
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
  auto vkCmdBindVertexBuffers =
      reinterpret_cast<PFN_vkCmdBindVertexBuffers>(proc_cmd_bind_vb_);
  auto vkCmdBindIndexBuffer =
      reinterpret_cast<PFN_vkCmdBindIndexBuffer>(proc_cmd_bind_ib_);
  auto vkCmdPushConstants =
      reinterpret_cast<PFN_vkCmdPushConstants>(proc_cmd_push_);
  auto vkCmdDrawIndexed =
      reinterpret_cast<PFN_vkCmdDrawIndexed>(proc_cmd_draw_indexed_);
  if (vkCmdBindVertexBuffers == nullptr || vkCmdBindIndexBuffer == nullptr ||
      vkCmdPushConstants == nullptr || vkCmdDrawIndexed == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "mesh no record entry");
  }
  VkBuffer vb = reinterpret_cast<VkBuffer>(
      static_cast<std::uintptr_t>(mesh.vertex_buffer_bits()));
  VkBuffer ib = reinterpret_cast<VkBuffer>(
      static_cast<std::uintptr_t>(mesh.index_buffer_bits()));
  const VkDeviceSize offsets[1] = {0};
  vkCmdBindVertexBuffers(cmd, 0, 1, &vb, offsets);
  vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
  vkCmdPushConstants(cmd, mesh_layout_,
                     VK_SHADER_STAGE_VERTEX_BIT |
                         VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, static_cast<std::uint32_t>(sizeof(MeshPush)), &push);
  vkCmdDrawIndexed(cmd, mesh.index_count(), 1, 0, 0, 0);
  return MakeOk();
}

auto ForwardRenderer::destroy() noexcept -> void {
  // Reference the create-time-only procs so Clang -Wunused-private-field
  // stays clean under -Werror (same pattern as GiFrameGraph).
  (void)proc_create_module_;
  (void)proc_create_pipelines_;
  (void)proc_create_layout_;
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
    ready_ = false;
    device_ = nullptr;
    return;
  }
  auto vkDestroyShaderModule = reinterpret_cast<PFN_vkDestroyShaderModule>(
      proc_destroy_module_);
  auto vkDestroyPipeline =
      reinterpret_cast<PFN_vkDestroyPipeline>(proc_destroy_pipeline_);
  auto vkDestroyPipelineLayout =
      reinterpret_cast<PFN_vkDestroyPipelineLayout>(proc_destroy_layout_);
  if (vkDestroyShaderModule != nullptr) {
    for (int i = 0; i < 2; ++i) {
      if (modules_[i] != nullptr) {
        vkDestroyShaderModule(device_, modules_[i], nullptr);
        modules_[i] = nullptr;
      }
    }
  }
  if (vkDestroyPipeline != nullptr && pipeline_ != nullptr) {
    vkDestroyPipeline(device_, pipeline_, nullptr);
    pipeline_ = nullptr;
  }
  if (vkDestroyPipelineLayout != nullptr && pipe_layout_ != nullptr) {
    vkDestroyPipelineLayout(device_, pipe_layout_, nullptr);
    pipe_layout_ = nullptr;
  }
  if (vkDestroyShaderModule != nullptr) {
    for (int i = 0; i < 2; ++i) {
      if (mesh_modules_[i] != nullptr) {
        vkDestroyShaderModule(device_, mesh_modules_[i], nullptr);
        mesh_modules_[i] = nullptr;
      }
    }
  }
  if (vkDestroyPipeline != nullptr && mesh_pipeline_ != nullptr) {
    vkDestroyPipeline(device_, mesh_pipeline_, nullptr);
    mesh_pipeline_ = nullptr;
  }
  if (vkDestroyPipelineLayout != nullptr && mesh_layout_ != nullptr) {
    vkDestroyPipelineLayout(device_, mesh_layout_, nullptr);
    mesh_layout_ = nullptr;
  }
  module_count_ = 0;
  mesh_module_count_ = 0;
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
  module_count_ = 0;
  ready_ = false;
}

#endif

}  // namespace engine::renderer

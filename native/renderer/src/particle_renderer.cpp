// particle_renderer.cpp — P5-05: particle billboard pipelines + one draw.
//
// Real Vulkan path: creates 2 shader modules from the checked-in particle
// .inc blobs and builds 2 graphics pipelines (swapchain UNORM target,
// dynamic rendering, ParticleVertex input, no depth): alpha
// (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) and additive (SRC_ALPHA, ONE), with
// one shared 80-byte push-constant layout (ParticlePush).
// begin_particle_pass() opens its own dynamic-rendering pass (CLEAR
// transparent black, STORE); record_draw() binds the quad buffers,
// pushes view_proj, and draws quad_count*6 indices at firstIndex
// first_quad*6 — exactly one vkCmdDrawIndexed per call. Entry points
// load via VulkanDevice::proc()/instance_proc() (same pattern as
// SpriteRenderer).
#include "engine/renderer/particle_renderer.hpp"

#include <initializer_list>

#include "engine/renderer/shaders/particle_frag_spv.inc"
#include "engine/renderer/shaders/particle_vert_spv.inc"

#if defined(ENGINE_RENDERER_HAS_VULKAN)
// volk-first include order (same rule as vulkan_device.cpp): volk defines
// VK_NO_PROTOTYPES and hard-errors if vulkan.h came first.
#include <volk.h>
#endif

#include "engine/renderer/gpu_mesh.hpp"
#include "engine/renderer/vulkan_device.hpp"

namespace engine::renderer {

ParticleRenderer::~ParticleRenderer() noexcept { destroy(); }

#if defined(ENGINE_RENDERER_HAS_VULKAN)

namespace {

auto valid_spv(const std::uint32_t* w, std::size_t n) noexcept -> bool {
  return w != nullptr && n > 5 && w[0] == 0x07230203u && w[1] >= 0x00010300u;
}

}  // namespace

auto ParticleRenderer::create(VulkanDevice& dev, VkFormat color_format,
                              const std::uint32_t* vert_spv,
                              std::size_t vert_words,
                              const std::uint32_t* frag_spv,
                              std::size_t frag_words) noexcept -> Status {
  if (ready_) {
    return MakeFail(StatusCode::kAlreadyInitialized, "particles built");
  }
  static_assert(sizeof(ParticlePush) == 80, "particle push is 80 bytes");
  static_assert(sizeof(ParticleVertex) == 40, "particle vertex is 40 bytes");
  const std::uint32_t* vert =
      (vert_spv != nullptr) ? vert_spv : kParticleVertSpv;
  const std::size_t vert_n =
      (vert_spv != nullptr) ? vert_words : kParticleVertSpvWords;
  const std::uint32_t* frag =
      (frag_spv != nullptr) ? frag_spv : kParticleFragSpv;
  const std::size_t frag_n =
      (frag_spv != nullptr) ? frag_words : kParticleFragSpvWords;
  if (!valid_spv(vert, vert_n) || !valid_spv(frag, frag_n)) {
    return MakeFail(StatusCode::kInvalidArgument, "particles bad blob");
  }
  if (color_format != VK_FORMAT_B8G8R8A8_UNORM &&
      color_format != VK_FORMAT_R8G8B8A8_UNORM) {
    return MakeFail(StatusCode::kInvalidArgument, "particles bad format");
  }
  VkDevice device = dev.device();
  if (device == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "particles null device");
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
    return MakeFail(StatusCode::kVulkanLoaderFailed, "particles no entry");
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
      return MakeFail(StatusCode::kDeviceCreateFailed, "particles module");
    }
    ++module_count_;
  }
  VkPipelineLayout layout = nullptr;
  {
    VkPushConstantRange push{};
    push.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset = 0;
    push.size = static_cast<std::uint32_t>(sizeof(ParticlePush));
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    if (p_vkCreatePipelineLayout(device, &pli, nullptr, &layout) !=
        VK_SUCCESS) {
      for (int j = 0; j < 2; ++j) {
        p_vkDestroyShaderModule(device, modules_[j], nullptr);
        modules_[j] = nullptr;
      }
      module_count_ = 0;
      return MakeFail(StatusCode::kDeviceCreateFailed, "particles layout");
    }
  }
  // Shared pipeline skeleton; only the blend attachment differs between
  // the alpha and additive pipes.
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = modules_[0];
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = modules_[1];
  stages[1].pName = "main";

  VkVertexInputBindingDescription binding{};
  binding.binding = 0;
  binding.stride = static_cast<std::uint32_t>(sizeof(ParticleVertex));
  binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  VkVertexInputAttributeDescription attrs[4]{};
  attrs[0].location = 0;  // pos[3]
  attrs[0].binding = 0;
  attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
  attrs[0].offset = 0;
  attrs[1].location = 1;  // color[4]
  attrs[1].binding = 0;
  attrs[1].format = VK_FORMAT_R32G32B32A32_SFLOAT;
  attrs[1].offset = 12;
  attrs[2].location = 2;  // size
  attrs[2].binding = 0;
  attrs[2].format = VK_FORMAT_R32_SFLOAT;
  attrs[2].offset = 28;
  attrs[3].location = 3;  // corner[2]
  attrs[3].binding = 0;
  attrs[3].format = VK_FORMAT_R32G32_SFLOAT;
  attrs[3].offset = 32;
  VkPipelineVertexInputStateCreateInfo vertex_input{};
  vertex_input.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertex_input.vertexBindingDescriptionCount = 1;
  vertex_input.pVertexBindingDescriptions = &binding;
  vertex_input.vertexAttributeDescriptionCount = 4;
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

  // No depth (billboard overlay): depth test off.
  VkPipelineDepthStencilStateCreateInfo depth{};
  depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

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

  VkPipeline pipes[2]{nullptr, nullptr};
  for (int p = 0; p < 2; ++p) {
    const bool additive = (p == 1);
    VkPipelineColorBlendAttachmentState blend_att{};
    blend_att.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    blend_att.blendEnable = VK_TRUE;
    blend_att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    // Sorted alpha: SRC_ALPHA + ONE_MINUS_SRC_ALPHA. Additive:
    // SRC_ALPHA + ONE (spec §6 P5-05 blend modes).
    blend_att.dstColorBlendFactor = additive ? VK_BLEND_FACTOR_ONE
                                             : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_att.colorBlendOp = VK_BLEND_OP_ADD;
    blend_att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_att.dstAlphaBlendFactor =
        additive ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;
    blend_att.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_att;

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

    if (p_vkCreateGraphicsPipelines(device, nullptr, 1, &pci, nullptr,
                                    &pipes[p]) != VK_SUCCESS) {
      for (int q = 0; q < p; ++q) {
        p_vkDestroyPipeline(device, pipes[q], nullptr);
      }
      p_vkDestroyPipelineLayout(device, layout, nullptr);
      for (int j = 0; j < 2; ++j) {
        p_vkDestroyShaderModule(device, modules_[j], nullptr);
        modules_[j] = nullptr;
      }
      module_count_ = 0;
      return MakeFail(StatusCode::kDeviceCreateFailed, "particles pipe");
    }
  }
  pipe_alpha_ = pipes[0];
  pipe_add_ = pipes[1];
  pipe_layout_ = layout;
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
  proc_cmd_draw_indexed_ = reinterpret_cast<void*>(p_vkCmdDrawIndexed);
  proc_cmd_bind_vb_ = reinterpret_cast<void*>(p_vkCmdBindVertexBuffers);
  proc_cmd_bind_ib_ = reinterpret_cast<void*>(p_vkCmdBindIndexBuffer);
  proc_cmd_push_ = reinterpret_cast<void*>(p_vkCmdPushConstants);
  ready_ = true;
  return MakeOk();
}

auto ParticleRenderer::begin_particle_pass(VkCommandBuffer cmd,
                                           VkImageView color_view,
                                           VkExtent2D extent) noexcept
    -> Status {
  if (pipe_alpha_ == nullptr || pipe_layout_ == nullptr) {
    return MakeFail(StatusCode::kNotInitialized, "particles not built");
  }
  if (cmd == nullptr || color_view == nullptr || extent.width == 0 ||
      extent.height == 0) {
    return MakeFail(StatusCode::kInvalidArgument, "particles bad pass arg");
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
    return MakeFail(StatusCode::kVulkanLoaderFailed, "particles no pass entry");
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
  p_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_alpha_);
  return MakeOk();
}

auto ParticleRenderer::end_particle_pass(VkCommandBuffer cmd) noexcept
    -> Status {
  if (pipe_alpha_ == nullptr || pipe_layout_ == nullptr) {
    return MakeFail(StatusCode::kNotInitialized, "particles not built");
  }
  if (cmd == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "particles bad end arg");
  }
  auto p_vkCmdEndRendering =
      reinterpret_cast<PFN_vkCmdEndRendering>(proc_end_rendering_);
  if (p_vkCmdEndRendering == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "particles no end entry");
  }
  p_vkCmdEndRendering(cmd);
  return MakeOk();
}

auto ParticleRenderer::record_draw(VkCommandBuffer cmd, const GpuMesh& mesh,
                                   const ParticlePush& push, bool additive,
                                   std::uint32_t first_quad,
                                   std::uint32_t quad_count,
                                   std::uint32_t total_quads) noexcept
    -> Status {
  if (pipe_alpha_ == nullptr || pipe_add_ == nullptr ||
      pipe_layout_ == nullptr) {
    return MakeFail(StatusCode::kNotInitialized, "particles not built");
  }
  if (!mesh.ready() || mesh.index_count() == 0 ||
      mesh.vertex_buffer_bits() == 0 || mesh.index_buffer_bits() == 0) {
    return MakeFail(StatusCode::kInvalidArgument, "particles bad record arg");
  }
  // Range: within the caller's quad count AND within the uploaded index
  // buffer (6 indices per quad), in 64-bit so nothing wraps.
  const std::uint64_t end_quad =
      static_cast<std::uint64_t>(first_quad) + quad_count;
  if (quad_count == 0 || end_quad > total_quads ||
      end_quad * 6U > static_cast<std::uint64_t>(mesh.index_count())) {
    return MakeFail(StatusCode::kInvalidArgument, "particles draw range");
  }
  if (cmd == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "particles bad record arg");
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
  if (p_vkCmdBindVertexBuffers == nullptr || p_vkCmdBindIndexBuffer == nullptr ||
      p_vkCmdPushConstants == nullptr || p_vkCmdDrawIndexed == nullptr ||
      p_vkCmdBindPipeline == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed,
                    "particles no record entry");
  }
  p_vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      additive ? pipe_add_ : pipe_alpha_);
  VkBuffer vb = reinterpret_cast<VkBuffer>(
      static_cast<std::uintptr_t>(mesh.vertex_buffer_bits()));
  VkBuffer ib = reinterpret_cast<VkBuffer>(
      static_cast<std::uintptr_t>(mesh.index_buffer_bits()));
  const VkDeviceSize offsets[1] = {0};
  p_vkCmdBindVertexBuffers(cmd, 0, 1, &vb, offsets);
  p_vkCmdBindIndexBuffer(cmd, ib, 0, VK_INDEX_TYPE_UINT32);
  p_vkCmdPushConstants(cmd, pipe_layout_,
                       VK_SHADER_STAGE_VERTEX_BIT |
                           VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, static_cast<std::uint32_t>(sizeof(ParticlePush)),
                       &push);
  // Exactly one draw call per record_draw (the acceptance contract):
  // 6 indices per quad, offset at the first quad.
  p_vkCmdDrawIndexed(cmd, quad_count * 6, 1, first_quad * 6, 0, 0);
  return MakeOk();
}

auto ParticleRenderer::destroy() noexcept -> void {
  // Reference the create-time-only procs so Clang -Wunused-private-field
  // stays clean under -Werror (same pattern as SpriteRenderer).
  (void)proc_create_module_;
  (void)proc_create_pipelines_;
  (void)proc_create_layout_;
  (void)proc_bind_pipeline_;
  (void)proc_cmd_viewport_;
  (void)proc_cmd_scissor_;
  (void)proc_cmd_draw_indexed_;
  (void)proc_cmd_bind_vb_;
  (void)proc_cmd_bind_ib_;
  (void)proc_cmd_push_;
  (void)proc_begin_rendering_;
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  if (device_ == nullptr) {
    pipe_alpha_ = nullptr;
    pipe_add_ = nullptr;
    pipe_layout_ = nullptr;
    modules_[0] = nullptr;
    modules_[1] = nullptr;
    module_count_ = 0;
    ready_ = false;
    return;
  }
  auto p_vkDestroyPipeline = reinterpret_cast<PFN_vkDestroyPipeline>(
      proc_destroy_pipeline_);
  auto p_vkDestroyPipelineLayout =
      reinterpret_cast<PFN_vkDestroyPipelineLayout>(proc_destroy_layout_);
  auto p_vkDestroyShaderModule = reinterpret_cast<PFN_vkDestroyShaderModule>(
      proc_destroy_module_);
  if (p_vkDestroyPipeline != nullptr) {
    for (auto* p : {&pipe_alpha_, &pipe_add_}) {
      if (*p != nullptr) {
        p_vkDestroyPipeline(device_, *p, nullptr);
        *p = nullptr;
      }
    }
  }
  if (p_vkDestroyPipelineLayout != nullptr && pipe_layout_ != nullptr) {
    p_vkDestroyPipelineLayout(device_, pipe_layout_, nullptr);
    pipe_layout_ = nullptr;
  }
  if (p_vkDestroyShaderModule != nullptr) {
    for (auto* m : {&modules_[0], &modules_[1]}) {
      if (*m != nullptr) {
        p_vkDestroyShaderModule(device_, *m, nullptr);
        *m = nullptr;
      }
    }
  }
  (void)proc_end_rendering_;
  module_count_ = 0;
  device_ = nullptr;
  ready_ = false;
#else
  pipe_alpha_ = nullptr;
  pipe_add_ = nullptr;
  pipe_layout_ = nullptr;
  modules_[0] = nullptr;
  modules_[1] = nullptr;
  module_count_ = 0;
  ready_ = false;
#endif
}

#else

auto ParticleRenderer::create(VulkanDevice& /*dev*/, VkFormat /*color_format*/,
                              const std::uint32_t* /*vert_spv*/,
                              std::size_t /*vert_words*/,
                              const std::uint32_t* /*frag_spv*/,
                              std::size_t /*frag_words*/) noexcept -> Status {
  return MakeFail(StatusCode::kVulkanLoaderFailed, "no Vulkan SDK");
}

auto ParticleRenderer::destroy() noexcept -> void {
  pipe_alpha_ = nullptr;
  pipe_add_ = nullptr;
  pipe_layout_ = nullptr;
  modules_[0] = nullptr;
  modules_[1] = nullptr;
  module_count_ = 0;
  ready_ = false;
}

auto ParticleRenderer::begin_particle_pass(VkCommandBuffer /*cmd*/,
                                           VkImageView /*color_view*/,
                                           VkExtent2D /*extent*/) noexcept
    -> Status {
  return MakeFail(StatusCode::kNotInitialized, "particles not built");
}

auto ParticleRenderer::end_particle_pass(VkCommandBuffer /*cmd*/) noexcept
    -> Status {
  return MakeFail(StatusCode::kNotInitialized, "particles not built");
}

auto ParticleRenderer::record_draw(VkCommandBuffer /*cmd*/,
                                   const GpuMesh& /*mesh*/,
                                   const ParticlePush& /*push*/,
                                   bool /*additive*/,
                                   std::uint32_t /*first_quad*/,
                                   std::uint32_t /*quad_count*/,
                                   std::uint32_t /*total_quads*/) noexcept
    -> Status {
  return MakeFail(StatusCode::kNotInitialized, "particles not built");
}

#endif

}  // namespace engine::renderer

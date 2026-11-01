// gi_frame_graph.cpp — M3-04: GI shader modules + graphics pipelines.
//
// Real Vulkan path: creates 4 shader modules from the checked-in GI .inc
// blobs and builds 2 fullscreen graphics pipelines (gi_resolve RGBA16F,
// shadow_trace R32F) against the bindless pipeline layout, with dynamic
// rendering (VkPipelineRenderingCreateInfo, no render-pass objects).
// Entry points load via VulkanDevice::proc()/instance_proc() (same pattern
// as the Lavapipe probes: device-level vk* through proc(), instance-level
// through instance_proc()).
#include "engine/renderer/gi_frame_graph.hpp"

#include "engine/renderer/pbr_pipeline.hpp"

#if defined(ENGINE_RENDERER_HAS_VULKAN)
// volk-first include order (same rule as vulkan_device.cpp): volk defines
// VK_NO_PROTOTYPES and hard-errors if vulkan.h came first.
#include <volk.h>
#endif

#include "engine/renderer/vulkan_device.hpp"

namespace engine::renderer {

GiFrameGraph::~GiFrameGraph() noexcept { destroy(); }

#if defined(ENGINE_RENDERER_HAS_VULKAN)

namespace {

auto valid_spv(const std::uint32_t* w, std::size_t n) noexcept -> bool {
  return w != nullptr && n > 5 && w[0] == 0x07230203u && w[1] >= 0x00010300u;
}

}  // namespace

auto GiFrameGraph::build(
    const VulkanDevice& dev, VkDescriptorSetLayout bindless_set,
    const std::uint32_t* gi_vert, std::size_t gi_vert_words,
    const std::uint32_t* gi_frag, std::size_t gi_frag_words,
    const std::uint32_t* shadow_vert, std::size_t shadow_vert_words,
    const std::uint32_t* shadow_frag, std::size_t shadow_frag_words,
    const PbrPipeline& pbr) noexcept -> Status {
  if (ready_) {
    return MakeFail(StatusCode::kAlreadyInitialized, "gi graph built");
  }
  if (!pbr.ready()) {
    return MakeFail(StatusCode::kInvalidArgument, "gi graph needs pbr plan");
  }
  if (bindless_set == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument,
                    "gi graph needs bindless set layout");
  }
  const std::uint32_t* blobs[4] = {gi_vert, gi_frag, shadow_vert,
                                   shadow_frag};
  const std::size_t words[4] = {gi_vert_words, gi_frag_words,
                                shadow_vert_words, shadow_frag_words};
  for (int i = 0; i < 4; ++i) {
    if (!valid_spv(blobs[i], words[i])) {
      return MakeFail(StatusCode::kInvalidArgument, "gi graph bad blob");
    }
  }
  VkDevice device = dev.device();
  if (device == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "gi graph null device");
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
  if (p_vkCreateShaderModule == nullptr || p_vkDestroyShaderModule == nullptr ||
      p_vkCreateGraphicsPipelines == nullptr ||
      p_vkDestroyPipeline == nullptr || p_vkCreatePipelineLayout == nullptr ||
      p_vkDestroyPipelineLayout == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "gi graph no entry");
  }
  const char* names[4] = {"vsGiResolve", "fsGiResolve", "vsShadowTrace",
                          "fsShadowTrace"};
  (void)names;
  for (int i = 0; i < 4; ++i) {
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
      return MakeFail(StatusCode::kDeviceCreateFailed, "gi module failed");
    }
    ++module_count_;
  }
  // Two fullscreen-triangle pipelines (empty vertex input, triangle list,
  // no depth test, no blending): gi -> RGBA16F, shadow -> R32F.
  // Pipeline-creation failures roll back modules (no leaks).
  // Pipeline layout: dedicated GI layout over the SAME bindless set 0,
  // plus a set-1 pair for the GI volumes (probe/SDF sampled images,
  // texel-fetch only — no samplers). Set-1 bindings mirror the slang:
  //   set 1 binding 0: probe volume (gi) / SDF bricks (shadow),
  //   set 1 binding 1: probe SDF (gi only).
  // One fragment-stage push range sized for the larger GI push
  // (GiResolvePush 96B vs ShadowTracePush 64B — ranges only carry size,
  // so the max covers both; VUID-07987 needs the FRAGMENT bit because
  // both shaders declare push blocks).
  // NOTE: SAMPLER elision — the shaders use Texture.Load() texel fetches
  // (no Sample/SamplerState on the volume paths; the shadow atlas keeps
  // its sampler via the bindless sampler array), so the set-1 bindings
  // are SAMPLED_IMAGE (matching cull.comp's sampled-image pattern? no:
  // the sampled-image descriptor type), never COMBINED_IMAGE_SAMPLER.
  VkPipelineLayout pipe_layout = nullptr;
  VkDescriptorSetLayout set1_layout = nullptr;
  auto p_vkCreateSetLayout =
      reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(
          dev.instance_proc("vkCreateDescriptorSetLayout"));
  auto p_vkDestroySetLayout =
      reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(
          dev.instance_proc("vkDestroyDescriptorSetLayout"));
  if (p_vkCreateSetLayout == nullptr || p_vkDestroySetLayout == nullptr) {
    for (int j = 0; j < 4; ++j) {
      p_vkDestroyShaderModule(device, modules_[j], nullptr);
      modules_[j] = nullptr;
    }
    module_count_ = 0;
    return MakeFail(StatusCode::kVulkanLoaderFailed, "gi no set layout");
  }
  {
    VkDescriptorSetLayoutBinding set1_bindings[4]{};
    set1_bindings[0].binding = 0;
    set1_bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    set1_bindings[0].descriptorCount = 1;
    set1_bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    set1_bindings[1].binding = 1;
    set1_bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    set1_bindings[1].descriptorCount = 1;
    set1_bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    // Shadow atlas pair (shadow_trace.slang set-1 bindings 2/3): sampled
    // image + its sampler. The gi_resolve pass leaves bindings 2/3
    // unbound (unused set-1 bindings need no descriptors).
    set1_bindings[2].binding = 2;
    set1_bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    set1_bindings[2].descriptorCount = 1;
    set1_bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    set1_bindings[3].binding = 3;
    set1_bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    set1_bindings[3].descriptorCount = 1;
    set1_bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo set1_info{};
    set1_info.sType =
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set1_info.bindingCount = 4;
    set1_info.pBindings = set1_bindings;
    if (p_vkCreateSetLayout(device, &set1_info, nullptr, &set1_layout) !=
        VK_SUCCESS) {
      for (int j = 0; j < 4; ++j) {
        p_vkDestroyShaderModule(device, modules_[j], nullptr);
        modules_[j] = nullptr;
      }
      module_count_ = 0;
      return MakeFail(StatusCode::kDeviceCreateFailed, "gi set1 failed");
    }
  }
  {
    VkDescriptorSetLayout sets[2] = {bindless_set, set1_layout};
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset = 0;
    push.size = 96;  // GiResolvePush: 4x vec4 + 4x u32 + 2x float
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 2;
    pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    if (p_vkCreatePipelineLayout(device, &pli, nullptr, &pipe_layout) !=
        VK_SUCCESS) {
      p_vkDestroySetLayout(device, set1_layout, nullptr);
      set1_layout = nullptr;
      for (int j = 0; j < 4; ++j) {
        p_vkDestroyShaderModule(device, modules_[j], nullptr);
        modules_[j] = nullptr;
      }
      module_count_ = 0;
      return MakeFail(StatusCode::kDeviceCreateFailed, "gi pipe layout");
    }
  }
  auto build_pipe = [&](int vi, int fi, VkFormat color_fmt,
                        VkPipeline* out) -> Status {
    // SPIR-V 1.3 module path: slang emits SPV 1.3 (see validated blobs in
    // the .inc headers); dynamic rendering needs the
    // VkPipelineRenderingCreateInfo struct in pNext (Vulkan 1.3 core —
    // no feature flag on top of the dynamicRendering device feature
    // the device was created with).
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = modules_[vi];
    stages[0].pName = (vi == 0) ? "vsGiResolve" : "vsShadowTrace";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = modules_[fi];
    stages[1].pName = (vi == 0) ? "fsGiResolve" : "fsShadowTrace";

    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    // Fullscreen triangle: the vertex shader ignores vertex buffers and
    // emits clip-space positions from gl_VertexIndex (no point topology,
    // which would demand PointSize writes per VUID-08773).
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
    raster.lineWidth = 1.0f;

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
    rendering.pColorAttachmentFormats = &color_fmt;

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

    if (p_vkCreateGraphicsPipelines(device, nullptr, 1, &pci, nullptr, out) !=
        VK_SUCCESS) {
      return MakeFail(StatusCode::kDeviceCreateFailed, "gi pipe failed");
    }
    return MakeOk();
  };
  Status s = build_pipe(0, 1, VK_FORMAT_R16G16B16A16_SFLOAT, &gi_pipeline_);
  if (!s.ok()) {
    p_vkDestroyPipelineLayout(device, pipe_layout, nullptr);
    p_vkDestroySetLayout(device, set1_layout, nullptr);
    for (int j = 0; j < 4; ++j) {
      p_vkDestroyShaderModule(device, modules_[j], nullptr);
      modules_[j] = nullptr;
    }
    module_count_ = 0;
    return s;
  }
  s = build_pipe(2, 3, VK_FORMAT_R32_SFLOAT, &shadow_pipeline_);
  if (!s.ok()) {
    p_vkDestroyPipeline(device, gi_pipeline_, nullptr);
    gi_pipeline_ = nullptr;
    p_vkDestroyPipelineLayout(device, pipe_layout, nullptr);
    p_vkDestroySetLayout(device, set1_layout, nullptr);
    for (int j = 0; j < 4; ++j) {
      p_vkDestroyShaderModule(device, modules_[j], nullptr);
      modules_[j] = nullptr;
    }
    module_count_ = 0;
    return s;
  }
  pipe_layout_ = pipe_layout;
  set1_layout_ = set1_layout;
  proc_destroy_set_layout_ = reinterpret_cast<void*>(p_vkDestroySetLayout);
  device_ = device;
  proc_create_module_ = reinterpret_cast<void*>(p_vkCreateShaderModule);
  proc_destroy_module_ = reinterpret_cast<void*>(p_vkDestroyShaderModule);
  proc_create_pipelines_ =
      reinterpret_cast<void*>(p_vkCreateGraphicsPipelines);
  proc_destroy_pipeline_ = reinterpret_cast<void*>(p_vkDestroyPipeline);
  proc_create_layout_ = reinterpret_cast<void*>(p_vkCreatePipelineLayout);
  proc_destroy_layout_ = reinterpret_cast<void*>(p_vkDestroyPipelineLayout);
  ready_ = true;
  return MakeOk();
}

auto GiFrameGraph::destroy() noexcept -> void {
  // Reference the create-time-only procs so Clang -Wunused-private-field
  // stays clean under -Werror (they are intentionally write-only after
  // build; destroy only calls the destroy_* entry points). Every stored
  // field is touched: device_ (guard), all six procs (cast or call).
  (void)proc_create_module_;
  (void)proc_create_pipelines_;
  (void)proc_create_layout_;
  if (device_ == nullptr) {
    // Never created, or the device is already gone (same rule as
    // BindlessTable: never vkDestroy* on a dead device — the owning
    // VulkanDevice::shutdown() reclaims driver objects out from under
    // every live handle; see M3-02 Lavapipe exit segfault).
    modules_[0] = modules_[1] = modules_[2] = modules_[3] = nullptr;
    gi_pipeline_ = nullptr;
    shadow_pipeline_ = nullptr;
    pipe_layout_ = nullptr;
    set1_layout_ = nullptr;
    module_count_ = 0;
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
  auto p_vkDestroySetLayout =
      reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(
          proc_destroy_set_layout_);
  if (p_vkDestroyShaderModule != nullptr) {
    for (int i = 0; i < 4; ++i) {
      if (modules_[i] != nullptr) {
        p_vkDestroyShaderModule(device_, modules_[i], nullptr);
        modules_[i] = nullptr;
      }
    }
  }
  if (p_vkDestroyPipeline != nullptr) {
    if (gi_pipeline_ != nullptr) {
      p_vkDestroyPipeline(device_, gi_pipeline_, nullptr);
      gi_pipeline_ = nullptr;
    }
    if (shadow_pipeline_ != nullptr) {
      p_vkDestroyPipeline(device_, shadow_pipeline_, nullptr);
      shadow_pipeline_ = nullptr;
    }
  }
  if (p_vkDestroyPipelineLayout != nullptr && pipe_layout_ != nullptr) {
    p_vkDestroyPipelineLayout(device_, pipe_layout_, nullptr);
    pipe_layout_ = nullptr;
  }
  if (p_vkDestroySetLayout != nullptr && set1_layout_ != nullptr) {
    p_vkDestroySetLayout(device_, set1_layout_, nullptr);
    set1_layout_ = nullptr;
  }
  module_count_ = 0;
  ready_ = false;
  device_ = nullptr;
}

#else
using VkDescriptorSetLayout = void*;
// (VulkanDevice + Vk* stubs come from gi_frame_graph.hpp's own SDK-less
// aliases; no new include needed.)

auto GiFrameGraph::build(const VulkanDevice& /*dev*/,
                         VkDescriptorSetLayout /*bindless_set*/,
                         const std::uint32_t* /*gi_vert*/,
                         std::size_t /*gi_vert_words*/,
                         const std::uint32_t* /*gi_frag*/,
                         std::size_t /*gi_frag_words*/,
                         const std::uint32_t* /*shadow_vert*/,
                         std::size_t /*shadow_vert_words*/,
                         const std::uint32_t* /*shadow_frag*/,
                         std::size_t /*shadow_frag_words*/,
                         const PbrPipeline& /*pbr*/) noexcept -> Status {
  return MakeFail(StatusCode::kVulkanLoaderFailed, "no Vulkan SDK");
}

auto GiFrameGraph::destroy() noexcept -> void {
  modules_[0] = modules_[1] = modules_[2] = modules_[3] = nullptr;
  gi_pipeline_ = nullptr;
  shadow_pipeline_ = nullptr;
  pipe_layout_ = nullptr;
  set1_layout_ = nullptr;
  module_count_ = 0;
  ready_ = false;
}

#endif

}  // namespace engine::renderer

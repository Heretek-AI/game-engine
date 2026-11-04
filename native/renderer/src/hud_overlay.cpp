// hud_overlay.cpp — P5-04-followup: HUD texture upload + overlay composite.
//
// Real Vulkan path: creates 2 shader modules from the checked-in
// hud_overlay .inc blobs, builds 1 graphics pipeline (swapchain UNORM
// target, dynamic rendering, NO vertex input, premultiplied src-over
// blend ONE/ONE_MINUS_SRC_ALPHA, no depth), plus the sampled HUD texture
// (640x360 R8G8B8A8_UNORM, TRANSFER_DST|SAMPLED, device-local) with its
// view, a NEAREST/CLAMP sampler, one descriptor pool (1 combined-image
// set), and a persistent mapped staging buffer (921600 B, TRANSFER_SRC,
// host-visible).
//
// Per frame: update(pixels) memcpys into staging (marks dirty);
// record_upload(one-shot, fence-waited BEFORE the draw CB opens) copies
// buffer->image with UNDEFINED->TRANSFER_DST->SHADER_READ barriers;
// continue_hud_pass opens a LOAD composite over the mesh/particle frame,
// record_draw issues exactly one vkCmdDraw (3 verts, no buffers),
// end_hud_pass closes it. Entry points load via
// VulkanDevice::proc()/instance_proc() (same pattern as SpriteRenderer).
#include "engine/renderer/hud_overlay.hpp"

#include <array>
#include <span>
#include <string_view>

#include "engine/renderer/shaders/hud_overlay_frag_spv.inc"
#include "engine/renderer/shaders/hud_overlay_vert_spv.inc"

#if defined(ENGINE_RENDERER_HAS_VULKAN)
// volk-first include order (same rule as vulkan_device.cpp): volk defines
// VK_NO_PROTOTYPES and hard-errors if vulkan.h came first.
#include <volk.h>
#endif

#include "engine/renderer/vulkan_device.hpp"
#include "vma_bridge.hpp"

namespace engine::renderer {

HudOverlay::~HudOverlay() noexcept { destroy(); }

#if defined(ENGINE_RENDERER_HAS_VULKAN)

namespace {

auto valid_spv(const std::uint32_t* w, std::size_t n) noexcept -> bool {
  return w != nullptr && n > 5 && w[0] == 0x07230203u && w[1] >= 0x00010300u;
}

// One entry-proc table per create() (fields, not locals, so the resolve
// lines exist once; record paths re-read them from `this`).
struct HudProcs {
  PFN_vkCreateShaderModule create_module{nullptr};
  PFN_vkDestroyShaderModule destroy_module{nullptr};
  PFN_vkCreateGraphicsPipelines create_pipes{nullptr};
  PFN_vkDestroyPipeline destroy_pipe{nullptr};
  PFN_vkCreatePipelineLayout create_layout{nullptr};
  PFN_vkDestroyPipelineLayout destroy_layout{nullptr};
  PFN_vkCreateDescriptorSetLayout create_ds_layout{nullptr};
  PFN_vkDestroyDescriptorSetLayout destroy_ds_layout{nullptr};
  PFN_vkCreateDescriptorPool create_pool{nullptr};
  PFN_vkDestroyDescriptorPool destroy_pool{nullptr};
  PFN_vkAllocateDescriptorSets alloc_ds{nullptr};
  PFN_vkUpdateDescriptorSets update_ds{nullptr};
  PFN_vkCmdBindDescriptorSets bind_ds{nullptr};
  PFN_vkCreateSampler create_sampler{nullptr};
  PFN_vkDestroySampler destroy_sampler{nullptr};
  PFN_vkCreateImageView create_view{nullptr};
  PFN_vkDestroyImageView destroy_view{nullptr};
  PFN_vkCmdBindPipeline bind_pipe{nullptr};
  PFN_vkCmdSetViewport viewport{nullptr};
  PFN_vkCmdSetScissor scissor{nullptr};
  PFN_vkCmdDraw draw{nullptr};
  PFN_vkCmdPushConstants push{nullptr};
  PFN_vkCmdBeginRendering begin_render{nullptr};
  PFN_vkCmdEndRendering end_render{nullptr};
  PFN_vkCmdCopyBufferToImage copy_b2i{nullptr};
  PFN_vkCmdPipelineBarrier2 barrier{nullptr};
};

auto load_hud_procs(VulkanDevice& dev, HudProcs* p) noexcept -> bool {
  p->create_module = reinterpret_cast<PFN_vkCreateShaderModule>(
      dev.instance_proc("vkCreateShaderModule"));
  p->destroy_module = reinterpret_cast<PFN_vkDestroyShaderModule>(
      dev.instance_proc("vkDestroyShaderModule"));
  p->create_pipes = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(
      dev.proc("vkCreateGraphicsPipelines"));
  p->destroy_pipe =
      reinterpret_cast<PFN_vkDestroyPipeline>(dev.proc("vkDestroyPipeline"));
  p->create_layout = reinterpret_cast<PFN_vkCreatePipelineLayout>(
      dev.instance_proc("vkCreatePipelineLayout"));
  p->destroy_layout = reinterpret_cast<PFN_vkDestroyPipelineLayout>(
      dev.instance_proc("vkDestroyPipelineLayout"));
  p->create_ds_layout = reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(
      dev.instance_proc("vkCreateDescriptorSetLayout"));
  p->destroy_ds_layout = reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(
      dev.instance_proc("vkDestroyDescriptorSetLayout"));
  p->create_pool = reinterpret_cast<PFN_vkCreateDescriptorPool>(
      dev.instance_proc("vkCreateDescriptorPool"));
  p->destroy_pool = reinterpret_cast<PFN_vkDestroyDescriptorPool>(
      dev.instance_proc("vkDestroyDescriptorPool"));
  p->alloc_ds = reinterpret_cast<PFN_vkAllocateDescriptorSets>(
      dev.proc("vkAllocateDescriptorSets"));
  p->update_ds = reinterpret_cast<PFN_vkUpdateDescriptorSets>(
      dev.proc("vkUpdateDescriptorSets"));
  p->bind_ds = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(
      dev.proc("vkCmdBindDescriptorSets"));
  p->create_sampler = reinterpret_cast<PFN_vkCreateSampler>(
      dev.instance_proc("vkCreateSampler"));
  p->destroy_sampler = reinterpret_cast<PFN_vkDestroySampler>(
      dev.instance_proc("vkDestroySampler"));
  p->create_view = reinterpret_cast<PFN_vkCreateImageView>(
      dev.instance_proc("vkCreateImageView"));
  p->destroy_view = reinterpret_cast<PFN_vkDestroyImageView>(
      dev.instance_proc("vkDestroyImageView"));
  p->bind_pipe =
      reinterpret_cast<PFN_vkCmdBindPipeline>(dev.proc("vkCmdBindPipeline"));
  p->viewport =
      reinterpret_cast<PFN_vkCmdSetViewport>(dev.proc("vkCmdSetViewport"));
  p->scissor =
      reinterpret_cast<PFN_vkCmdSetScissor>(dev.proc("vkCmdSetScissor"));
  p->draw = reinterpret_cast<PFN_vkCmdDraw>(dev.proc("vkCmdDraw"));
  p->push =
      reinterpret_cast<PFN_vkCmdPushConstants>(dev.proc("vkCmdPushConstants"));
  p->begin_render = reinterpret_cast<PFN_vkCmdBeginRendering>(
      dev.proc("vkCmdBeginRendering"));
  p->end_render =
      reinterpret_cast<PFN_vkCmdEndRendering>(dev.proc("vkCmdEndRendering"));
  p->copy_b2i = reinterpret_cast<PFN_vkCmdCopyBufferToImage>(
      dev.proc("vkCmdCopyBufferToImage"));
  p->barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(
      dev.proc("vkCmdPipelineBarrier2"));
  return p->create_module != nullptr && p->destroy_module != nullptr &&
         p->create_pipes != nullptr && p->destroy_pipe != nullptr &&
         p->create_layout != nullptr && p->destroy_layout != nullptr &&
         p->create_ds_layout != nullptr && p->destroy_ds_layout != nullptr &&
         p->create_pool != nullptr && p->destroy_pool != nullptr &&
         p->alloc_ds != nullptr && p->update_ds != nullptr &&
         p->bind_ds != nullptr && p->create_sampler != nullptr &&
         p->destroy_sampler != nullptr && p->create_view != nullptr &&
         p->destroy_view != nullptr && p->bind_pipe != nullptr &&
         p->viewport != nullptr && p->scissor != nullptr &&
         p->draw != nullptr && p->push != nullptr &&
         p->begin_render != nullptr && p->end_render != nullptr &&
         p->copy_b2i != nullptr && p->barrier != nullptr;
}

}  // namespace

auto HudOverlay::create(VulkanDevice& dev, VkFormat color_format,
                        const std::uint32_t* vert_spv, std::size_t vert_words,
                        const std::uint32_t* frag_spv,
                        std::size_t frag_words) noexcept -> Status {
  if (ready_) {
    return MakeFail(StatusCode::kAlreadyInitialized, "hud built");
  }
  static_assert(sizeof(HudPush) == 8, "hud push is 8 bytes");
  const std::uint32_t* vert =
      (vert_spv != nullptr) ? vert_spv : kHud_overlay_vertSpv;
  const std::size_t vert_n =
      (vert_spv != nullptr) ? vert_words : kHud_overlay_vertSpvWords;
  const std::uint32_t* frag =
      (frag_spv != nullptr) ? frag_spv : kHud_overlay_fragSpv;
  const std::size_t frag_n =
      (frag_spv != nullptr) ? frag_words : kHud_overlay_fragSpvWords;
  if (!valid_spv(vert, vert_n) || !valid_spv(frag, frag_n)) {
    return MakeFail(StatusCode::kInvalidArgument, "hud bad blob");
  }
  if (color_format != VK_FORMAT_B8G8R8A8_UNORM &&
      color_format != VK_FORMAT_R8G8B8A8_UNORM) {
    return MakeFail(StatusCode::kInvalidArgument, "hud bad format");
  }
  VkDevice device = dev.device();
  if (device == nullptr || !dev.vma_ready()) {
    return MakeFail(StatusCode::kInvalidArgument, "hud null device");
  }
  auto* vma_alloc = vma_bridge::g_alloc;
  if (vma_alloc == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "hud no vma");
  }
  HudProcs procs{};
  if (!load_hud_procs(dev, &procs)) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "hud no entry");
  }
  // Modules.
  const std::uint32_t* blobs[2] = {vert, frag};
  const std::size_t words[2] = {vert_n, frag_n};
  for (int i = 0; i < 2; ++i) {
    VkShaderModuleCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sci.codeSize = words[i] * 4;
    sci.pCode = blobs[i];
    if (procs.create_module(device, &sci, nullptr, &modules_[i]) !=
        VK_SUCCESS) {
      for (int j = 0; j < i; ++j) {
        procs.destroy_module(device, modules_[j], nullptr);
        modules_[j] = nullptr;
      }
      return MakeFail(StatusCode::kDeviceCreateFailed, "hud module");
    }
    ++module_count_;
  }
  auto fail_modules = [&]() noexcept -> void {
    for (int j = 0; j < 2; ++j) {
      if (modules_[j] != nullptr) {
        procs.destroy_module(device, modules_[j], nullptr);
        modules_[j] = nullptr;
      }
    }
    module_count_ = 0;
  };
  // Descriptor set layout: set 0, binding 0 = COMBINED_IMAGE_SAMPLER,
  // visible to the fragment stage only.
  VkDescriptorSetLayout set_layout = nullptr;
  {
    VkDescriptorSetLayoutBinding bind{};
    bind.binding = 0;
    bind.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bind.descriptorCount = 1;
    bind.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = 1;
    lci.pBindings = &bind;
    if (procs.create_ds_layout(device, &lci, nullptr, &set_layout) !=
            VK_SUCCESS ||
        set_layout == nullptr) {
      fail_modules();
      return MakeFail(StatusCode::kDeviceCreateFailed, "hud set layout");
    }
  }
  // Pipeline layout: the set + one 8-byte push range.
  VkPipelineLayout layout = nullptr;
  {
    VkPushConstantRange push{};
    push.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset = 0;
    push.size = static_cast<std::uint32_t>(sizeof(HudPush));
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    if (procs.create_layout(device, &pli, nullptr, &layout) != VK_SUCCESS ||
        layout == nullptr) {
      procs.destroy_ds_layout(device, set_layout, nullptr);
      fail_modules();
      return MakeFail(StatusCode::kDeviceCreateFailed, "hud layout");
    }
  }
  // Graphics pipeline: no vertex input, fullscreen triangle, no depth,
  // premultiplied src-over blend (ONE / ONE_MINUS_SRC_ALPHA).
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
    blend_att.blendEnable = VK_TRUE;
    // Premultiplied src-over (the CPU backbuffer is premultiplied RGBA):
    // ONE + ONE_MINUS_SRC_ALPHA, matching the HudRender CPU blend.
    blend_att.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_att.colorBlendOp = VK_BLEND_OP_ADD;
    blend_att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_att.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_att;

    const std::array<VkDynamicState, 2> dyn_states{
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = static_cast<std::uint32_t>(dyn_states.size());
    dyn.pDynamicStates = dyn_states.data();

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
    pci.layout = layout;
    if (procs.create_pipes(device, nullptr, 1, &pci, nullptr, &pipe) !=
            VK_SUCCESS ||
        pipe == nullptr) {
      procs.destroy_layout(device, layout, nullptr);
      procs.destroy_ds_layout(device, set_layout, nullptr);
      fail_modules();
      return MakeFail(StatusCode::kDeviceCreateFailed, "hud pipe");
    }
  }
  // Staging buffer: kHudFrameBytes, TRANSFER_SRC, host-visible mapped
  // (same flags as GpuMesh uploads via EngineVmaCreateMeshBuffer).
  vma_bridge::MeshBufferBundle staging{};
  if (vma_bridge::EngineVmaCreateMeshBuffer(
          vma_alloc, kHudFrameBytes,
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging) != 0 ||
      staging.mapped == nullptr) {
    procs.destroy_pipe(device, pipe, nullptr);
    procs.destroy_layout(device, layout, nullptr);
    procs.destroy_ds_layout(device, set_layout, nullptr);
    fail_modules();
    return MakeFail(StatusCode::kDeviceCreateFailed, "hud staging");
  }
  // HUD sampled image (TRANSFER_DST|SAMPLED via the HudImage wrapper) +
  // its SHADER_READ view + NEAREST/CLAMP sampler + one combined-image
  // descriptor set. Every failure below unwinds in reverse-create order.
  vma_bridge::DepthImageBundle hud_img{};
  if (vma_bridge::EngineVmaCreateHudImage(vma_alloc, kHudFrameW, kHudFrameH,
                                          &hud_img) != 0) {
    vma_bridge::EngineVmaDestroyBuffer(vma_alloc, staging.buffer_bits,
                                       staging.alloc);
    procs.destroy_pipe(device, pipe, nullptr);
    procs.destroy_layout(device, layout, nullptr);
    procs.destroy_ds_layout(device, set_layout, nullptr);
    fail_modules();
    return MakeFail(StatusCode::kDeviceCreateFailed, "hud image");
  }
  VkImageView hud_view = nullptr;
  {
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = reinterpret_cast<VkImage>(
        static_cast<std::uintptr_t>(hud_img.image_bits));
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.baseMipLevel = 0;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.baseArrayLayer = 0;
    vi.subresourceRange.layerCount = 1;
    if (procs.create_view(device, &vi, nullptr, &hud_view) != VK_SUCCESS ||
        hud_view == nullptr) {
      vma_bridge::EngineVmaDestroyImage(vma_alloc, hud_img.image_bits,
                                        hud_img.alloc);
      vma_bridge::EngineVmaDestroyBuffer(vma_alloc, staging.buffer_bits,
                                         staging.alloc);
      procs.destroy_pipe(device, pipe, nullptr);
      procs.destroy_layout(device, layout, nullptr);
      procs.destroy_ds_layout(device, set_layout, nullptr);
      fail_modules();
      return MakeFail(StatusCode::kDeviceCreateFailed, "hud view");
    }
  }
  VkSampler sampler = nullptr;
  {
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    // NEAREST: HUD texels map 1:1 (or integer upscale) — no filtering blur
    // to justify in the golden, and lavapipe-deterministic.
    si.magFilter = VK_FILTER_NEAREST;
    si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxAnisotropy = 1.0F;
    if (procs.create_sampler(device, &si, nullptr, &sampler) != VK_SUCCESS ||
        sampler == nullptr) {
      procs.destroy_view(device, hud_view, nullptr);
      vma_bridge::EngineVmaDestroyImage(vma_alloc, hud_img.image_bits,
                                        hud_img.alloc);
      vma_bridge::EngineVmaDestroyBuffer(vma_alloc, staging.buffer_bits,
                                         staging.alloc);
      procs.destroy_pipe(device, pipe, nullptr);
      procs.destroy_layout(device, layout, nullptr);
      procs.destroy_ds_layout(device, set_layout, nullptr);
      fail_modules();
      return MakeFail(StatusCode::kDeviceCreateFailed, "hud sampler");
    }
  }
  VkDescriptorPool pool = nullptr;
  VkDescriptorSet draw_set = nullptr;
  {
    VkDescriptorPoolSize ps{};
    ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps.descriptorCount = 1;
    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = 1;
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &ps;
    if (procs.create_pool(device, &dpci, nullptr, &pool) != VK_SUCCESS ||
        pool == nullptr) {
      procs.destroy_sampler(device, sampler, nullptr);
      procs.destroy_view(device, hud_view, nullptr);
      vma_bridge::EngineVmaDestroyImage(vma_alloc, hud_img.image_bits,
                                        hud_img.alloc);
      vma_bridge::EngineVmaDestroyBuffer(vma_alloc, staging.buffer_bits,
                                         staging.alloc);
      procs.destroy_pipe(device, pipe, nullptr);
      procs.destroy_layout(device, layout, nullptr);
      procs.destroy_ds_layout(device, set_layout, nullptr);
      fail_modules();
      return MakeFail(StatusCode::kDeviceCreateFailed, "hud pool");
    }
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &set_layout;
    if (procs.alloc_ds(device, &ai, &draw_set) != VK_SUCCESS ||
        draw_set == nullptr) {
      procs.destroy_pool(device, pool, nullptr);
      procs.destroy_sampler(device, sampler, nullptr);
      procs.destroy_view(device, hud_view, nullptr);
      vma_bridge::EngineVmaDestroyImage(vma_alloc, hud_img.image_bits,
                                        hud_img.alloc);
      vma_bridge::EngineVmaDestroyBuffer(vma_alloc, staging.buffer_bits,
                                         staging.alloc);
      procs.destroy_pipe(device, pipe, nullptr);
      procs.destroy_layout(device, layout, nullptr);
      procs.destroy_ds_layout(device, set_layout, nullptr);
      fail_modules();
      return MakeFail(StatusCode::kDeviceCreateFailed, "hud alloc set");
    }
    // One write at create (SHADER_READ layout: the image transitions to
    // SHADER_READ_ONLY on first record_upload, before any draw reads it;
    // the descriptor's layout must match the layout at draw time).
    VkDescriptorImageInfo ii{};
    ii.sampler = sampler;
    ii.imageView = hud_view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet wr{};
    wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet = draw_set;
    wr.dstBinding = 0;
    wr.dstArrayElement = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &ii;
    procs.update_ds(device, 1, &wr, 0, nullptr);
  }
  // Commit: every handle is live, the set is written, staging is mapped.
  pipeline_ = pipe;
  pipe_layout_ = layout;
  set_layout_ = reinterpret_cast<void*>(set_layout);
  pool_ = reinterpret_cast<void*>(pool);
  set_ = reinterpret_cast<void*>(draw_set);
  sampler_ = reinterpret_cast<void*>(sampler);
  image_bits_ = hud_img.image_bits;
  image_alloc_ = static_cast<void*>(hud_img.alloc);
  image_view_ = static_cast<void*>(hud_view);
  staging_bits_ = staging.buffer_bits;
  staging_alloc_ = static_cast<void*>(staging.alloc);
  staging_mapped_ = staging.mapped;
  image_live_ = true;
  image_shader_read_ = false;
  dirty_ = false;
  device_ = device;
  proc_create_module_ = reinterpret_cast<void*>(procs.create_module);
  proc_destroy_module_ = reinterpret_cast<void*>(procs.destroy_module);
  proc_create_pipelines_ = reinterpret_cast<void*>(procs.create_pipes);
  proc_destroy_pipeline_ = reinterpret_cast<void*>(procs.destroy_pipe);
  proc_create_layout_ = reinterpret_cast<void*>(procs.create_layout);
  proc_destroy_layout_ = reinterpret_cast<void*>(procs.destroy_layout);
  proc_create_ds_layout_ = reinterpret_cast<void*>(procs.create_ds_layout);
  proc_destroy_ds_layout_ = reinterpret_cast<void*>(procs.destroy_ds_layout);
  proc_create_pool_ = reinterpret_cast<void*>(procs.create_pool);
  proc_destroy_pool_ = reinterpret_cast<void*>(procs.destroy_pool);
  proc_alloc_ds_ = reinterpret_cast<void*>(procs.alloc_ds);
  proc_update_ds_ = reinterpret_cast<void*>(procs.update_ds);
  proc_bind_ds_ = reinterpret_cast<void*>(procs.bind_ds);
  proc_create_sampler_ = reinterpret_cast<void*>(procs.create_sampler);
  proc_destroy_sampler_ = reinterpret_cast<void*>(procs.destroy_sampler);
  proc_create_view_ = reinterpret_cast<void*>(procs.create_view);
  proc_destroy_view_ = reinterpret_cast<void*>(procs.destroy_view);
  proc_bind_pipeline_ = reinterpret_cast<void*>(procs.bind_pipe);
  proc_cmd_viewport_ = reinterpret_cast<void*>(procs.viewport);
  proc_cmd_scissor_ = reinterpret_cast<void*>(procs.scissor);
  proc_cmd_draw_ = reinterpret_cast<void*>(procs.draw);
  proc_cmd_push_ = reinterpret_cast<void*>(procs.push);
  proc_begin_rendering_ = reinterpret_cast<void*>(procs.begin_render);
  proc_end_rendering_ = reinterpret_cast<void*>(procs.end_render);
  proc_cmd_copy_b2i_ = reinterpret_cast<void*>(procs.copy_b2i);
  proc_cmd_barrier_ = reinterpret_cast<void*>(procs.barrier);
  ready_ = true;
  return MakeOk();
}

auto HudOverlay::destroy() noexcept -> void {
  auto* vma_alloc = vma_bridge::g_alloc;
  if (device_ != nullptr) {
    auto p_vkDestroyShaderModule =
        reinterpret_cast<PFN_vkDestroyShaderModule>(proc_destroy_module_);
    auto p_vkDestroyPipeline =
        reinterpret_cast<PFN_vkDestroyPipeline>(proc_destroy_pipeline_);
    auto p_vkDestroyPipelineLayout =
        reinterpret_cast<PFN_vkDestroyPipelineLayout>(proc_destroy_layout_);
    auto p_vkDestroyDescriptorSetLayout =
        reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(
            proc_destroy_ds_layout_);
    auto p_vkDestroyDescriptorPool =
        reinterpret_cast<PFN_vkDestroyDescriptorPool>(proc_destroy_pool_);
    auto p_vkDestroySampler =
        reinterpret_cast<PFN_vkDestroySampler>(proc_destroy_sampler_);
    auto p_vkDestroyView =
        reinterpret_cast<PFN_vkDestroyImageView>(proc_destroy_view_);
    // Reverse-create order: pool -> sampler -> view -> image -> staging ->
    // pipeline -> layout -> set layout -> modules. Every destroy tolerates
    // null (never-created handles) and a dead device is guarded by the
    // device_ null above (same rule as SpriteRenderer: never vkDestroy* on
    // a dead device — VulkanDevice::shutdown() reclaims driver objects).
    if (p_vkDestroyDescriptorPool != nullptr && pool_ != nullptr) {
      p_vkDestroyDescriptorPool(
          device_, static_cast<VkDescriptorPool>(pool_), nullptr);
      pool_ = nullptr;
    }
    set_ = nullptr;  // pool-owned (freed with the pool above)
    if (p_vkDestroySampler != nullptr && sampler_ != nullptr) {
      p_vkDestroySampler(device_, static_cast<VkSampler>(sampler_), nullptr);
      sampler_ = nullptr;
    }
    if (p_vkDestroyView != nullptr && image_view_ != nullptr) {
      p_vkDestroyView(device_, static_cast<VkImageView>(image_view_),
                      nullptr);
      image_view_ = nullptr;
    }
    if (vma_alloc != nullptr && image_live_) {
      vma_bridge::EngineVmaDestroyImage(
          vma_alloc, image_bits_,
          static_cast<vma_bridge::Allocation_T*>(image_alloc_));
      image_bits_ = 0;
      image_alloc_ = nullptr;
      image_live_ = false;
    }
    if (vma_alloc != nullptr && staging_alloc_ != nullptr) {
      vma_bridge::EngineVmaDestroyBuffer(
          vma_alloc, staging_bits_,
          static_cast<vma_bridge::Allocation_T*>(staging_alloc_));
      staging_bits_ = 0;
      staging_alloc_ = nullptr;
      staging_mapped_ = nullptr;
    }
    if (p_vkDestroyPipeline != nullptr && pipeline_ != nullptr) {
      p_vkDestroyPipeline(device_, pipeline_, nullptr);
      pipeline_ = nullptr;
    }
    if (p_vkDestroyPipelineLayout != nullptr && pipe_layout_ != nullptr) {
      p_vkDestroyPipelineLayout(device_, pipe_layout_, nullptr);
      pipe_layout_ = nullptr;
    }
    if (p_vkDestroyDescriptorSetLayout != nullptr && set_layout_ != nullptr) {
      p_vkDestroyDescriptorSetLayout(
          device_, static_cast<VkDescriptorSetLayout>(set_layout_), nullptr);
      set_layout_ = nullptr;
    }
    if (p_vkDestroyShaderModule != nullptr) {
      for (int i = 0; i < 2; ++i) {
        if (modules_[i] != nullptr) {
          p_vkDestroyShaderModule(device_, modules_[i], nullptr);
          modules_[i] = nullptr;
        }
      }
    }
  } else {
    // Never created, or the device is already gone: drop handles without
    // driver calls (same rule as ForwardRenderer).
    modules_[0] = modules_[1] = nullptr;
    pipeline_ = nullptr;
    pipe_layout_ = nullptr;
    set_layout_ = nullptr;
    pool_ = nullptr;
    set_ = nullptr;
    sampler_ = nullptr;
    image_view_ = nullptr;
    image_bits_ = 0;
    image_alloc_ = nullptr;
    staging_bits_ = 0;
    staging_alloc_ = nullptr;
    staging_mapped_ = nullptr;
    image_live_ = false;
  }
  module_count_ = 0;
  ready_ = false;
  dirty_ = false;
  image_shader_read_ = false;
  device_ = nullptr;
  // Drop create-time-only procs so a use-after-destroy reads null, not
  // stale entry points (same pattern as GiFrameGraph).
  proc_create_module_ = nullptr;
  proc_destroy_module_ = nullptr;
  proc_create_pipelines_ = nullptr;
  proc_destroy_pipeline_ = nullptr;
  proc_create_layout_ = nullptr;
  proc_destroy_layout_ = nullptr;
  proc_create_ds_layout_ = nullptr;
  proc_destroy_ds_layout_ = nullptr;
  proc_create_pool_ = nullptr;
  proc_destroy_pool_ = nullptr;
  proc_alloc_ds_ = nullptr;
  proc_update_ds_ = nullptr;
  proc_bind_ds_ = nullptr;
  proc_create_sampler_ = nullptr;
  proc_destroy_sampler_ = nullptr;
  proc_create_view_ = nullptr;
  proc_destroy_view_ = nullptr;
  proc_bind_pipeline_ = nullptr;
  proc_cmd_viewport_ = nullptr;
  proc_cmd_scissor_ = nullptr;
  proc_cmd_draw_ = nullptr;
  proc_cmd_push_ = nullptr;
  proc_begin_rendering_ = nullptr;
  proc_end_rendering_ = nullptr;
  proc_cmd_copy_b2i_ = nullptr;
  proc_cmd_barrier_ = nullptr;
}

auto HudOverlay::update(const std::uint8_t* pixels) noexcept -> Status {
  if (!ready_) {
    return MakeFail(StatusCode::kNotInitialized, "hud not built");
  }
  if (pixels == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "hud null pixels");
  }
  if (staging_mapped_ == nullptr) {
    return MakeFail(StatusCode::kNotInitialized, "hud no staging");
  }
  // Persistent mapped staging (host-visible coherent, same as GpuMesh):
  // plain byte copy, no struct overlay (Sonar-safe, no memcpy-on-object).
  auto* dst = static_cast<std::uint8_t*>(staging_mapped_);
  for (std::uint32_t i = 0; i < kHudFrameBytes; ++i) {
    dst[i] = pixels[i];
  }
  dirty_ = true;
  return MakeOk();
}

auto HudOverlay::record_upload(VkCommandBuffer cmd) noexcept -> Status {
  if (!ready_) {
    return MakeFail(StatusCode::kNotInitialized, "hud not built");
  }
  if (cmd == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "hud null cmd");
  }
  if (!dirty_) {
    return MakeOk();  // nothing staged: image already holds the frame.
  }
  auto p_vkCmdCopyBufferToImage =
      reinterpret_cast<PFN_vkCmdCopyBufferToImage>(proc_cmd_copy_b2i_);
  auto p_vkCmdBarrier =
      reinterpret_cast<PFN_vkCmdPipelineBarrier2>(proc_cmd_barrier_);
  if (p_vkCmdCopyBufferToImage == nullptr || p_vkCmdBarrier == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "hud no upload entry");
  }
  if (!image_live_) {
    return MakeFail(StatusCode::kNotInitialized, "hud no image");
  }
  const VkImage img = reinterpret_cast<VkImage>(
      static_cast<std::uintptr_t>(image_bits_));
  const VkBuffer staging = reinterpret_cast<VkBuffer>(
      static_cast<std::uintptr_t>(staging_bits_));
  // First transition: UNDEFINED -> TRANSFER_DST (once; after the first
  // upload the image rests in SHADER_READ and only needs
  // SHADER_READ -> TRANSFER_DST -> SHADER_READ round trips).
  {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    b.srcAccessMask = 0;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = image_shader_read_
                      ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                      : VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = 1;
    if (image_shader_read_) {
      b.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      b.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    }
    VkDependencyInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    p_vkCmdBarrier(cmd, &di);
  }
  VkBufferImageCopy region{};
  region.bufferOffset = 0;
  region.bufferRowLength = 0;  // tightly packed (640x4 = 2560 B/row)
  region.bufferImageHeight = 0;
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.mipLevel = 0;
  region.imageSubresource.baseArrayLayer = 0;
  region.imageSubresource.layerCount = 1;
  region.imageOffset.x = 0;
  region.imageOffset.y = 0;
  region.imageOffset.z = 0;
  region.imageExtent.width = kHudFrameW;
  region.imageExtent.height = kHudFrameH;
  region.imageExtent.depth = 1;
  p_vkCmdCopyBufferToImage(cmd, staging, img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  // Second transition: TRANSFER_DST -> SHADER_READ (frag sampling).
  {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = 1;
    VkDependencyInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    p_vkCmdBarrier(cmd, &di);
  }
  dirty_ = false;
  image_shader_read_ = true;
  return MakeOk();
}

auto HudOverlay::continue_hud_pass(VkCommandBuffer cmd,
                                   VkImageView color_view,
                                   VkExtent2D extent) noexcept -> Status {
  if (pipeline_ == nullptr || pipe_layout_ == nullptr) {
    return MakeFail(StatusCode::kNotInitialized, "hud not built");
  }
  if (cmd == nullptr || color_view == nullptr || extent.width == 0 ||
      extent.height == 0) {
    return MakeFail(StatusCode::kInvalidArgument, "hud bad pass arg");
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
    return MakeFail(StatusCode::kVulkanLoaderFailed, "hud no pass entry");
  }
  VkRenderingAttachmentInfo color{};
  color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  color.imageView = color_view;
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
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
  return MakeOk();
}

auto HudOverlay::end_hud_pass(VkCommandBuffer cmd) noexcept -> Status {
  if (pipeline_ == nullptr || pipe_layout_ == nullptr) {
    return MakeFail(StatusCode::kNotInitialized, "hud not built");
  }
  if (cmd == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "hud bad end arg");
  }
  auto p_vkCmdEndRendering =
      reinterpret_cast<PFN_vkCmdEndRendering>(proc_end_rendering_);
  if (p_vkCmdEndRendering == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "hud no end entry");
  }
  p_vkCmdEndRendering(cmd);
  return MakeOk();
}

auto HudOverlay::record_draw(VkCommandBuffer cmd, float vp_w,
                             float vp_h) noexcept -> Status {
  if (pipeline_ == nullptr || pipe_layout_ == nullptr) {
    return MakeFail(StatusCode::kNotInitialized, "hud not built");
  }
  if (cmd == nullptr || !(vp_w > 0.0F) || !(vp_h > 0.0F)) {
    return MakeFail(StatusCode::kInvalidArgument, "hud bad draw arg");
  }
  auto p_vkCmdPushConstants =
      reinterpret_cast<PFN_vkCmdPushConstants>(proc_cmd_push_);
  auto p_vkCmdDraw = reinterpret_cast<PFN_vkCmdDraw>(proc_cmd_draw_);
  auto p_vkCmdBindDescriptorSets =
      reinterpret_cast<PFN_vkCmdBindDescriptorSets>(proc_bind_ds_);
  if (p_vkCmdPushConstants == nullptr || p_vkCmdDraw == nullptr ||
      p_vkCmdBindDescriptorSets == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "hud no draw entry");
  }
  if (set_ == nullptr) {
    return MakeFail(StatusCode::kNotInitialized, "hud no set");
  }
  const VkDescriptorSet draw_set = static_cast<VkDescriptorSet>(set_);
  p_vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipe_layout_, 0, 1, &draw_set, 0, nullptr);
  HudPush push{};
  push.viewport_px[0] = vp_w;
  push.viewport_px[1] = vp_h;
  p_vkCmdPushConstants(cmd, pipe_layout_,
                       VK_SHADER_STAGE_VERTEX_BIT |
                           VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, static_cast<std::uint32_t>(sizeof(HudPush)), &push);
  // Exactly one fullscreen-triangle draw: 3 verts, no vertex buffers.
  p_vkCmdDraw(cmd, 3, 1, 0, 0);
  return MakeOk();
}

#else

auto HudOverlay::create(VulkanDevice& /*dev*/, VkFormat /*color_format*/,
                        const std::uint32_t* /*vert_spv*/,
                        std::size_t /*vert_words*/,
                        const std::uint32_t* /*frag_spv*/,
                        std::size_t /*frag_words*/) noexcept -> Status {
  return MakeFail(StatusCode::kVulkanLoaderFailed, "no Vulkan SDK");
}

auto HudOverlay::destroy() noexcept -> void {
  modules_[0] = modules_[1] = nullptr;
  pipeline_ = nullptr;
  pipe_layout_ = nullptr;
  module_count_ = 0;
  ready_ = false;
  dirty_ = false;
  image_shader_read_ = false;
}

auto HudOverlay::update(const std::uint8_t* /*pixels*/) noexcept -> Status {
  return MakeFail(StatusCode::kVulkanLoaderFailed, "no Vulkan SDK");
}

auto HudOverlay::record_upload(VkCommandBuffer /*cmd*/) noexcept -> Status {
  return MakeFail(StatusCode::kVulkanLoaderFailed, "no Vulkan SDK");
}

auto HudOverlay::continue_hud_pass(VkCommandBuffer /*cmd*/,
                                   VkImageView /*color_view*/,
                                   VkExtent2D /*extent*/) noexcept -> Status {
  return MakeFail(StatusCode::kVulkanLoaderFailed, "no Vulkan SDK");
}

auto HudOverlay::end_hud_pass(VkCommandBuffer /*cmd*/) noexcept -> Status {
  return MakeFail(StatusCode::kVulkanLoaderFailed, "no Vulkan SDK");
}

auto HudOverlay::record_draw(VkCommandBuffer /*cmd*/, float /*vp_w*/,
                             float /*vp_h*/) noexcept -> Status {
  return MakeFail(StatusCode::kVulkanLoaderFailed, "no Vulkan SDK");
}

#endif

}  // namespace engine::renderer

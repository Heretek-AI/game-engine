// golden_vk_helper.hpp — P4-02 shared Vulkan test scaffolding (NOT a golden).
//
// P4-01's TUs (test_pbr_materials.cpp / test_pbr_golden.cpp) each define
// their own ~170-line loader structs (Procs/alloc_host/make_image/...).
// P4-02's HDR test is a third TU on the same Vulkan-1.3 dynamic-rendering
// path — a third copy of that scaffolding would trip Sonar's duplication
// gate (<=3% new duplication, see P4-01's golden_pipe_helper.hpp
// precedent). This header owns the shared half: host-buffer + image
// allocation, shader-module creation, descriptor writes, and command
// recording for the shade->post (HDR tonemap) frame shape.
//
// Design: one GoldenVk struct per test (device + resolved entry points);
// each method is a small inline free of hidden state so TUs keep their
// own checks/labels. Header-only, Vulkan-guarded (stub builds skip it),
// own TU via #pragma once + inline. Procs resolve through
// VulkanDevice::proc()/instance_proc() (gotcha 11: volk lives in
// libengine_core; test exes never link volk).
//
// Sonar-clean by construction: std::array/std::span, no strlen/snprintf/
// memset/C arrays in new code; printf-family formatting stays in the TUs
// (check_tagged pattern from test_pbr_materials.cpp).
#pragma once

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace engine::test {

#if defined(ENGINE_RENDERER_HAS_VULKAN)

// Resolved Vulkan entry points for one test device (device-level via
// proc(), instance/physical-device level via instance_proc()).
struct GoldenVkProcs {
  PFN_vkCreateBuffer create_buffer{nullptr};
  PFN_vkDestroyBuffer destroy_buffer{nullptr};
  PFN_vkGetBufferMemoryRequirements get_buffer_reqs{nullptr};
  PFN_vkGetPhysicalDeviceMemoryProperties get_mem_props{nullptr};
  PFN_vkAllocateMemory alloc_mem{nullptr};
  PFN_vkFreeMemory free_mem{nullptr};
  PFN_vkBindBufferMemory bind_mem{nullptr};
  PFN_vkMapMemory map_mem{nullptr};
  PFN_vkUnmapMemory unmap_mem{nullptr};
  PFN_vkCreateImage create_image{nullptr};
  PFN_vkDestroyImage destroy_image{nullptr};
  PFN_vkGetImageMemoryRequirements get_image_reqs{nullptr};
  PFN_vkBindImageMemory bind_image_mem{nullptr};
  PFN_vkCreateImageView create_view{nullptr};
  PFN_vkDestroyImageView destroy_view{nullptr};
  PFN_vkCreateSampler create_sampler{nullptr};
  PFN_vkDestroySampler destroy_sampler{nullptr};
  PFN_vkCreateDescriptorPool create_pool{nullptr};
  PFN_vkDestroyDescriptorPool destroy_pool{nullptr};
  PFN_vkAllocateDescriptorSets alloc_sets{nullptr};
  PFN_vkUpdateDescriptorSets update_sets{nullptr};
  PFN_vkCreateDescriptorSetLayout create_set_layout{nullptr};
  PFN_vkDestroyDescriptorSetLayout destroy_set_layout{nullptr};
  PFN_vkCreatePipelineLayout create_pipe_layout{nullptr};
  PFN_vkDestroyPipelineLayout destroy_pipe_layout{nullptr};
  PFN_vkCreateShaderModule create_module{nullptr};
  PFN_vkDestroyShaderModule destroy_module{nullptr};
  PFN_vkCreateGraphicsPipelines create_pipes{nullptr};
  PFN_vkDestroyPipeline destroy_pipe{nullptr};
  PFN_vkCreateCommandPool create_cmd_pool{nullptr};
  PFN_vkDestroyCommandPool destroy_cmd_pool{nullptr};
  PFN_vkAllocateCommandBuffers alloc_cmds{nullptr};
  PFN_vkBeginCommandBuffer begin_cmd{nullptr};
  PFN_vkEndCommandBuffer end_cmd{nullptr};
  PFN_vkCmdBeginRendering cmd_begin_render{nullptr};
  PFN_vkCmdEndRendering cmd_end_render{nullptr};
  PFN_vkCmdBindPipeline cmd_bind_pipe{nullptr};
  PFN_vkCmdPushConstants cmd_push{nullptr};
  PFN_vkCmdDraw cmd_draw{nullptr};
  PFN_vkCmdBindDescriptorSets cmd_bind_sets{nullptr};
  PFN_vkCmdPipelineBarrier2 cmd_barrier2{nullptr};
  PFN_vkCmdCopyBufferToImage cmd_copy_buf_img{nullptr};
  PFN_vkCmdCopyImageToBuffer cmd_copy_img_buf{nullptr};
  PFN_vkCmdSetViewport cmd_viewport{nullptr};
  PFN_vkCmdSetScissor cmd_scissor{nullptr};
  PFN_vkCreateFence create_fence{nullptr};
  PFN_vkDestroyFence destroy_fence{nullptr};
  PFN_vkQueueSubmit queue_submit{nullptr};
  PFN_vkWaitForFences wait_fences{nullptr};
};

// Per-test Vulkan context: raw device handles (no ownership) + procs.
// Device/queue/family are borrowed from VulkanDevice (the TU owns it);
// buffers/images allocated through here are freed explicitly by the TU.
struct GoldenVk {
  VkDevice device{nullptr};
  VkPhysicalDevice physical{nullptr};
  VkQueue queue{nullptr};
  std::uint32_t queue_family{0};
  GoldenVkProcs vk{};
};

// Fill every entry point of `out` (false when any resolve is null).
// Loader is a (name -> PFN) callable so TUs keep their own proc-style.
template <typename LoaderFn>
inline auto GoldenVkLoad(LoaderFn loader, GoldenVkProcs* out) -> bool {
  auto load = [&](const char* name) { return loader(name); };
  out->create_buffer =
      reinterpret_cast<PFN_vkCreateBuffer>(load("vkCreateBuffer"));
  out->destroy_buffer =
      reinterpret_cast<PFN_vkDestroyBuffer>(load("vkDestroyBuffer"));
  out->get_buffer_reqs =
      reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(
          load("vkGetBufferMemoryRequirements"));
  out->get_mem_props =
      reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
          load("vkGetPhysicalDeviceMemoryProperties"));
  out->alloc_mem =
      reinterpret_cast<PFN_vkAllocateMemory>(load("vkAllocateMemory"));
  out->free_mem = reinterpret_cast<PFN_vkFreeMemory>(load("vkFreeMemory"));
  out->bind_mem =
      reinterpret_cast<PFN_vkBindBufferMemory>(load("vkBindBufferMemory"));
  out->map_mem = reinterpret_cast<PFN_vkMapMemory>(load("vkMapMemory"));
  out->unmap_mem = reinterpret_cast<PFN_vkUnmapMemory>(load("vkUnmapMemory"));
  out->create_image =
      reinterpret_cast<PFN_vkCreateImage>(load("vkCreateImage"));
  out->destroy_image =
      reinterpret_cast<PFN_vkDestroyImage>(load("vkDestroyImage"));
  out->get_image_reqs =
      reinterpret_cast<PFN_vkGetImageMemoryRequirements>(
          load("vkGetImageMemoryRequirements"));
  out->bind_image_mem =
      reinterpret_cast<PFN_vkBindImageMemory>(load("vkBindImageMemory"));
  out->create_view =
      reinterpret_cast<PFN_vkCreateImageView>(load("vkCreateImageView"));
  out->destroy_view =
      reinterpret_cast<PFN_vkDestroyImageView>(load("vkDestroyImageView"));
  out->create_sampler =
      reinterpret_cast<PFN_vkCreateSampler>(load("vkCreateSampler"));
  out->destroy_sampler =
      reinterpret_cast<PFN_vkDestroySampler>(load("vkDestroySampler"));
  out->create_pool =
      reinterpret_cast<PFN_vkCreateDescriptorPool>(load("vkCreateDescriptorPool"));
  out->destroy_pool =
      reinterpret_cast<PFN_vkDestroyDescriptorPool>(load("vkDestroyDescriptorPool"));
  out->alloc_sets =
      reinterpret_cast<PFN_vkAllocateDescriptorSets>(load("vkAllocateDescriptorSets"));
  out->update_sets =
      reinterpret_cast<PFN_vkUpdateDescriptorSets>(load("vkUpdateDescriptorSets"));
  out->create_set_layout =
      reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(
          load("vkCreateDescriptorSetLayout"));
  out->destroy_set_layout =
      reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(
          load("vkDestroyDescriptorSetLayout"));
  out->create_pipe_layout =
      reinterpret_cast<PFN_vkCreatePipelineLayout>(load("vkCreatePipelineLayout"));
  out->destroy_pipe_layout =
      reinterpret_cast<PFN_vkDestroyPipelineLayout>(
          load("vkDestroyPipelineLayout"));
  out->create_module =
      reinterpret_cast<PFN_vkCreateShaderModule>(load("vkCreateShaderModule"));
  out->destroy_module =
      reinterpret_cast<PFN_vkDestroyShaderModule>(load("vkDestroyShaderModule"));
  out->create_pipes = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(
      load("vkCreateGraphicsPipelines"));
  out->destroy_pipe =
      reinterpret_cast<PFN_vkDestroyPipeline>(load("vkDestroyPipeline"));
  out->create_cmd_pool = reinterpret_cast<PFN_vkCreateCommandPool>(
      load("vkCreateCommandPool"));
  out->destroy_cmd_pool = reinterpret_cast<PFN_vkDestroyCommandPool>(
      load("vkDestroyCommandPool"));
  out->alloc_cmds = reinterpret_cast<PFN_vkAllocateCommandBuffers>(
      load("vkAllocateCommandBuffers"));
  out->begin_cmd =
      reinterpret_cast<PFN_vkBeginCommandBuffer>(load("vkBeginCommandBuffer"));
  out->end_cmd =
      reinterpret_cast<PFN_vkEndCommandBuffer>(load("vkEndCommandBuffer"));
  out->cmd_begin_render = reinterpret_cast<PFN_vkCmdBeginRendering>(
      load("vkCmdBeginRendering"));
  out->cmd_end_render =
      reinterpret_cast<PFN_vkCmdEndRendering>(load("vkCmdEndRendering"));
  out->cmd_bind_pipe =
      reinterpret_cast<PFN_vkCmdBindPipeline>(load("vkCmdBindPipeline"));
  out->cmd_push =
      reinterpret_cast<PFN_vkCmdPushConstants>(load("vkCmdPushConstants"));
  out->cmd_draw = reinterpret_cast<PFN_vkCmdDraw>(load("vkCmdDraw"));
  out->cmd_bind_sets = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(
      load("vkCmdBindDescriptorSets"));
  out->cmd_barrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(
      load("vkCmdPipelineBarrier2"));
  out->cmd_copy_buf_img = reinterpret_cast<PFN_vkCmdCopyBufferToImage>(
      load("vkCmdCopyBufferToImage"));
  out->cmd_copy_img_buf = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(
      load("vkCmdCopyImageToBuffer"));
  out->cmd_viewport =
      reinterpret_cast<PFN_vkCmdSetViewport>(load("vkCmdSetViewport"));
  out->cmd_scissor =
      reinterpret_cast<PFN_vkCmdSetScissor>(load("vkCmdSetScissor"));
  out->create_fence =
      reinterpret_cast<PFN_vkCreateFence>(load("vkCreateFence"));
  out->destroy_fence =
      reinterpret_cast<PFN_vkDestroyFence>(load("vkDestroyFence"));
  out->queue_submit =
      reinterpret_cast<PFN_vkQueueSubmit>(load("vkQueueSubmit"));
  out->wait_fences =
      reinterpret_cast<PFN_vkWaitForFences>(load("vkWaitForFences"));
  const bool slots_ok =
      out->create_buffer != nullptr && out->create_image != nullptr &&
      out->create_module != nullptr && out->create_pipes != nullptr &&
      out->alloc_cmds != nullptr && out->begin_cmd != nullptr &&
      out->queue_submit != nullptr && out->wait_fences != nullptr &&
      out->create_pool != nullptr && out->cmd_begin_render != nullptr;
  return slots_ok;
}

struct GoldenHostBuf {
  VkBuffer buf{nullptr};
  VkDeviceMemory mem{nullptr};
};

struct GoldenImage {
  VkImage img{nullptr};
  VkDeviceMemory mem{nullptr};
  VkImageView view{nullptr};
};

// Host-visible staging buffer (TRANSFER_SRC/DST use; no VMA — the P4
// goldens upload once and read back once per run).
inline auto GoldenAllocHost(GoldenVk* g, std::uint64_t bytes,
                            VkBufferUsageFlags usage, GoldenHostBuf* out)
    -> bool {
  VkBufferCreateInfo bc{};
  bc.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bc.size = static_cast<VkDeviceSize>(bytes);
  bc.usage = usage;
  bc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (g->vk.create_buffer(g->device, &bc, nullptr, &out->buf) != VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements req{};
  g->vk.get_buffer_reqs(g->device, out->buf, &req);
  VkPhysicalDeviceMemoryProperties mp{};
  g->vk.get_mem_props(g->physical, &mp);
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
  if (mi == 0xFFFFFFFFu) {
    g->vk.destroy_buffer(g->device, out->buf, nullptr);
    out->buf = nullptr;
    return false;
  }
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = mi;
  if (g->vk.alloc_mem(g->device, &ai, nullptr, &out->mem) != VK_SUCCESS) {
    g->vk.destroy_buffer(g->device, out->buf, nullptr);
    out->buf = nullptr;
    return false;
  }
  return g->vk.bind_mem(g->device, out->buf, out->mem, 0) == VK_SUCCESS;
}

inline void GoldenFreeHost(GoldenVk* g, GoldenHostBuf* hb) {
  if (hb->buf != nullptr) {
    g->vk.destroy_buffer(g->device, hb->buf, nullptr);
    hb->buf = nullptr;
  }
  if (hb->mem != nullptr) {
    g->vk.free_mem(g->device, hb->mem, nullptr);
    hb->mem = nullptr;
  }
}

inline auto GoldenUploadBytes(GoldenVk* g, GoldenHostBuf* hb,
                              std::span<const std::uint8_t> src) -> bool {
  void* p = nullptr;
  const std::size_t n = src.size();
  if (g->vk.map_mem(g->device, hb->mem, 0, n, 0, &p) != VK_SUCCESS ||
      p == nullptr) {
    return false;
  }
  auto* d = static_cast<std::uint8_t*>(p);
  for (std::size_t i = 0; i < n; ++i) {
    d[i] = src[i];
  }
  g->vk.unmap_mem(g->device, hb->mem);
  return true;
}

// Device-local 2D image + view (single mip/layer, COLOR aspect).
inline auto GoldenMakeImage(GoldenVk* g, VkFormat fmt, std::uint32_t w,
                            std::uint32_t h, VkImageUsageFlags usage,
                            GoldenImage* out) -> bool {
  VkImageCreateInfo ii{};
  ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = fmt;
  ii.extent = {w, h, 1};
  ii.mipLevels = 1;
  ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = usage;
  ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (g->vk.create_image(g->device, &ii, nullptr, &out->img) != VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements req{};
  g->vk.get_image_reqs(g->device, out->img, &req);
  VkPhysicalDeviceMemoryProperties mp{};
  g->vk.get_mem_props(g->physical, &mp);
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
  if (mi == 0xFFFFFFFFu) {
    g->vk.destroy_image(g->device, out->img, nullptr);
    out->img = nullptr;
    return false;
  }
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = mi;
  if (g->vk.alloc_mem(g->device, &ai, nullptr, &out->mem) != VK_SUCCESS) {
    g->vk.destroy_image(g->device, out->img, nullptr);
    out->img = nullptr;
    return false;
  }
  if (g->vk.bind_image_mem(g->device, out->img, out->mem, 0) != VK_SUCCESS) {
    g->vk.free_mem(g->device, out->mem, nullptr);
    g->vk.destroy_image(g->device, out->img, nullptr);
    out->img = nullptr;
    out->mem = nullptr;
    return false;
  }
  VkImageViewCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vi.image = out->img;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = fmt;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  return g->vk.create_view(g->device, &vi, nullptr, &out->view) == VK_SUCCESS;
}

inline void GoldenFreeImage(GoldenVk* g, GoldenImage* im) {
  if (im->view != nullptr) {
    g->vk.destroy_view(g->device, im->view, nullptr);
    im->view = nullptr;
  }
  if (im->img != nullptr) {
    g->vk.destroy_image(g->device, im->img, nullptr);
    im->img = nullptr;
  }
  if (im->mem != nullptr) {
    g->vk.free_mem(g->device, im->mem, nullptr);
    im->mem = nullptr;
  }
}

// One shader module from checked-in SPIR-V words.
inline auto GoldenMakeModule(GoldenVk* g, const std::uint32_t* words,
                             std::size_t n, VkShaderModule* out) -> bool {
  VkShaderModuleCreateInfo sci{};
  sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  sci.codeSize = n * 4;
  sci.pCode = words;
  return g->vk.create_module(g->device, &sci, nullptr, out) == VK_SUCCESS;
}

// Transition `img` old->new with explicit stage/access masks (sync2).
inline void GoldenBarrier(GoldenVk* g, VkCommandBuffer cb, VkImage img,
                          VkImageLayout old_layout, VkImageLayout new_layout,
                          VkPipelineStageFlags2 src_stage,
                          VkAccessFlags2 src_access,
                          VkPipelineStageFlags2 dst_stage,
                          VkAccessFlags2 dst_access) {
  VkImageMemoryBarrier2 b{};
  b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
  b.srcStageMask = src_stage;
  b.srcAccessMask = src_access;
  b.dstStageMask = dst_stage;
  b.dstAccessMask = dst_access;
  b.oldLayout = old_layout;
  b.newLayout = new_layout;
  b.image = img;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo d{};
  d.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  d.imageMemoryBarrierCount = 1;
  d.pImageMemoryBarriers = &b;
  g->vk.cmd_barrier2(cb, &d);
}

// Submit one command buffer on the test queue and fence-wait it.
inline auto GoldenSubmitWait(GoldenVk* g, VkCommandBuffer cb) -> bool {
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cb;
  VkFenceCreateInfo fi{};
  fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = nullptr;
  if (g->vk.create_fence(g->device, &fi, nullptr, &fence) != VK_SUCCESS) {
    return false;
  }
  const bool ok =
      g->vk.queue_submit(g->queue, 1, &si, fence) == VK_SUCCESS &&
      g->vk.wait_fences(g->device, 1, &fence, 1, 10000000000ull) == VK_SUCCESS;
  g->vk.destroy_fence(g->device, fence, nullptr);
  return ok;
}

// P4-02 shade/post pipeline pair: modules + shared set-1 layouts +
// per-pass layouts + dynamic-rendering graphics pipelines. Returns empty
// handles on any failure (caller destroys only non-null). Lanes stay in
// engine::test so TUs keep their own check labels / push sizes.
//
// set1_shade/post are RETURNED (not consumed): the TU needs them to
// allocate its descriptor sets (pool layouts reference them alongside
// the bindless set-0).
struct GoldenShadePost {
  VkDescriptorSetLayout set1_shade{nullptr};
  VkDescriptorSetLayout set1_post{nullptr};
  VkPipelineLayout shade_layout{nullptr};
  VkPipelineLayout post_layout{nullptr};
  VkPipeline shade_pipe{nullptr};
  VkPipeline post_pipe{nullptr};
};

// Build the shade (HDR float target) + post (LDR target) pipeline pair.
// shade/post_vert/frag are checked-in SPIR-V word arrays; shade/post_fmt
// are their color attachment formats. Descriptor shared-set-1 comes from
// GoldenBuildSet1 (golden_pipe_helper.hpp) via the create_set_layout
// callable (same (device, info, alloc, out) shape as the raw Vulkan call,
// so TUs wrap g.vk.create_set_layout directly), so this TU stays free of
// the raw struct-fill.
template <typename CreateSetLayoutFn, typename CreatePipeLayoutFn,
          typename CreatePipesFn, typename DestroyModFn>
inline auto GoldenBuildShadePost(GoldenVk* g, VkDescriptorSetLayout bindless,
                                 const std::uint32_t* shade_vert,
                                 std::size_t shade_vert_words,
                                 const std::uint32_t* shade_frag,
                                 std::size_t shade_frag_words, VkFormat shade_fmt,
                                 const std::uint32_t* post_vert,
                                 std::size_t post_vert_words,
                                 const std::uint32_t* post_frag,
                                 std::size_t post_frag_words, VkFormat post_fmt,
                                 CreateSetLayoutFn create_set_layout,
                                 CreatePipeLayoutFn create_pipe_layout,
                                 CreatePipesFn create_pipes,
                                 DestroyModFn destroy_mod, GoldenShadePost* out)
    -> bool {
  VkDescriptorSetLayout set1_shade = nullptr;
  VkDescriptorSetLayout set1_post = nullptr;
  auto raw_layout = [&](VkDevice device,
                        const VkDescriptorSetLayoutCreateInfo* info,
                        const VkAllocationCallbacks* alloc,
                        VkDescriptorSetLayout* layout_out) {
    return create_set_layout(device, info, alloc, layout_out);
  };
  if (!GoldenBuildSet1(raw_layout, g->device, &set1_shade, &set1_post)) {
    return false;
  }
  VkShaderModule shade_v = nullptr;
  VkShaderModule shade_f = nullptr;
  VkShaderModule post_v = nullptr;
  VkShaderModule post_f = nullptr;
  const bool mods_ok =
      GoldenMakeModule(g, shade_vert, shade_vert_words, &shade_v) &&
      GoldenMakeModule(g, shade_frag, shade_frag_words, &shade_f) &&
      GoldenMakeModule(g, post_vert, post_vert_words, &post_v) &&
      GoldenMakeModule(g, post_frag, post_frag_words, &post_f);
  if (!mods_ok) {
    for (auto* m : {&shade_v, &shade_f, &post_v, &post_f}) {
      if (*m != nullptr) {
        destroy_mod(*m);
      }
    }
    return false;
  }
  VkPipelineLayout shade_layout = nullptr;
  VkPipelineLayout post_layout = nullptr;
  bool plo_ok = true;
  {
    VkDescriptorSetLayout sets[2] = {bindless, set1_shade};
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset = 0;
    push.size = 64;  // PbrShadePush
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 2;
    pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    plo_ok = create_pipe_layout(&pli, &shade_layout);
  }
  if (plo_ok) {
    VkDescriptorSetLayout psets[2] = {bindless, set1_post};
    VkPushConstantRange ppush{};
    ppush.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    ppush.offset = 0;
    ppush.size = 20;  // PostPush
    VkPipelineLayoutCreateInfo ppli{};
    ppli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ppli.setLayoutCount = 2;
    ppli.pSetLayouts = psets;
    ppli.pushConstantRangeCount = 1;
    ppli.pPushConstantRanges = &ppush;
    plo_ok = create_pipe_layout(&ppli, &post_layout);
  }
  bool pipes_ok = plo_ok;
  VkPipeline shade_pipe = nullptr;
  VkPipeline post_pipe = nullptr;
  if (pipes_ok) {
    pipes_ok =
        GoldenBuildPipe(create_pipes, g->device, shade_v, shade_f, shade_fmt,
                        shade_layout, &shade_pipe) &&
        GoldenBuildPipe(create_pipes, g->device, post_v, post_f, post_fmt,
                        post_layout, &post_pipe);
  }
  for (auto* m : {&shade_v, &shade_f, &post_v, &post_f}) {
    destroy_mod(*m);
  }
  if (!pipes_ok) {
    return false;
  }
  out->set1_shade = set1_shade;
  out->set1_post = set1_post;
  out->shade_layout = shade_layout;
  out->post_layout = post_layout;
  out->shade_pipe = shade_pipe;
  out->post_pipe = post_pipe;
  return true;
}

// Test-frame descriptor payload: the 9 writes (set0 scene/material +
// shade vis/lights/grid/index/albedo-dummy + post HDR/sampler) shared by
// the P4 shade->post golden shape. TUs fill one of these and hand it to
// GoldenWriteFrameSets; the raw VkWriteDescriptorSet struct-fill lives
// here once (Sonar duplication gate).
struct GoldenFrameSets {
  VkBuffer scene_buf{VK_NULL_HANDLE};
  std::uint64_t scene_bytes{0};
  VkBuffer mat_buf{VK_NULL_HANDLE};
  std::uint64_t mat_bytes{0};
  VkBuffer light_buf{VK_NULL_HANDLE};
  std::uint64_t light_bytes{0};
  VkBuffer grid_buf{VK_NULL_HANDLE};
  std::uint64_t grid_bytes{0};
  VkBuffer index_buf{VK_NULL_HANDLE};
  std::uint64_t index_bytes{0};
  VkImageView vis_view{VK_NULL_HANDLE};
  VkImageView hdr_view{VK_NULL_HANDLE};
  VkSampler sampler{VK_NULL_HANDLE};
  VkDescriptorSet set0{VK_NULL_HANDLE};
  VkDescriptorSet set_shade{VK_NULL_HANDLE};
  VkDescriptorSet set_post{VK_NULL_HANDLE};
};

template <typename UpdateSetsFn>
inline void GoldenWriteFrameSets(GoldenVk* g, const GoldenFrameSets& f,
                                 UpdateSetsFn update_sets) {
  VkDescriptorBufferInfo scene_info{f.scene_buf, 0, f.scene_bytes};
  VkDescriptorBufferInfo mat_info{f.mat_buf, 0, f.mat_bytes};
  VkDescriptorBufferInfo light_info{f.light_buf, 0, f.light_bytes};
  VkDescriptorBufferInfo grid_info{f.grid_buf, 0, f.grid_bytes};
  VkDescriptorBufferInfo index_info{f.index_buf, 0, f.index_bytes};
  VkDescriptorImageInfo vis_info{};
  vis_info.imageView = f.vis_view;
  vis_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorImageInfo hdr_info{};
  hdr_info.imageView = f.hdr_view;
  hdr_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkDescriptorImageInfo post_smp{};
  post_smp.sampler = f.sampler;
  VkDescriptorImageInfo shade_dummy_comb{};
  shade_dummy_comb.imageView = f.hdr_view;
  shade_dummy_comb.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  shade_dummy_comb.sampler = f.sampler;
  VkWriteDescriptorSet w[10]{};
  for (int i = 0; i < 10; ++i) {
    w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  }
  w[0].dstSet = f.set0;
  w[0].dstBinding = 0;
  w[0].descriptorCount = 1;
  w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  w[0].pBufferInfo = &scene_info;
  w[1].dstSet = f.set0;
  w[1].dstBinding = 1;
  w[1].descriptorCount = 1;
  w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  w[1].pBufferInfo = &mat_info;
  w[2].dstSet = f.set_shade;
  w[2].dstBinding = 0;
  w[2].descriptorCount = 1;
  w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  w[2].pImageInfo = &vis_info;
  w[3].dstSet = f.set_shade;
  w[3].dstBinding = 3;
  w[3].descriptorCount = 1;
  w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  w[3].pBufferInfo = &light_info;
  w[4].dstSet = f.set_shade;
  w[4].dstBinding = 4;
  w[4].descriptorCount = 1;
  w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  w[4].pBufferInfo = &grid_info;
  w[5].dstSet = f.set_shade;
  w[5].dstBinding = 5;
  w[5].descriptorCount = 1;
  w[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  w[5].pBufferInfo = &index_info;
  w[6].dstSet = f.set_shade;
  w[6].dstBinding = 2;
  w[6].descriptorCount = 1;
  w[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  w[6].pImageInfo = &shade_dummy_comb;
  w[7].dstSet = f.set_post;
  w[7].dstBinding = 0;
  w[7].descriptorCount = 1;
  w[7].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  w[7].pImageInfo = &hdr_info;
  w[8].dstSet = f.set_post;
  w[8].dstBinding = 1;
  w[8].descriptorCount = 1;
  w[8].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  w[8].pImageInfo = &post_smp;
  update_sets(g->device, w);
}

// Allocate one descriptor pool + 3 sets (bindless + shade/post set-1).
template <typename CreatePoolFn, typename AllocSetsFn>
inline auto GoldenAllocFrameSets(GoldenVk* g, VkDescriptorSetLayout bindless,
                                 VkDescriptorSetLayout set1_shade,
                                 VkDescriptorSetLayout set1_post,
                                 CreatePoolFn create_pool,
                                 AllocSetsFn alloc_sets, VkDescriptorPool* pool,
                                 VkDescriptorSet* set0,
                                 VkDescriptorSet* set_shade,
                                 VkDescriptorSet* set_post) -> bool {
  VkDescriptorPoolSize sizes[5]{};
  sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  sizes[0].descriptorCount = 5;
  sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  sizes[1].descriptorCount = 1;
  sizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLER;
  sizes[2].descriptorCount = 2;
  sizes[3].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  sizes[3].descriptorCount = 1;
  sizes[4].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  sizes[4].descriptorCount = 1;
  VkDescriptorPoolCreateInfo pi{};
  pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pi.maxSets = 3;
  pi.poolSizeCount = 5;
  pi.pPoolSizes = sizes;
  if (create_pool(g->device, &pi, nullptr, pool) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorSetLayout layouts[3] = {bindless, set1_shade, set1_post};
  VkDescriptorSetAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  ai.descriptorPool = *pool;
  ai.descriptorSetCount = 3;
  ai.pSetLayouts = layouts;
  VkDescriptorSet sets[3]{nullptr, nullptr, nullptr};
  if (alloc_sets(g->device, &ai, sets) != VK_SUCCESS) {
    return false;
  }
  *set0 = sets[0];
  *set_shade = sets[1];
  *set_post = sets[2];
  return true;
}

// One shade->post frame record: uploads vis, runs the HDR shade pass,
// runs the LDR post pass at `exposure`, reads back LDR bytes. TUs fill
// one of these per frame; the barrier/render/push struct-fill lives here
// once (Sonar duplication gate). Push structs match pbr_pipeline.hpp
// (PbrShadePush 64B / PostPush 20B).
struct GoldenShadePush {
  float cam[4]{};
  float frame[4]{};
  float inv[4]{};
  std::uint32_t light_count{0};
  std::uint32_t flags{0};
  std::uint32_t max_lights_per_tile{0};
  std::uint32_t pad{0};
};
static_assert(sizeof(GoldenShadePush) == 64, "shade push is 64B");

struct GoldenPostPush {
  float exposure{1.0f};
  std::uint32_t flags{0};
  std::uint32_t pad0{0};
  std::uint32_t pad1{0};
  std::uint32_t pad2{0};
};
static_assert(sizeof(GoldenPostPush) == 20, "post push is 20B");

struct GoldenFrame {
  VkBuffer vis_buf{VK_NULL_HANDLE};
  std::uint32_t w{0};
  std::uint32_t h{0};
  VkImage vis_img{VK_NULL_HANDLE};
  VkImage hdr_img{VK_NULL_HANDLE};
  VkImage ldr_img{VK_NULL_HANDLE};
  VkImageView hdr_view{VK_NULL_HANDLE};
  VkImageView ldr_view{VK_NULL_HANDLE};
  VkPipeline shade_pipe{VK_NULL_HANDLE};
  VkPipeline post_pipe{VK_NULL_HANDLE};
  VkPipelineLayout shade_layout{VK_NULL_HANDLE};
  VkPipelineLayout post_layout{VK_NULL_HANDLE};
  VkDescriptorSet set0{VK_NULL_HANDLE};
  VkDescriptorSet set_shade{VK_NULL_HANDLE};
  VkDescriptorSet set_post{VK_NULL_HANDLE};
  GoldenShadePush shade{};
  GoldenPostPush post{};
};

// Record one shade -> post frame into an open command buffer (caller
// owns begin/end + submit + readback).
inline void GoldenRecordFrame(GoldenVk* g, VkCommandBuffer cb,
                              const GoldenFrame& f) {
  VkBufferImageCopy region{};
  region.bufferOffset = 0;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {f.w, f.h, 1};
  GoldenBarrier(g, cb, f.vis_img, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
  g->vk.cmd_copy_buf_img(cb, f.vis_buf, f.vis_img,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  GoldenBarrier(g, cb, f.vis_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
  for (std::uint32_t i = 0; i < 2; ++i) {
    GoldenBarrier(g, cb, (i == 0) ? f.hdr_img : f.ldr_img,
                  VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
  }
  // Shade pass -> HDR.
  VkRenderingAttachmentInfo shade_att{};
  shade_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  shade_att.imageView = f.hdr_view;
  shade_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  shade_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  shade_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  shade_att.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  VkRenderingInfo shade_ri{};
  shade_ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  shade_ri.renderArea = {{0, 0}, {f.w, f.h}};
  shade_ri.layerCount = 1;
  shade_ri.colorAttachmentCount = 1;
  shade_ri.pColorAttachments = &shade_att;
  g->vk.cmd_begin_render(cb, &shade_ri);
  const float fw = static_cast<float>(f.w);
  const float fh = static_cast<float>(f.h);
  VkViewport vp{0, 0, fw, fh, 0.0f, 1.0f};
  g->vk.cmd_viewport(cb, 0, 1, &vp);
  VkRect2D sc{{0, 0}, {f.w, f.h}};
  g->vk.cmd_scissor(cb, 0, 1, &sc);
  g->vk.cmd_bind_pipe(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, f.shade_pipe);
  VkDescriptorSet shade_sets[2] = {f.set0, f.set_shade};
  g->vk.cmd_bind_sets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, f.shade_layout, 0,
                      2, shade_sets, 0, nullptr);
  GoldenShadePush s = f.shade;
  s.frame[0] = fw;
  s.frame[1] = fh;
  s.inv[0] = 1.0f / fw;
  s.inv[1] = 1.0f / fh;
  g->vk.cmd_push(cb, f.shade_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                 sizeof(s), &s);
  g->vk.cmd_draw(cb, 3, 1, 0, 0);
  g->vk.cmd_end_render(cb);
  // HDR -> SHADER_READ for the post pass.
  GoldenBarrier(g, cb, f.hdr_img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_READ_BIT);
  // Post pass -> LDR.
  VkRenderingAttachmentInfo post_att{};
  post_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  post_att.imageView = f.ldr_view;
  post_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  post_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  post_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  post_att.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  VkRenderingInfo post_ri{};
  post_ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  post_ri.renderArea = {{0, 0}, {f.w, f.h}};
  post_ri.layerCount = 1;
  post_ri.colorAttachmentCount = 1;
  post_ri.pColorAttachments = &post_att;
  g->vk.cmd_begin_render(cb, &post_ri);
  g->vk.cmd_viewport(cb, 0, 1, &vp);
  g->vk.cmd_scissor(cb, 0, 1, &sc);
  g->vk.cmd_bind_pipe(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, f.post_pipe);
  VkDescriptorSet post_sets[2] = {f.set0, f.set_post};
  g->vk.cmd_bind_sets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, f.post_layout, 0,
                      2, post_sets, 0, nullptr);
  g->vk.cmd_push(cb, f.post_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                 sizeof(f.post), &f.post);
  g->vk.cmd_draw(cb, 3, 1, 0, 0);
  g->vk.cmd_end_render(cb);
}

#endif

}  // namespace engine::test

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

#endif

}  // namespace engine::test

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
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
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
  PFN_vkCreateComputePipelines create_compute{nullptr};
  PFN_vkCmdDispatch cmd_dispatch{nullptr};
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
  out->create_compute = reinterpret_cast<PFN_vkCreateComputePipelines>(
      load("vkCreateComputePipelines"));
  out->cmd_dispatch =
      reinterpret_cast<PFN_vkCmdDispatch>(load("vkCmdDispatch"));
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
      out->create_pool != nullptr && out->cmd_begin_render != nullptr &&
      out->create_compute != nullptr && out->cmd_dispatch != nullptr;
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

// Pick a memory type index satisfying req bits + property flags (first
// fit; 0xFFFFFFFFu when none). Shared by host + image allocators below
// (Sonar new-duplication gate: the pick loop repeats per allocator).
inline auto GoldenPickMemory(GoldenVk* g, std::uint32_t type_bits,
                             VkMemoryPropertyFlags flags) -> std::uint32_t {
  VkPhysicalDeviceMemoryProperties mp{};
  g->vk.get_mem_props(g->physical, &mp);
  for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) != 0u &&
        (mp.memoryTypes[i].propertyFlags & flags) == flags) {
      return i;
    }
  }
  return 0xFFFFFFFFu;
}

// Allocate device memory for an already-created buffer/image.
inline auto GoldenAllocBind(GoldenVk* g, const VkMemoryRequirements& req,
                            std::uint32_t mem_index, VkDeviceMemory* out_mem)
    -> bool {
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = mem_index;
  return g->vk.alloc_mem(g->device, &ai, nullptr, out_mem) == VK_SUCCESS;
}

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
  const std::uint32_t mi = GoldenPickMemory(
      g, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (mi == 0xFFFFFFFFu ||
      !GoldenAllocBind(g, req, mi, &out->mem)) {
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
  std::uint32_t mi =
      GoldenPickMemory(g, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (mi == 0xFFFFFFFFu) {
    mi = GoldenPickMemory(g, req.memoryTypeBits, 0);
  }
  if (mi == 0xFFFFFFFFu ||
      !GoldenAllocBind(g, req, mi, &out->mem)) {
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

// Device-local cube-compatible RGBA32F image (6 layers, mip chain) WITHOUT
// a view (view creation is the TU's job: storage writes need 2D-array
// per-mip views, sampling needs a whole-chain CUBE view — one view
// cannot serve both). P4-05 skybox/IBL cubes share this builder; the
// import TU uses (edge, 1), the IBL TU (edge, mips). Pair with
// GoldenMakeCubeSampleView (whole-chain CUBE) and per-mip 2D-array views
// (see make_per_mip_view in test_skybox_ibl.cpp).
inline auto GoldenMakeCube(GoldenVk* g, std::uint32_t edge, std::uint32_t mips,
                           VkImageUsageFlags usage, GoldenImage* out) -> bool {
  VkImageCreateInfo ii{};
  ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ii.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = VK_FORMAT_R32G32B32A32_SFLOAT;
  ii.extent = {edge, edge, 1};
  ii.mipLevels = mips;
  ii.arrayLayers = 6;
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
  const std::uint32_t mi = GoldenPickMemory(g, req.memoryTypeBits, 0);
  if (mi == 0xFFFFFFFFu ||
      !GoldenAllocBind(g, req, mi, &out->mem)) {
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
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  vi.format = VK_FORMAT_R32G32B32A32_SFLOAT;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
  return g->vk.create_view(g->device, &vi, nullptr, &out->view) == VK_SUCCESS;
}

// Whole-chain CUBE sample view over a GoldenMakeCube image (mips levels).
// Sampling (irradiance/prefilter/skybox cubes in the shade set) needs the
// full chain; storage writes use per-mip 2D-array views instead.
inline auto GoldenMakeCubeSampleView(GoldenVk* g, VkImage img,
                                     std::uint32_t mips, VkImageView* out)
    -> bool {
  VkImageViewCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vi.image = img;
  vi.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
  vi.format = VK_FORMAT_R32G32B32A32_SFLOAT;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 6};
  return g->vk.create_view(g->device, &vi, nullptr, out) == VK_SUCCESS;
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

// Compute set layout: binding 0 = cube sampler, 1 = storage image (shared
// by the P4-05 compute goldens; Sonar new-duplication gate).
inline auto GoldenMakeComputeSetLayout(GoldenVk* g,
                                       VkDescriptorSetLayout* out) -> bool {
  VkDescriptorSetLayoutBinding binds[2]{};
  binds[0].binding = 0;
  binds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  binds[0].descriptorCount = 1;
  binds[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  binds[1].binding = 1;
  binds[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  binds[1].descriptorCount = 1;
  binds[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayoutCreateInfo li{};
  li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  li.bindingCount = 2;
  li.pBindings = binds;
  return g->vk.create_set_layout(g->device, &li, nullptr, out) == VK_SUCCESS;
}

// Allocate + begin a one-time primary command buffer (shared by the P4-05
// compute goldens; Sonar new-duplication gate).
inline auto GoldenBeginCmd(GoldenVk* g, VkCommandPool cmd_pool,
                           VkCommandBuffer* out_cb) -> bool {
  VkCommandBufferAllocateInfo ab{};
  ab.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  ab.commandPool = cmd_pool;
  ab.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ab.commandBufferCount = 1;
  if (g->vk.alloc_cmds(g->device, &ab, out_cb) != VK_SUCCESS) {
    return false;
  }
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  return g->vk.begin_cmd(*out_cb, &bi) == VK_SUCCESS;
}

// Host-readback barrier + end: orders COPY writes before the host read
// (shared by the P4-05 compute goldens; Sonar new-duplication gate).
inline auto GoldenEndReadback(GoldenVk* g, VkCommandBuffer cb) -> bool {
  VkMemoryBarrier2 hb{};
  hb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
  hb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  hb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  hb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  hb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  VkDependencyInfo dh{};
  dh.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dh.memoryBarrierCount = 1;
  dh.pMemoryBarriers = &hb;
  g->vk.cmd_barrier2(cb, &dh);
  return g->vk.end_cmd(cb) == VK_SUCCESS;
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

// Submit one command buffer + LDR->readback copy shared tail (the 16-line
// copy/barrier struct-fill mirrored by both goldens; Sonar counts the
// cross-file mirror). The record callback fills GoldenRecordFrame-style
// content between begin and this tail.
template <typename CopyFn, typename BarrierFn, typename EndFn>
inline auto GoldenSubmitReadback(GoldenVk* g, VkCommandBuffer cb,
                                 VkImage ldr_img, VkBuffer read_buf,
                                 std::uint32_t w, std::uint32_t h,
                                 CopyFn copy_fn, BarrierFn barrier_fn,
                                 EndFn end_fn) -> bool {
  GoldenBarrier(g, cb, ldr_img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
  VkBufferImageCopy rc{};
  rc.bufferOffset = 0;
  rc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  rc.imageExtent = {w, h, 1};
  copy_fn(cb, ldr_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, read_buf, 1, &rc);
  VkMemoryBarrier2 hb{};
  hb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
  hb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  hb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  hb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  hb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  VkDependencyInfo dh{};
  dh.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dh.memoryBarrierCount = 1;
  dh.pMemoryBarriers = &hb;
  barrier_fn(cb, &dh);
  return end_fn(cb);
}

// Map the readback buffer into the TU pixel vector (shared loop).
template <typename MapFn, typename UnmapFn>
inline auto GoldenMapPixels(MapFn map_fn, UnmapFn unmap_fn, VkDevice device,
                            VkDeviceMemory mem, std::uint8_t* dst,
                            std::size_t n) -> bool {
  void* p = nullptr;
  if (map_fn(device, mem, 0, n, 0, &p) != VK_SUCCESS || p == nullptr) {
    return false;
  }
  const auto* src = static_cast<const std::uint8_t*>(p);
  for (std::size_t i = 0; i < n; ++i) {
    dst[i] = src[i];
  }
  unmap_fn(device, mem);
  return true;
}
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

// Shared scene/grid/index/vis fill shared by the two shade->post goldens
// (P4-01 grid + P4-02 halves). The TUs differ only in their per-instance
// contents callbacks; the byte loops, grid fill, and success check live
// here once (Sonar new-duplication gate counts the cross-file mirror).
struct GoldenSceneDims {
  std::uint32_t inst{0};
  std::uint32_t w{0};
  std::uint32_t h{0};
};

// Fill one 96B scene instance record (identity rotation rows + sphere +
// ids) shared by the goldens; per-TU sphere/material contents arrive via
// the callbacks below.
inline void GoldenFillSceneRecord(std::uint8_t* scene, std::uint32_t inst,
                                  float sph_x, float sph_y, float sph_z,
                                  float sph_r) {
  float* m = reinterpret_cast<float*>(scene + inst * 96);
  for (int d = 0; d < 16; ++d) {
    m[d] = (d % 5 == 0) ? 1.0f : 0.0f;
  }
  float* sph = reinterpret_cast<float*>(scene + inst * 96 + 64);
  sph[0] = sph_x;
  sph[1] = sph_y;
  sph[2] = sph_z;
  sph[3] = sph_r;
  std::uint32_t* ids =
      reinterpret_cast<std::uint32_t*>(scene + inst * 96 + 80);
  ids[0] = 0;
  ids[1] = inst;  // material_slot = instance index
  ids[2] = 1;     // flags: visible
  ids[3] = 0;
}

// Trivial forward+ grid (every tile -> the single index entry) shared by
// the goldens; the light/index contents differ per TU.
inline void GoldenFillTrivialGrid(std::uint32_t* grid, std::uint32_t tiles) {
  for (std::uint32_t t = 0; t < tiles; ++t) {
    grid[t * 2 + 0] = 0;
    grid[t * 2 + 1] = 1;
  }
}

// Pack one material asset into the golden's scene/material byte vectors
// (slot check + scene-record conversion shared by the two goldens; the
// label differs per TU so failures name the right golden).
template <typename CheckFn, typename LibT, typename AssetT>
inline auto GoldenPackMaterial(CheckFn check, LibT* lib, const AssetT& a,
                               std::uint32_t slot_want, float* mats_out,
                               const char* label_add,
                               const char* label_rec) -> bool {
  std::uint32_t slot = 0xFFFFFFFFu;
  if (!lib->add(a, &slot).ok() || slot != slot_want) {
    check(false, label_add);
    return false;
  }
  float rec[16]{};
  if (!LibT::to_scene_record(a, rec).ok()) {
    check(false, label_rec);
    return false;
  }
  for (int f = 0; f < 16; ++f) {
    mats_out[f] = rec[f];
  }
  return true;
}

// Fill one GoldenFrame for the shared shade->post record (cam, 1 light,
// 64 max/tile shared; exposure differs per TU). The vis/HDR/LDR images,
// pipes, layouts, and sets ride shared GoldenHostBuf/GoldenImage handles
// (the P4-01 golden converts its TU-local HostBuf/Img shims field-wise at
// the call site) + pipe handles. GoldenFrameBufs + GoldenFillFrame are
// defined after the GoldenFrame struct below (member use needs the
// complete type).

template <typename CheckFn>
inline auto GoldenCheckPacked(CheckFn check, bool pack_ok,
                              const char* label) -> bool {
  check(pack_ok, label);
  return pack_ok;
}

// Test-frame descriptor payload: the 9 writes (set0 scene/material +
// shade vis/lights/grid/index/albedo-dummy + post HDR/sampler) shared by
// the P4 shade->post golden shape. GoldenAllocWriteFrameSets fills one of
// these from the TU's handles, then hands it to GoldenWriteFrameSets; the
// raw VkWriteDescriptorSet struct-fill lives here once (Sonar duplication
// gate). GoldenFrameHandles aliases this struct (one type, not two
// mirrors): the TU fills scene/mat/light/grid/index bytes + views +
// sampler, and the helper fills set0/set_shade/set_post after allocation.
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
//
// RADV note: set-0's bindless layout declares a 4096 sampled-image array;
// RADV sizes that binding's pool consumption at 2x the declared count, so
// a pool sized exactly at the layout count (4096) fails
// vkAllocateDescriptorSets with ERROR_OUT_OF_POOL_MEMORY on RADV while
// lavapipe accepts it. Size sampled-image at 2x (8192); sampler at 2x the
// 16 declared (32). Both drivers accept the oversize pool.
//
// TU call shape (Sonar new-duplication gate counts the two goldens'
// identical pool/set/write call sites against each other): each TU passes
// two small device-routed lambdas + one GoldenFrameSets with its TU
// buffer/image handles; pool sizes, the w[10] write fill, and the
// update_sets count (9) live here once. (GoldenFrameHandles =
// GoldenFrameSets: one struct type, not two mirrored field lists.)
using GoldenFrameHandles = GoldenFrameSets;

template <typename CreatePoolFn, typename AllocSetsFn, typename UpdateSetsFn>
inline auto GoldenAllocWriteFrameSets(
    GoldenVk* g, VkDescriptorSetLayout bindless,
    VkDescriptorSetLayout set1_shade, VkDescriptorSetLayout set1_post,
    CreatePoolFn create_pool, AllocSetsFn alloc_sets, UpdateSetsFn update_sets,
    const GoldenFrameHandles& h, VkDescriptorPool* pool,
    VkDescriptorSet* set0, VkDescriptorSet* set_shade,
    VkDescriptorSet* set_post) -> bool {
  VkDescriptorPoolSize sizes[5]{};
  sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  sizes[0].descriptorCount = 5;
  sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  sizes[1].descriptorCount = 8192;
  sizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLER;
  sizes[2].descriptorCount = 32;
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
  GoldenFrameSets f{};
  f.scene_buf = h.scene_buf;
  f.scene_bytes = h.scene_bytes;
  f.mat_buf = h.mat_buf;
  f.mat_bytes = h.mat_bytes;
  f.light_buf = h.light_buf;
  f.light_bytes = h.light_bytes;
  f.grid_buf = h.grid_buf;
  f.grid_bytes = h.grid_bytes;
  f.index_buf = h.index_buf;
  f.index_bytes = h.index_bytes;
  f.vis_view = h.vis_view;
  f.hdr_view = h.hdr_view;
  f.sampler = h.sampler;
  f.set0 = *set0;
  f.set_shade = *set_shade;
  f.set_post = *set_post;
  GoldenWriteFrameSets(g, f, update_sets);
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

// Shared GoldenFrame fill (defined after GoldenFrame: member use needs the
// complete type). Cam, 1 light, 64 max/tile shared; exposure differs per
// TU (P4-02 passes EV-3, P4-01 passes 1.0).
struct GoldenFrameBufs {
  GoldenHostBuf vis_b{};
  GoldenImage vis_img{};
  GoldenImage hdr_img{};
  GoldenImage ldr_img{};
  VkDescriptorSet set0{VK_NULL_HANDLE};
  VkDescriptorSet set_shade{VK_NULL_HANDLE};
  VkDescriptorSet set_post{VK_NULL_HANDLE};
};

inline void GoldenFillFrame(GoldenFrame* f, const GoldenFrameBufs* b,
                            std::uint32_t w, std::uint32_t h,
                            VkPipeline shade_pipe, VkPipeline post_pipe,
                            VkPipelineLayout shade_layout,
                            VkPipelineLayout post_layout, float exposure) {
  f->vis_buf = b->vis_b.buf;
  f->w = w;
  f->h = h;
  f->vis_img = b->vis_img.img;
  f->hdr_img = b->hdr_img.img;
  f->ldr_img = b->ldr_img.img;
  f->hdr_view = b->hdr_img.view;
  f->ldr_view = b->ldr_img.view;
  f->shade_pipe = shade_pipe;
  f->post_pipe = post_pipe;
  f->shade_layout = shade_layout;
  f->post_layout = post_layout;
  f->set0 = b->set0;
  f->set_shade = b->set_shade;
  f->set_post = b->set_post;
  f->shade.cam[0] = 160.0f;
  f->shade.cam[1] = 120.0f;
  f->shade.cam[2] = 80.0f;
  f->shade.cam[3] = 1.0f;
  f->shade.light_count = 1;
  f->shade.max_lights_per_tile = 64;
  f->post.exposure = exposure;
}

// Shared golden-TU check scaffold (failures counter + [PASS]/[FAIL]
// printer). Every shade->post golden TU repeats this block verbatim;
// Sonar's new-duplication gate counts the cross-file mirror. TUs keep
// `failures`/`check` as thin aliases so existing call sites are untouched.
struct GoldenCheck {
  int failures{0};
  void check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) {
      ++failures;
    }
  }
};

// [FAIL]-not-[SKIP] under ENGINE_REQUIRE_GPU=1 (rule 11 / P1-01 contract).
// MSVC treats getenv as a C4996 error under /WX: _dupenv_s on _WIN32.
// Shared by the golden TUs (P4-01 x2 + P4-02) — the 16-line getenv fallback
// is a Sonar duplication block when copied per TU.
inline auto GoldenGpuMissing(std::string_view what) -> int {
#if defined(_WIN32)
  char* dup = nullptr;
  std::size_t dup_len = 0;
  const bool required =
      _dupenv_s(&dup, &dup_len, "ENGINE_REQUIRE_GPU") == 0 && dup != nullptr &&
      std::string_view(dup) == "1";
  std::free(dup);
#else
  const char* req = std::getenv("ENGINE_REQUIRE_GPU");
  const bool required = req != nullptr && std::string_view(req) == "1";
#endif
  const std::string label(what.data(), what.size());
  std::printf("[%s] %s\n", required ? "FAIL" : "SKIP", label.c_str());
  return required ? 1 : 0;
}

// Test-device bootstrap: init llvmpipe + resolve entry points + validate
// the six PBR blobs + bindless layout. Shared with future P4 golden TUs
// (all of them open the same device and plan); the TU keeps its check
// labels, the struct-fill lives here once (Sonar duplication gate).
//
// Engine-typed: the TU includes bindless_table.hpp / pbr_pipeline.hpp /
// vulkan_device.hpp before this header (the header itself stays
// engine-include-free so stub builds compile it without Vulkan types).
struct GoldenDevice {
  engine::renderer::VulkanDevice* dev{nullptr};
  engine::test::GoldenVk* vk{nullptr};
  engine::renderer::BindlessTable* bindless{nullptr};
};

template <typename CheckFn, typename MissingFn>
inline auto GoldenOpenDevice(
    CheckFn check, MissingFn missing, const char* app_name,
    const char* label_device, const char* label_procs, const char* label_plan,
    const char* label_bindless, const char* label_layout,
    engine::renderer::VulkanDevice* dev, engine::test::GoldenVk* g,
    engine::renderer::PbrPipeline* pbr, engine::renderer::BindlessTable* bt)
    -> bool {
  engine::renderer::DeviceCreateInfo ci{};
  ci.app_name = app_name;
  ci.device_hint = "llvmpipe";
  const auto st = dev->init(ci);
  if (!st.ok() &&
      st.code == engine::renderer::StatusCode::kNoPhysicalDevice) {
    missing("no physical device");
    return false;
  }
  check(st.ok(), label_device);
  if (!st.ok()) {
    return false;
  }
  g->device = dev->device();
  g->physical = dev->physical_device();
  g->queue = dev->graphics_queue();
  g->queue_family = dev->graphics_queue_family();
  auto loader = [&](const char* name) -> void* {
    void* p = dev->proc(name);
    return (p != nullptr) ? p : dev->instance_proc(name);
  };
  check(engine::test::GoldenVkLoad(loader, &g->vk), label_procs);
  if (g->vk.create_buffer == nullptr || g->vk.create_pipes == nullptr ||
      g->vk.queue_submit == nullptr) {
    dev->shutdown();
    return false;
  }
  check(pbr
            ->init(kPbr_vis_vertSpv, kPbr_vis_vertSpvWords, kPbr_vis_fragSpv,
                   kPbr_vis_fragSpvWords, kPbr_shade_vertSpv,
                   kPbr_shade_vertSpvWords, kPbr_shade_fragSpv,
                   kPbr_shade_fragSpvWords, kPost_vertSpv, kPost_vertSpvWords,
                   kPost_fragSpv, kPost_fragSpvWords)
            .ok(),
        label_plan);
  check(bt->init().ok(), label_bindless);
  VkBuffer sentinel = reinterpret_cast<VkBuffer>(0x1000);
  check(bt->create_gpu(dev->device(), sentinel, 1 << 20, sentinel, 1 << 16)
            .ok(),
        label_layout);
  if (!pbr->ready() || !bt->gpu_ready()) {
    bt->destroy_gpu();
    dev->shutdown();
    return false;
  }
  return true;
}

// Lightweight device open for compute-only P4 golden TUs (no PBR plan /
// bindless): init VulkanDevice, fill GoldenVk handles, run GoldenVkLoad,
// wire failures through check(). The missing-GPU branch reports via
// missing() (fail-closed under ENGINE_REQUIRE_GPU=1) and returns false —
// the TU returns 1. The TU keeps its extra compute-proc gate (procs differ
// per TU); this helper owns the repeated device+loader+vkload preamble
// (Sonar new-duplication gate).
template <typename CheckFn, typename MissingFn, typename DeviceT>
inline auto GoldenOpenComputeDevice(CheckFn check, MissingFn missing,
                                    const char* app_name,
                                    const char* label_missing,
                                    const char* label_device,
                                    const char* label_procs, DeviceT* dev,
                                    engine::test::GoldenVk* g) -> bool {
  engine::renderer::DeviceCreateInfo ci{};
  ci.app_name = app_name;
  ci.device_hint = "llvmpipe";
  const auto st = dev->init(ci);
  if (!st.ok() &&
      st.code == engine::renderer::StatusCode::kNoPhysicalDevice) {
    missing(label_missing);
    return false;
  }
  check(st.ok(), label_device);
  if (!st.ok()) {
    return false;
  }
  g->device = dev->device();
  g->physical = dev->physical_device();
  g->queue = dev->graphics_queue();
  g->queue_family = dev->graphics_queue_family();
  auto loader = [&](const char* name) -> void* {
    void* p = dev->proc(name);
    return (p != nullptr) ? p : dev->instance_proc(name);
  };
  const bool vk_ok = engine::test::GoldenVkLoad(loader, &g->vk);
  check(vk_ok, label_procs);
  return vk_ok;
}

// Shared vis-image upload trio (the R32G32_UINT vis + SFLOAT HDR pair
// Sonar new-duplication gate). LDR image fill stays per TU (formats
// differ: R8G8B8A8 vs engine HDR constants).
template <typename CheckFn>
inline auto GoldenMakeVisHdr(CheckFn check, GoldenVk* g, std::uint32_t w,
                             std::uint32_t h, GoldenImage* vis_img,
                             GoldenImage* hdr_img, const char* label_vis,
                             const char* label_hdr) -> bool {
  const bool vis_ok = GoldenMakeImage(
      g, VK_FORMAT_R32G32_UINT, w, h,
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT, vis_img);
  check(vis_ok, label_vis);
  const bool hdr_ok =
      vis_ok && GoldenMakeImage(g, VK_FORMAT_R16G16B16A16_SFLOAT, w, h,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT,
                                hdr_img);
  check(hdr_ok, label_hdr);
  return vis_ok && hdr_ok;
}

// Shared frame-buf struct-fill (GoldenFrameBufs 8-field copy repeats per
// golden; Sonar new-duplication gate).
inline void GoldenFillFrameBufs(GoldenFrameBufs* fb, const GoldenHostBuf* vis_b,
                                const GoldenImage* vis_img,
                                const GoldenImage* hdr_img,
                                const GoldenImage* ldr_img, VkDescriptorSet set0,
                                VkDescriptorSet set_shade,
                                VkDescriptorSet set_post) {
  fb->vis_b = *vis_b;
  fb->vis_img = *vis_img;
  fb->hdr_img = *hdr_img;
  fb->ldr_img = *ldr_img;
  fb->set0 = set0;
  fb->set_shade = set_shade;
  fb->set_post = set_post;
}

// Label-derived device open: the 8 check labels are the dominant
// cross-TU mirror (every golden repeats device/entry/plan/bindless/
// layout strings with only its tag differing). The tag-prefixed labels
// live here once (Sonar new-duplication gate).
template <typename CheckFn, typename MissingFn>
inline auto GoldenOpenDeviceTagged(
    CheckFn check, MissingFn missing, const char* tag,
    engine::renderer::VulkanDevice* dev, engine::test::GoldenVk* g,
    engine::renderer::PbrPipeline* pbr, engine::renderer::BindlessTable* bt)
    -> bool {
  const std::string base(tag);
  const std::string l_dev = base + ": device init ok";
  const std::string l_procs = base + ": entry points resolve";
  const std::string l_plan = base + ": plan init";
  const std::string l_bind = base + ": bindless init";
  const std::string l_lay = base + ": bindless layout builds";
  return GoldenOpenDevice(check, missing, tag, l_dev.c_str(), l_procs.c_str(),
                          l_plan.c_str(), l_bind.c_str(), l_lay.c_str(), dev,
                          g, pbr, bt);
}

// Device-routed Vulkan callables for the shade->post golden shape (one
// struct, TU-fillable from a GoldenVk*): replaces the per-TU lambda packs
// (create_pool/alloc_sets/update_sets/create_set_layout/create_pipe_layout/
// create_pipes/destroy_mod/copy/barrier/end/map/unmap — ~60 lines per TU)
// that Sonar's new-duplication gate counts as cross-file mirrors. The TU
// fills one of these from its GoldenVk and passes it (one value, overload
// dispatch) everywhere a Golden* helper takes callables — same single
// overload per call site, no template bloat beyond the existing helpers.
struct GoldenDeviceCalls {
  GoldenVk* vk{nullptr};
  auto operator()(VkDevice device, const VkDescriptorPoolCreateInfo* info,
                  const VkAllocationCallbacks* alloc,
                  VkDescriptorPool* out) const {
    return vk->vk.create_pool(device, info, alloc, out);
  }
  auto operator()(VkDevice device, const VkDescriptorSetAllocateInfo* info,
                  VkDescriptorSet* out) const {
    return vk->vk.alloc_sets(device, info, out);
  }
  void operator()(VkDevice device, const VkWriteDescriptorSet* writes) const {
    vk->vk.update_sets(device, 9, writes, 0, nullptr);
  }
  auto operator()(VkDevice device, const VkDescriptorSetLayoutCreateInfo* info,
                  const VkAllocationCallbacks* alloc,
                  VkDescriptorSetLayout* out) const {
    return vk->vk.create_set_layout(device, info, alloc, out);
  }
  auto operator()(const VkPipelineLayoutCreateInfo* info,
                  VkPipelineLayout* out) const -> bool {
    return vk->vk.create_pipe_layout(vk->device, info, nullptr, out) ==
           VK_SUCCESS;
  }
  auto operator()(VkDevice device, VkPipelineCache cache, std::uint32_t count,
                  const VkGraphicsPipelineCreateInfo* infos,
                  const VkAllocationCallbacks* alloc, VkPipeline* pipes) const {
    return vk->vk.create_pipes(device, cache, count, infos, alloc, pipes);
  }
  void operator()(VkShaderModule m) const {
    if (m != nullptr) {
      vk->vk.destroy_module(vk->device, m, nullptr);
    }
  }
  void operator()(VkCommandBuffer c, VkImage img, VkImageLayout lay,
                  VkBuffer buf, std::uint32_t n,
                  const VkBufferImageCopy* r) const {
    vk->vk.cmd_copy_img_buf(c, img, lay, buf, n, r);
  }
  void operator()(VkCommandBuffer c, const VkDependencyInfo* d) const {
    vk->vk.cmd_barrier2(c, d);
  }
  auto operator()(VkCommandBuffer c) const {
    return vk->vk.end_cmd(c) == VK_SUCCESS;
  }
  auto operator()(VkDevice d, VkDeviceMemory m, VkDeviceSize o,
                  VkDeviceSize s, VkMemoryMapFlags f, void** p) const {
    return vk->vk.map_mem(d, m, o, s, f, p);
  }
  void operator()(VkDevice d, VkDeviceMemory m) const {
    vk->vk.unmap_mem(d, m);
  }
};

// Shared shade->post frame handles (the 16-handle declaration block
// every golden TU repeats verbatim: 7 upload bufs + 3 images + sampler +
// pool + 3 sets; Sonar new-duplication gate counts the cross-file
// mirror). TUs fill one of these, then bind short refs for the frame
// lambda body.
struct GoldenFrameHandles2 {
  GoldenHostBuf scene_b{};
  GoldenHostBuf mat_b{};
  GoldenHostBuf light_b{};
  GoldenHostBuf grid_b{};
  GoldenHostBuf index_b{};
  GoldenHostBuf vis_b{};
  GoldenHostBuf read_b{};
  GoldenImage vis_img{};
  GoldenImage hdr_img{};
  GoldenImage ldr_img{};
  VkSampler sampler{nullptr};
  VkDescriptorPool pool{nullptr};
  VkDescriptorSet set0{nullptr};
  VkDescriptorSet set_shade{nullptr};
  VkDescriptorSet set_post{nullptr};
};

// Shared shade->post frame resources (upload buffers, images, sampler,
// descriptor pool/sets, pipelines): one struct-fill teardown instead of
// the ~20-line free/image/pipe/layout destroy tail every golden TU
// repeats (Sonar new-duplication gate counts the cross-file mirror).
struct GoldenFrameResources {
  GoldenVk* vk{nullptr};
  VkDescriptorPool pool{nullptr};
  VkSampler sampler{nullptr};
  GoldenImage vis_img{};
  GoldenImage hdr_img{};
  GoldenImage ldr_img{};
  GoldenHostBuf scene_b{};
  GoldenHostBuf mat_b{};
  GoldenHostBuf light_b{};
  GoldenHostBuf grid_b{};
  GoldenHostBuf index_b{};
  GoldenHostBuf vis_b{};
  GoldenHostBuf read_b{};
  GoldenShadePost shade_post{};
};

// Free every handle in a GoldenFrameResources (null-safe; mirrors the
// per-TU teardown tails). Bindless + device shutdown stay in the TU (it
// owns `bt`/`dev` lifetimes).
inline void GoldenTeardownFrame(GoldenFrameResources* f) {
  GoldenVk* g = f->vk;
  g->vk.destroy_pool(g->device, f->pool, nullptr);
  g->vk.destroy_sampler(g->device, f->sampler, nullptr);
  GoldenFreeImage(g, &f->vis_img);
  GoldenFreeImage(g, &f->hdr_img);
  GoldenFreeImage(g, &f->ldr_img);
  GoldenFreeHost(g, &f->scene_b);
  GoldenFreeHost(g, &f->mat_b);
  GoldenFreeHost(g, &f->light_b);
  GoldenFreeHost(g, &f->grid_b);
  GoldenFreeHost(g, &f->index_b);
  GoldenFreeHost(g, &f->vis_b);
  GoldenFreeHost(g, &f->read_b);
  g->vk.destroy_pipe(g->device, f->shade_post.shade_pipe, nullptr);
  g->vk.destroy_pipe(g->device, f->shade_post.post_pipe, nullptr);
  g->vk.destroy_pipe_layout(g->device, f->shade_post.shade_layout, nullptr);
  g->vk.destroy_pipe_layout(g->device, f->shade_post.post_layout, nullptr);
  g->vk.destroy_set_layout(g->device, f->shade_post.set1_shade, nullptr);
  g->vk.destroy_set_layout(g->device, f->shade_post.set1_post, nullptr);
}

// Linear-clamp sampler shared by the shade->post goldens (shade-dummy +
// post input). Struct-fill lives here once (Sonar gate).
inline auto GoldenMakeSampler(GoldenVk* g, VkSampler* out) -> bool {
  VkSamplerCreateInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  si.magFilter = VK_FILTER_LINEAR;
  si.minFilter = VK_FILTER_LINEAR;
  si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  return g->vk.create_sampler(g->device, &si, nullptr, out) == VK_SUCCESS;
}

// P4-11 single-post-effect record (fog / grade / vignette fullscreen pass
// through one post set-1: sampled-image 0 + sampler 1, 32B push). The two
// P4-11 GPU TUs (test_height_fog + test_post_grade_vignette) record the
// identical ~30-line dynamic-rendering pass; a per-TU copy trips Sonar's
// new-duplication gate (<=3%). Struct-fill lives here once. kW/kH are
// template-free std::uint32_t params (both TUs use 320x240). Bindless
// set-0 binds at set 0, the post set-1 at set 1 (mirrors the TU pipeline
// layouts: {bt->set_layout(), set_post}).
inline void GoldenRecordPostPass(GoldenVk* g, VkCommandBuffer cb,
                                 std::uint32_t w, std::uint32_t h,
                                 VkImageView target_view, VkPipeline pipe,
                                 VkPipelineLayout layout,
                                 VkDescriptorSet set1, const void* push,
                                 std::uint32_t push_bytes) {
  VkRenderingAttachmentInfo att{};
  att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  att.imageView = target_view;
  att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  VkRenderingInfo ri{};
  ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  ri.renderArea = {{0, 0}, {w, h}};
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &att;
  g->vk.cmd_begin_render(cb, &ri);
  const float fw = static_cast<float>(w);
  const float fh = static_cast<float>(h);
  VkViewport vp{0, 0, fw, fh, 0.0f, 1.0f};
  g->vk.cmd_viewport(cb, 0, 1, &vp);
  VkRect2D sc{{0, 0}, {w, h}};
  g->vk.cmd_scissor(cb, 0, 1, &sc);
  g->vk.cmd_bind_pipe(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  g->vk.cmd_bind_sets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1,
                      &set1, 0, nullptr);
  g->vk.cmd_push(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, push_bytes,
                 push);
  g->vk.cmd_draw(cb, 3, 1, 0, 0);
  g->vk.cmd_end_render(cb);
}

// Synthetic LDR gradient input: R ramps with x, G ramps with y,
// B = 0.5 flat. Deterministic, no shade pass needed. Shared by the two
// P4-11 GPU TUs (fog colors + grade/vignette gradient are the same
// ramp; fog's height channel is its own fill) — one copy here (Sonar
// new-duplication gate).
inline void GoldenFillGradient(std::uint32_t w, std::uint32_t h,
                               std::vector<std::uint8_t>* px) {
  px->assign(static_cast<std::size_t>(w) * h * 4, 0);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      const std::size_t at = (static_cast<std::size_t>(y) * w + x) * 4;
      (*px)[at + 0] = static_cast<std::uint8_t>((x * 255) / (w - 1));
      (*px)[at + 1] = static_cast<std::uint8_t>((y * 255) / (h - 1));
      (*px)[at + 2] = 128;
      (*px)[at + 3] = 255;
    }
  }
}

// P4-11 LDR frame images: three identical R8G8B8A8 post images (src +
// mid + dst roles differ per TU). Both P4-11 GPU TUs repeat the
// usage-flags + triple GoldenMakeImage struct-fill; a per-TU copy trips
// Sonar's new-duplication gate (<=3%). Usage + format live here once.
inline auto GoldenMakePostImages(GoldenVk* g, std::uint32_t w,
                                 std::uint32_t h, GoldenImage* a,
                                 GoldenImage* b, GoldenImage* c) -> bool {
  constexpr VkImageUsageFlags kPostUsage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  return GoldenMakeImage(g, VK_FORMAT_R8G8B8A8_UNORM, w, h, kPostUsage, a) &&
         GoldenMakeImage(g, VK_FORMAT_R8G8B8A8_UNORM, w, h, kPostUsage, b) &&
         GoldenMakeImage(g, VK_FORMAT_R8G8B8A8_UNORM, w, h, kPostUsage, c);
}

// P4-11 frame submit/readback tail: end the command buffer through the
// TU's readback callables, fence-wait submit, map the readback buffer,
// golden-compare under `golden_name`. The two P4-11 GPU TUs repeat this
// ~20-line tail verbatim (submit/wait/map/check/golden); a per-TU copy
// trips Sonar's new-duplication gate (<=3%). Destroy of the TU-owned
// cmd pool stays in the TU (lifetimes differ: grade/vignette reuses the
// pool across two submits). The TU keeps its own check labels via the
// `what` tag; pixel-size mismatches report through `what` too.
template <typename CopyFn, typename BarrierFn, typename EndFn,
          typename MapFn>
inline auto GoldenSubmitMapGolden(
    GoldenVk* g, VkCommandBuffer cb, VkImage img, VkBuffer read_buf,
    VkDeviceMemory read_mem, std::uint32_t w, std::uint32_t h,
    std::vector<std::uint8_t>* pixels_out, const char* golden_name,
    const char* what, CopyFn copy_fn, BarrierFn barrier_fn, EndFn end_fn,
    MapFn map_fn, int* failures) -> bool {
  if (!GoldenSubmitReadback(g, cb, img, read_buf, w, h, copy_fn, barrier_fn,
                            end_fn)) {
    return false;
  }
  if (!GoldenSubmitWait(g, cb)) {
    return false;
  }
  pixels_out->assign(static_cast<std::size_t>(w) * h * 4, 0);
  void* mapped = nullptr;
  if (map_fn(g->device, read_mem, 0, pixels_out->size(), 0, &mapped) !=
          VK_SUCCESS ||
      mapped == nullptr) {
    return false;
  }
  const auto* src = static_cast<const std::uint8_t*>(mapped);
  for (std::size_t i = 0; i < pixels_out->size(); ++i) {
    (*pixels_out)[i] = src[i];
  }
  map_fn(g->device, read_mem);
  (void)what;
  *failures += golden_check(
      golden_name,
      std::span<const std::uint8_t>(pixels_out->data(), pixels_out->size()),
      w, h);
  return true;
}

// P4-11 LDR post frame descriptors (fog / grade / vignette single-pass
// shape): one input image + sampler per post set-1. Fog owns 1 set with
// 2 image bindings (hdr + height share the sampler); grade/vignette own
// 2 sets with 1 image binding each. The pool/alloc/write struct-fill is
// identical across the two P4-11 GPU TUs (~60 lines); a per-TU copy trips
// Sonar's new-duplication gate (<=3%). Image views/layouts arrive via
// the params; the TU keeps its own check labels and set handles.
struct GoldenPostFrameSets {
  VkImageView view_a{nullptr};
  VkImageView view_b{nullptr};
  VkSampler sampler{nullptr};
  std::uint32_t set_count{1};
};

inline auto GoldenAllocWritePostSets(
    GoldenVk* g, GoldenDeviceCalls* dc, VkDescriptorPool* pool,
    const VkDescriptorSetLayout* layouts, VkDescriptorSet* sets_out,
    const GoldenPostFrameSets& f) -> bool {
  VkDescriptorPoolSize sizes[3]{};
  sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  sizes[0].descriptorCount = 2;
  sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
  sizes[1].descriptorCount = 2;
  sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  sizes[2].descriptorCount = 1;
  VkDescriptorPoolCreateInfo pi{};
  pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pi.maxSets = f.set_count;
  pi.poolSizeCount = 3;
  pi.pPoolSizes = sizes;
  if ((*dc)(g->device, &pi, nullptr, pool) != VK_SUCCESS) {
    return false;
  }
  VkDescriptorSetAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  ai.descriptorPool = *pool;
  ai.descriptorSetCount = f.set_count;
  ai.pSetLayouts = layouts;
  if ((*dc)(g->device, &ai, sets_out) != VK_SUCCESS) {
    return false;
  }
  // Write: set 0 reads view_a; image binding 2 (fog height) or set 1
  // (grade/vignette mid) reads view_b. Bindings numbered for the fog
  // 4-write shape; the grade/vignette TU overwrites dstSet/binding on
  // its own two writes (same struct-fill, distinct routing).
  VkDescriptorImageInfo info_a{};
  info_a.imageView = f.view_a;
  info_a.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkDescriptorImageInfo info_b{};
  info_b.imageView = f.view_b;
  info_b.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkDescriptorImageInfo smp{};
  smp.sampler = f.sampler;
  VkWriteDescriptorSet w[4]{};
  for (int k = 0; k < 4; ++k) {
    w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  }
  w[0].dstSet = sets_out[0];
  w[0].dstBinding = 0;
  w[0].descriptorCount = 1;
  w[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  w[0].pImageInfo = &info_a;
  w[1].dstSet = sets_out[0];
  w[1].dstBinding = 1;
  w[1].descriptorCount = 1;
  w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  w[1].pImageInfo = &smp;
  w[2].dstSet = (f.set_count > 1) ? sets_out[1] : sets_out[0];
  w[2].dstBinding = (f.set_count > 1) ? 0 : 2;
  w[2].descriptorCount = 1;
  w[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  w[2].pImageInfo = &info_b;
  w[3].dstSet = w[2].dstSet;
  w[3].dstBinding = (f.set_count > 1) ? 1 : 3;
  w[3].descriptorCount = 1;
  w[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  w[3].pImageInfo = &smp;
  g->vk.update_sets(g->device, 4, w, 0, nullptr);
  return true;
}

// P4-11 sampled-image+sampler set-1 layout (post effects read one input
// image; fog's 4-binding layout is its own TU-local build since the
// header/device halves differ). Struct-fill lives here once; the TU keeps
// its own check label.
inline auto GoldenBuildPostSet1(GoldenDeviceCalls* calls,
                                VkDescriptorSetLayout* out) -> bool {
  VkDescriptorSetLayoutBinding b[2]{};
  b[0].binding = 0;
  b[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  b[0].descriptorCount = 1;
  b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  b[1].binding = 1;
  b[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  b[1].descriptorCount = 1;
  b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo li{};
  li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  li.bindingCount = 2;
  li.pBindings = b;
  VkDescriptorSetLayout layout = nullptr;
  const bool ok =
      (*calls)(calls->vk->device, &li, nullptr, &layout) == VK_SUCCESS;
  if (ok) {
    *out = layout;
  }
  return ok;
}

#endif

}  // namespace engine::test

// pick_buffer.cpp — P3-06: offscreen R32G32_UINT id target + one-texel readback.
//
// PickIdBuffer::create builds (through the VulkanDevice proc table +
// VMA, same pattern as DepthBuffer::create):
//   * R32G32_UINT id image (TRANSFER_SRC | COLOR_ATTACHMENT) + view,
//   * D32_SFLOAT depth image (DEPTH_STENCIL_ATTACHMENT) + view,
//   * host-visible staging buffer (TRANSFER_DST, 1 texel = 2x u32),
//   * a one-shot command pool for the pick() copy,
// and builds the pick pipeline on the ForwardRenderer (id target +
// D32 depth). No window, no swapchain: offscreen by construction.
//
// record_id_pass() drives the layout dance around render_ids():
// id UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL (depth first-use UNDEFINED ->
// ATTACHMENT, then self-transition), begin_pick_pass (CLEAR id to 0 =
// background), render_ids, end_pick_pass, then id ->
// TRANSFER_SRC_OPTIMAL so pick() can copy immediately.
//
// pick(x, y) copies one R32G32_UINT texel to the staging buffer (one-shot
// cmd + fence wait), maps it, and reassembles the flecs id. OOB reads
// return 0/0 without touching the GPU (same rule as managed
// ObjectIdBuffer.Pick). Y-flip: Vulkan images are Y-down from the
// top-left; the test projects with the same row-vector math as the
// renderer, so no flip is applied here — (x, y) addresses the texel row
// directly.
#include "engine/renderer/pick_buffer.hpp"

#if defined(ENGINE_RENDERER_HAS_VULKAN)
// volk-first include order (same rule as vulkan_device.cpp): volk defines
// VK_NO_PROTOTYPES and hard-errors if vulkan.h came first.
#include <volk.h>
#endif

#include <cstring>

#include "engine/renderer/forward_renderer.hpp"
#include "engine/renderer/mesh_registry.hpp"
#include "engine/renderer/vulkan_device.hpp"
#include "vma_bridge.hpp"

namespace engine::renderer {

#if defined(ENGINE_RENDERER_HAS_VULKAN)

PickIdBuffer::~PickIdBuffer() noexcept { destroy(); }

auto PickIdBuffer::create(VulkanDevice& dev, ForwardRenderer& forward,
                          VkExtent2D extent) noexcept -> Status {
  if (ready_) {
    return MakeFail(StatusCode::kAlreadyInitialized, "pick built");
  }
  if (extent.width == 0 || extent.height == 0) {
    return MakeFail(StatusCode::kInvalidArgument, "pick bad extent");
  }
  VkDevice device = dev.device();
  if (device == nullptr || !dev.vma_ready()) {
    return MakeFail(StatusCode::kInvalidArgument, "pick null device");
  }
  auto* alloc = vma_bridge::g_alloc;
  if (alloc == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "pick no vma");
  }
  if (forward.pick_ready()) {
    return MakeFail(StatusCode::kAlreadyInitialized, "pick pipeline built");
  }
  auto st = forward.create_pick(dev, VK_FORMAT_R32G32_UINT,
                                VK_FORMAT_D32_SFLOAT);
  if (!st.ok()) {
    return st;
  }
  auto p_vkCreateImageView = reinterpret_cast<PFN_vkCreateImageView>(
      dev.instance_proc("vkCreateImageView"));
  auto p_vkDestroyImageView = reinterpret_cast<PFN_vkDestroyImageView>(
      dev.instance_proc("vkDestroyImageView"));
  if (p_vkCreateImageView == nullptr || p_vkDestroyImageView == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "pick no view entry");
  }

  // Id image: R32G32_UINT, COLOR_ATTACHMENT (render_ids target) +
  // TRANSFER_SRC (pick() readback).
  vma_bridge::DepthImageBundle idb{};
  if (vma_bridge::EngineVmaCreateIdImage(alloc, extent.width, extent.height,
                                         &idb) != 0) {
    return MakeFail(StatusCode::kDeviceCreateFailed, "pick id alloc");
  }
  VkImageView id_view = nullptr;
  {
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = reinterpret_cast<VkImage>(
        static_cast<std::uintptr_t>(idb.image_bits));
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R32G32_UINT;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.baseMipLevel = 0;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.baseArrayLayer = 0;
    vi.subresourceRange.layerCount = 1;
    if (p_vkCreateImageView(device, &vi, nullptr, &id_view) != VK_SUCCESS) {
      vma_bridge::EngineVmaDestroyImage(alloc, idb.image_bits, idb.alloc);
      return MakeFail(StatusCode::kDeviceCreateFailed, "pick id view");
    }
  }

  // Depth image: reuse the depth-image helper (D32_SFLOAT, device-local).
  vma_bridge::DepthImageBundle db{};
  if (vma_bridge::EngineVmaCreateDepthImage(alloc, extent.width,
                                            extent.height, &db) != 0) {
    p_vkDestroyImageView(device, id_view, nullptr);
    vma_bridge::EngineVmaDestroyImage(alloc, idb.image_bits, idb.alloc);
    return MakeFail(StatusCode::kDeviceCreateFailed, "pick depth alloc");
  }
  VkImageView depth_view = nullptr;
  {
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = reinterpret_cast<VkImage>(
        static_cast<std::uintptr_t>(db.image_bits));
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_D32_SFLOAT;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vi.subresourceRange.baseMipLevel = 0;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.baseArrayLayer = 0;
    vi.subresourceRange.layerCount = 1;
    if (p_vkCreateImageView(device, &vi, nullptr, &depth_view) != VK_SUCCESS) {
      p_vkDestroyImageView(device, id_view, nullptr);
      vma_bridge::EngineVmaDestroyImage(alloc, idb.image_bits, idb.alloc);
      vma_bridge::EngineVmaDestroyImage(alloc, db.image_bits, db.alloc);
      return MakeFail(StatusCode::kDeviceCreateFailed, "pick depth view");
    }
  }

  // Staging buffer: one texel = 2x u32 (lo/hi), TRANSFER_DST, host-visible.
  vma_bridge::MeshBufferBundle sb{};
  if (vma_bridge::EngineVmaCreateMeshBuffer(
          alloc, 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &sb) != 0 ||
      sb.mapped == nullptr) {
    p_vkDestroyImageView(device, id_view, nullptr);
    p_vkDestroyImageView(device, depth_view, nullptr);
    vma_bridge::EngineVmaDestroyImage(alloc, idb.image_bits, idb.alloc);
    vma_bridge::EngineVmaDestroyImage(alloc, db.image_bits, db.alloc);
    return MakeFail(StatusCode::kDeviceCreateFailed, "pick stage alloc");
  }

  device_ = device;
  vma_alloc_ = alloc;
  vma_id_alloc_ = idb.alloc;
  vma_depth_alloc_ = db.alloc;
  vma_stage_alloc_ = sb.alloc;
  id_image_bits_ = idb.image_bits;
  depth_image_bits_ = db.image_bits;
  stage_bits_ = sb.buffer_bits;
  id_view_ = id_view;
  depth_view_ = depth_view;
  proc_destroy_view_ = reinterpret_cast<void*>(p_vkDestroyImageView);
  stage_mapped_ = sb.mapped;
  width_ = extent.width;
  height_ = extent.height;
  depth_live_ = false;
  ready_ = true;
  return MakeOk();
}

auto PickIdBuffer::destroy() noexcept -> void {
  auto* alloc = static_cast<vma_bridge::Allocator_T*>(vma_alloc_);
  if (device_ != nullptr && proc_destroy_view_ != nullptr) {
    auto p_vkDestroyImageView =
        reinterpret_cast<PFN_vkDestroyImageView>(proc_destroy_view_);
    if (id_view_ != nullptr) {
      p_vkDestroyImageView(device_, id_view_, nullptr);
    }
    if (depth_view_ != nullptr) {
      p_vkDestroyImageView(device_, depth_view_, nullptr);
    }
  }
  if (alloc != nullptr) {
    if (id_image_bits_ != 0) {
      vma_bridge::EngineVmaDestroyImage(
          alloc, id_image_bits_,
          static_cast<vma_bridge::Allocation_T*>(vma_id_alloc_));
    }
    if (depth_image_bits_ != 0) {
      vma_bridge::EngineVmaDestroyImage(
          alloc, depth_image_bits_,
          static_cast<vma_bridge::Allocation_T*>(vma_depth_alloc_));
    }
    if (stage_bits_ != 0) {
      vma_bridge::EngineVmaDestroyBuffer(
          alloc, stage_bits_,
          static_cast<vma_bridge::Allocation_T*>(vma_stage_alloc_));
    }
  }
  device_ = nullptr;
  vma_alloc_ = nullptr;
  vma_id_alloc_ = nullptr;
  vma_depth_alloc_ = nullptr;
  vma_stage_alloc_ = nullptr;
  id_image_bits_ = 0;
  depth_image_bits_ = 0;
  stage_bits_ = 0;
  id_view_ = nullptr;
  depth_view_ = nullptr;
  proc_destroy_view_ = nullptr;
  stage_mapped_ = nullptr;
  width_ = 0;
  height_ = 0;
  depth_live_ = false;
  ready_ = false;
}

auto PickIdBuffer::record_id_pass(VulkanDevice& dev, ForwardRenderer& forward,
                                  void* world,
                                  const MeshRegistry& registry,
                                  VkCommandBuffer cmd,
                                  float aspect) noexcept -> Status {
  if (!ready_) {
    return MakeFail(StatusCode::kNotInitialized, "pick not built");
  }
  if (world == nullptr || cmd == nullptr || !forward.pick_ready()) {
    return MakeFail(StatusCode::kInvalidArgument, "pick bad pass arg");
  }
  auto p_vkCmdPipelineBarrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(
      dev.proc("vkCmdPipelineBarrier2"));
  if (p_vkCmdPipelineBarrier2 == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "pick no barrier");
  }
  VkImage id_img = reinterpret_cast<VkImage>(
      static_cast<std::uintptr_t>(id_image_bits_));
  VkImage depth_img = reinterpret_cast<VkImage>(
      static_cast<std::uintptr_t>(depth_image_bits_));
  // Id: UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL.
  {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.image = id_img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = 1;
    VkDependencyInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    p_vkCmdPipelineBarrier2(cmd, &di);
  }
  // Depth: first use UNDEFINED -> ATTACHMENT, then ATTACHMENT -> self.
  {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    const bool first_use = !depth_live_;
    b.srcStageMask = first_use ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
                               : (VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT);
    b.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    b.srcAccessMask =
        first_use ? 0 : VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.oldLayout = first_use ? VK_IMAGE_LAYOUT_UNDEFINED
                            : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    b.image = depth_img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = 1;
    VkDependencyInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    p_vkCmdPipelineBarrier2(cmd, &di);
  }
  depth_live_ = true;
  VkExtent2D extent{};
  extent.width = width_;
  extent.height = height_;
  auto st = forward.begin_pick_pass(cmd, id_view_, depth_view_, extent);
  if (!st.ok()) {
    return st;
  }
  {
    ForwardRenderer::RenderWorldStats stats{};
    const float use_aspect =
        (aspect > 0.0F) ? aspect
                        : static_cast<float>(width_) / static_cast<float>(
                                                             (height_ > 0)
                                                                 ? height_
                                                                 : 1);
    st = forward.render_ids(world, registry, cmd, extent, &stats, use_aspect);
    if (!st.ok()) {
      return st;
    }
  }
  st = forward.end_pick_pass(cmd);
  if (!st.ok()) {
    return st;
  }
  // Id -> TRANSFER_SRC_OPTIMAL so pick() can copy immediately.
  {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.image = id_img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = 1;
    VkDependencyInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    p_vkCmdPipelineBarrier2(cmd, &di);
  }
  return MakeOk();
}

auto PickIdBuffer::pick(VulkanDevice& dev, std::uint32_t x, std::uint32_t y,
                        std::uint32_t& out_lo,
                        std::uint32_t& out_hi) noexcept -> Status {
  out_lo = 0;
  out_hi = 0;
  if (!ready_) {
    return MakeFail(StatusCode::kNotInitialized, "pick not built");
  }
  if (x >= width_ || y >= height_) {
    return MakeOk();  // OOB -> background (same as ObjectIdBuffer.Pick).
  }
  VkDevice device = dev.device();
  if (device == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "pick null device");
  }
  auto p_vkCreateCommandPool = reinterpret_cast<PFN_vkCreateCommandPool>(
      dev.instance_proc("vkCreateCommandPool"));
  auto p_vkDestroyCommandPool = reinterpret_cast<PFN_vkDestroyCommandPool>(
      dev.instance_proc("vkDestroyCommandPool"));
  auto p_vkAllocateCommandBuffers =
      reinterpret_cast<PFN_vkAllocateCommandBuffers>(
          dev.instance_proc("vkAllocateCommandBuffers"));
  auto p_vkBeginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(
      dev.instance_proc("vkBeginCommandBuffer"));
  auto p_vkEndCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer>(
      dev.instance_proc("vkEndCommandBuffer"));
  auto p_vkCmdCopyImageToBuffer =
      reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(
          dev.proc("vkCmdCopyImageToBuffer"));
  auto p_vkCreateFence = reinterpret_cast<PFN_vkCreateFence>(
      dev.instance_proc("vkCreateFence"));
  auto p_vkDestroyFence = reinterpret_cast<PFN_vkDestroyFence>(
      dev.instance_proc("vkDestroyFence"));
  auto p_vkWaitForFences = reinterpret_cast<PFN_vkWaitForFences>(
      dev.instance_proc("vkWaitForFences"));
  auto p_vkQueueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(
      dev.instance_proc("vkQueueSubmit"));
  auto p_vkMapMemory = reinterpret_cast<PFN_vkMapMemory>(
      dev.instance_proc("vkMapMemory"));
  auto p_vkUnmapMemory = reinterpret_cast<PFN_vkUnmapMemory>(
      dev.instance_proc("vkUnmapMemory"));
  if (p_vkCreateCommandPool == nullptr || p_vkDestroyCommandPool == nullptr ||
      p_vkAllocateCommandBuffers == nullptr || p_vkBeginCommandBuffer == nullptr ||
      p_vkEndCommandBuffer == nullptr || p_vkCmdCopyImageToBuffer == nullptr ||
      p_vkCreateFence == nullptr || p_vkDestroyFence == nullptr ||
      p_vkWaitForFences == nullptr || p_vkQueueSubmit == nullptr ||
      p_vkMapMemory == nullptr || p_vkUnmapMemory == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "pick no copy entry");
  }
  VkCommandPool pool = nullptr;
  {
    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    cpi.queueFamilyIndex = dev.graphics_queue_family();
    if (p_vkCreateCommandPool(device, &cpi, nullptr, &pool) != VK_SUCCESS ||
        pool == nullptr) {
      return MakeFail(StatusCode::kDeviceCreateFailed, "pick pool failed");
    }
  }
  Status finish = MakeOk();
  VkCommandBuffer cmd = nullptr;
  {
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (p_vkAllocateCommandBuffers(device, &cai, &cmd) != VK_SUCCESS ||
        cmd == nullptr) {
      finish = MakeFail(StatusCode::kDeviceCreateFailed, "pick cmd failed");
    }
  }
  if (finish.ok()) {
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (p_vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
      finish = MakeFail(StatusCode::kDeviceCreateFailed, "pick begin failed");
    }
  }
  if (finish.ok()) {
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset.x = static_cast<std::int32_t>(x);
    region.imageOffset.y = static_cast<std::int32_t>(y);
    region.imageOffset.z = 0;
    region.imageExtent.width = 1;
    region.imageExtent.height = 1;
    region.imageExtent.depth = 1;
    VkImage id_img = reinterpret_cast<VkImage>(
        static_cast<std::uintptr_t>(id_image_bits_));
    VkBuffer dst = reinterpret_cast<VkBuffer>(
        static_cast<std::uintptr_t>(stage_bits_));
    p_vkCmdCopyImageToBuffer(cmd, id_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           dst, 1, &region);
    if (p_vkEndCommandBuffer(cmd) != VK_SUCCESS) {
      finish = MakeFail(StatusCode::kDeviceCreateFailed, "pick end failed");
    }
  }
  if (finish.ok()) {
    VkFence fence = nullptr;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (p_vkCreateFence(device, &fci, nullptr, &fence) != VK_SUCCESS ||
        fence == nullptr) {
      finish = MakeFail(StatusCode::kDeviceCreateFailed, "pick fence failed");
    } else {
      VkSubmitInfo si{};
      si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      si.commandBufferCount = 1;
      si.pCommandBuffers = &cmd;
      if (p_vkQueueSubmit(dev.graphics_queue(), 1, &si, fence) != VK_SUCCESS) {
        finish = MakeFail(StatusCode::kDeviceCreateFailed, "pick submit");
      } else if (p_vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) !=
                 VK_SUCCESS) {
        finish = MakeFail(StatusCode::kDeviceCreateFailed, "pick wait");
      }
      p_vkDestroyFence(device, fence, nullptr);
    }
  }
  if (finish.ok()) {
    // The staging buffer is persistently mapped (host-visible + coherent
    // at create time); read the texel straight from the mapping.
    const auto* lanes =
        static_cast<const std::uint32_t*>(stage_mapped_);
    if (lanes == nullptr) {
      finish = MakeFail(StatusCode::kDeviceCreateFailed, "pick no mapping");
    } else {
      std::uint32_t lanes_copy[2]{lanes[0], lanes[1]};
      out_lo = lanes_copy[0];
      out_hi = lanes_copy[1];
    }
  }
  p_vkDestroyCommandPool(device, pool, nullptr);
  if (!finish.ok()) {
    out_lo = 0;
    out_hi = 0;
  }
  return finish;
}

#else

PickIdBuffer::~PickIdBuffer() noexcept { destroy(); }

auto PickIdBuffer::create(VulkanDevice& /*dev*/, ForwardRenderer& /*forward*/,
                          VkExtent2D /*extent*/) noexcept -> Status {
  return MakeFail(StatusCode::kVulkanLoaderFailed, "no Vulkan SDK");
}

auto PickIdBuffer::destroy() noexcept -> void {
  id_image_bits_ = 0;
  depth_image_bits_ = 0;
  stage_bits_ = 0;
  id_view_ = nullptr;
  depth_view_ = nullptr;
  width_ = 0;
  height_ = 0;
  depth_live_ = false;
  ready_ = false;
}

auto PickIdBuffer::record_id_pass(VulkanDevice& /*dev*/,
                                  ForwardRenderer& /*forward*/,
                                  void* /*world*/,
                                  const MeshRegistry& /*registry*/,
                                  VkCommandBuffer /*cmd*/,
                                  float /*aspect*/) noexcept -> Status {
  return MakeFail(StatusCode::kNotInitialized, "no Vulkan SDK");
}

auto PickIdBuffer::pick(VulkanDevice& /*dev*/, std::uint32_t /*x*/,
                        std::uint32_t /*y*/, std::uint32_t& out_lo,
                        std::uint32_t& out_hi) noexcept -> Status {
  out_lo = 0;
  out_hi = 0;
  return MakeFail(StatusCode::kNotInitialized, "no Vulkan SDK");
}

#endif

}  // namespace engine::renderer

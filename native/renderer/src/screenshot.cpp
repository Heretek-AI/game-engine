// screenshot.cpp — P1-04: capture_rgba8 + write_png.
//
// capture_rgba8 records a one-shot TRANSFER_SRC copy into a host-visible
// staging buffer and swizzles BGRA->RGBA on the CPU readback. write_png
// delegates PNG encoding to stb_image_write (the single implementation
// TU lives in native/renderer/src/stb_image_write_impl.c).
#if defined(ENGINE_RENDERER_HAS_VULKAN)
#include <volk.h>
#endif
#include "engine/renderer/swapchain.hpp"

#if defined(ENGINE_RENDERER_HAS_STB_IMAGE_WRITE)
#include "stb_image_write.h"
#endif

#include <algorithm>
#include <cstdint>
#include <vector>

namespace engine::renderer {

auto capture_rgba8(VulkanDevice& dev, VkImage image, VkFormat fmt, VkExtent2D ext,
                   VkImageLayout current_layout, std::vector<std::uint8_t>& out) noexcept
    -> Status {
#if !defined(ENGINE_RENDERER_HAS_VULKAN)
  (void)dev;
  (void)image;
  (void)fmt;
  (void)ext;
  (void)current_layout;
  (void)out;
  return MakeFail(StatusCode::kVulkanLoaderFailed, "Vulkan SDK unavailable");
#else
  if (!dev.valid() || image == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "device or image null");
  }
  if (ext.width == 0 || ext.height == 0) {
    return MakeFail(StatusCode::kInvalidArgument, "capture extent must be non-zero");
  }
  bool supported = (fmt == VK_FORMAT_R8G8B8A8_UNORM) || (fmt == VK_FORMAT_B8G8R8A8_UNORM);
  if (!supported) {
    return MakeFail(StatusCode::kUnsupported, "capture format must be R8G8B8A8 or B8G8R8A8");
  }
  std::uint64_t pixels = static_cast<std::uint64_t>(ext.width) * ext.height;
  if (pixels > 16384u * 16384u) {
    return MakeFail(StatusCode::kInvalidArgument, "capture extent too large");
  }
  std::uint64_t bytes_needed = pixels * 4u;
  if (bytes_needed > static_cast<std::uint64_t>(SIZE_MAX)) {
    return MakeFail(StatusCode::kOutOfMemory, "capture buffer size overflow");
  }

  VkDevice device = dev.device();
  VkPhysicalDevice physical = dev.physical_device();
  VkQueue queue = dev.graphics_queue();

  // Host-visible staging buffer (TRANSFER_DST).
  VkBuffer buf = nullptr;
  VkBufferCreateInfo bci{};
  bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bci.size = static_cast<VkDeviceSize>(bytes_needed);
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(device, &bci, nullptr, &buf) != VK_SUCCESS || buf == nullptr) {
    return MakeFail(StatusCode::kDeviceCreateFailed, "capture staging buffer failed");
  }
  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(device, buf, &req);
  VkPhysicalDeviceMemoryProperties props{};
  vkGetPhysicalDeviceMemoryProperties(physical, &props);
  std::uint32_t mem_type = 0xFFFFFFFFu;
  for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    bool in_mask = (req.memoryTypeBits & (1u << i)) != 0u;
    VkMemoryPropertyFlags flags = props.memoryTypes[i].propertyFlags;
    bool host_visible = (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    bool host_coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    if (in_mask && host_visible && host_coherent) {
      mem_type = i;
      break;
    }
  }
  if (mem_type == 0xFFFFFFFFu) {
    vkDestroyBuffer(device, buf, nullptr);
    return MakeFail(StatusCode::kOutOfMemory, "no host-visible memory for capture");
  }
  VkDeviceMemory mem = nullptr;
  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = mem_type;
  if (vkAllocateMemory(device, &mai, nullptr, &mem) != VK_SUCCESS || mem == nullptr) {
    vkDestroyBuffer(device, buf, nullptr);
    return MakeFail(StatusCode::kOutOfMemory, "capture staging alloc failed");
  }
  if (vkBindBufferMemory(device, buf, mem, 0) != VK_SUCCESS) {
    vkFreeMemory(device, mem, nullptr);
    vkDestroyBuffer(device, buf, nullptr);
    return MakeFail(StatusCode::kDeviceCreateFailed, "capture staging bind failed");
  }

  VkCommandPool pool = nullptr;
  VkCommandPoolCreateInfo cpi{};
  cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  cpi.queueFamilyIndex = dev.graphics_queue_family();
  if (vkCreateCommandPool(device, &cpi, nullptr, &pool) != VK_SUCCESS || pool == nullptr) {
    vkFreeMemory(device, mem, nullptr);
    vkDestroyBuffer(device, buf, nullptr);
    return MakeFail(StatusCode::kDeviceCreateFailed, "capture command pool failed");
  }
  VkCommandBuffer cmd = nullptr;
  VkCommandBufferAllocateInfo cai{};
  cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cai.commandPool = pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(device, &cai, &cmd) != VK_SUCCESS || cmd == nullptr) {
    vkDestroyCommandPool(device, pool, nullptr);
    vkFreeMemory(device, mem, nullptr);
    vkDestroyBuffer(device, buf, nullptr);
    return MakeFail(StatusCode::kDeviceCreateFailed, "capture command buffer failed");
  }
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
    vkDestroyCommandPool(device, pool, nullptr);
    vkFreeMemory(device, mem, nullptr);
    vkDestroyBuffer(device, buf, nullptr);
    return MakeFail(StatusCode::kDeviceCreateFailed, "capture command begin failed");
  }

  auto barrier = [&](VkImageLayout from, VkImageLayout to, VkAccessFlags2 src_access,
                     VkAccessFlags2 dst_access, VkPipelineStageFlags2 src_stage,
                     VkPipelineStageFlags2 dst_stage) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = 1;
    VkDependencyInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
  };
  barrier(current_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
          VK_ACCESS_2_MEMORY_WRITE_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
          VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_2_COPY_BIT);

  VkBufferImageCopy region{};
  region.bufferOffset = 0;
  region.bufferRowLength = 0;
  region.bufferImageHeight = 0;
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.mipLevel = 0;
  region.imageSubresource.baseArrayLayer = 0;
  region.imageSubresource.layerCount = 1;
  region.imageOffset.x = 0;
  region.imageOffset.y = 0;
  region.imageOffset.z = 0;
  region.imageExtent.width = ext.width;
  region.imageExtent.height = ext.height;
  region.imageExtent.depth = 1;
  vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);

  barrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, current_layout,
          VK_ACCESS_2_TRANSFER_READ_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
          VK_PIPELINE_STAGE_2_COPY_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);

  Status finish = MakeOk();
  if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
    finish = MakeFail(StatusCode::kDeviceCreateFailed, "capture command end failed");
  }
  if (finish.ok()) {
    VkFence fence = nullptr;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(device, &fci, nullptr, &fence) != VK_SUCCESS || fence == nullptr) {
      finish = MakeFail(StatusCode::kDeviceCreateFailed, "capture fence failed");
    } else {
      VkSubmitInfo si{};
      si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      si.commandBufferCount = 1;
      si.pCommandBuffers = &cmd;
      if (vkQueueSubmit(queue, 1, &si, fence) != VK_SUCCESS) {
        finish = MakeFail(StatusCode::kDeviceCreateFailed, "capture submit failed");
      } else if (vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        finish = MakeFail(StatusCode::kDeviceCreateFailed, "capture fence wait failed");
      }
      vkDestroyFence(device, fence, nullptr);
    }
  }

  bool swizzle_bgra = (fmt == VK_FORMAT_B8G8R8A8_UNORM);
  if (finish.ok()) {
    void* mapped = nullptr;
    if (vkMapMemory(device, mem, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS ||
        mapped == nullptr) {
      finish = MakeFail(StatusCode::kDeviceCreateFailed, "capture map failed");
    } else {
      const auto* src = static_cast<const std::uint8_t*>(mapped);
      std::size_t n = static_cast<std::size_t>(bytes_needed);
      std::vector<std::uint8_t> tmp;
      tmp.resize(n);
      for (std::size_t i = 0; i < n; ++i) {
        tmp[i] = src[i];
      }
      out = tmp;
      if (finish.ok() && swizzle_bgra) {
        for (std::size_t i = 0; i + 3 < out.size() + 1 && i + 3 < n; i += 4) {
          std::swap(out[i], out[i + 2]);
        }
      }
      vkUnmapMemory(device, mem);
    }
  }

  vkDestroyCommandPool(device, pool, nullptr);
  vkFreeMemory(device, mem, nullptr);
  vkDestroyBuffer(device, buf, nullptr);
  return finish;
#endif
}

auto write_png(const char* path, std::span<const std::uint8_t> rgba, std::uint32_t w,
               std::uint32_t h) noexcept -> Status {
#if !defined(ENGINE_RENDERER_HAS_VULKAN) || !defined(ENGINE_RENDERER_HAS_STB_IMAGE_WRITE)
  (void)path;
  (void)rgba;
  (void)w;
  (void)h;
  return MakeFail(StatusCode::kVulkanLoaderFailed, "PNG writer unavailable");
#else
  if (path == nullptr || rgba.data() == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "png path or pixels null");
  }
  if (w == 0 || h == 0) {
    return MakeFail(StatusCode::kInvalidArgument, "png extent must be non-zero");
  }
  std::uint64_t need = static_cast<std::uint64_t>(w) * h * 4u;
  constexpr std::uint64_t kMaxPng = 16384u * 16384u * 4u;
  if (need > kMaxPng || rgba.size() < static_cast<std::size_t>(need)) {
    return MakeFail(StatusCode::kInvalidArgument, "png buffer too small");
  }
  int wi = static_cast<int>(w);
  int hi = static_cast<int>(h);
  int stride = static_cast<int>(static_cast<std::uint64_t>(w) * 4u);
  int ok = stbi_write_png(path, wi, hi, 4, rgba.data(), stride);
  if (ok == 0) {
    return MakeFail(StatusCode::kDeviceCreateFailed, "stbi_write_png failed");
  }
  return MakeOk();
#endif
}

}  // namespace engine::renderer

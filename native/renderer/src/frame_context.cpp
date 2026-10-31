// frame_context.cpp — P1-04: 2-frames-in-flight acquire/record/submit.
#if defined(ENGINE_RENDERER_HAS_VULKAN)
#include <volk.h>
#endif
#include "engine/renderer/swapchain.hpp"

namespace engine::renderer {

FrameContext::~FrameContext() noexcept { destroy(); }

auto FrameContext::create(VulkanDevice& dev, Swapchain& sc) noexcept -> Status {
#if !defined(ENGINE_RENDERER_HAS_VULKAN)
  (void)dev;
  (void)sc;
  return MakeFail(StatusCode::kVulkanLoaderFailed, "Vulkan SDK unavailable");
#else
  if (!dev.valid() || sc.handle() == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "device or swapchain not ready");
  }
  if (valid_) {
    return MakeFail(StatusCode::kAlreadyInitialized, "frame context already created");
  }
  VkDevice device = dev.device();
  VkCommandPoolCreateInfo cpi{};
  cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpi.queueFamilyIndex = dev.graphics_queue_family();
  if (vkCreateCommandPool(device, &cpi, nullptr, &pool_) != VK_SUCCESS) {
    return MakeFail(StatusCode::kDeviceCreateFailed, "frame command pool failed");
  }
  VkCommandBufferAllocateInfo cai{};
  cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cai.commandPool = pool_;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = kFramesInFlight;
  if (vkAllocateCommandBuffers(device, &cai, cmds_.data()) != VK_SUCCESS) {
    vkDestroyCommandPool(device, pool_, nullptr);
    pool_ = nullptr;
    return MakeFail(StatusCode::kDeviceCreateFailed, "frame command buffers failed");
  }
  for (std::uint32_t i = 0; i < kFramesInFlight; ++i) {
    VkSemaphoreCreateInfo semi{};
    semi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (vkCreateSemaphore(device, &semi, nullptr, &acquire_[i]) != VK_SUCCESS ||
        vkCreateSemaphore(device, &semi, nullptr, &submit_[i]) != VK_SUCCESS) {
      destroy();
      return MakeFail(StatusCode::kDeviceCreateFailed, "frame semaphores failed");
    }
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;  // first frame proceeds
    if (vkCreateFence(device, &fci, nullptr, &fences_[i]) != VK_SUCCESS) {
      destroy();
      return MakeFail(StatusCode::kDeviceCreateFailed, "frame fences failed");
    }
  }
  dev_ = &dev;
  sc_ = &sc;
  width_ = sc.extent().width;
  height_ = sc.extent().height;
  frame_ = 0;
  image_valid_ = false;
  valid_ = true;
  return MakeOk();
#endif
}

auto FrameContext::destroy() noexcept -> void {
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  if (dev_ != nullptr && dev_->device() != nullptr) {
    vkDeviceWaitIdle(dev_->device());
    for (std::uint32_t i = 0; i < kFramesInFlight; ++i) {
      if (acquire_[i] != nullptr) {
        vkDestroySemaphore(dev_->device(), acquire_[i], nullptr);
        acquire_[i] = nullptr;
      }
      if (submit_[i] != nullptr) {
        vkDestroySemaphore(dev_->device(), submit_[i], nullptr);
        submit_[i] = nullptr;
      }
      if (fences_[i] != nullptr) {
        vkDestroyFence(dev_->device(), fences_[i], nullptr);
        fences_[i] = nullptr;
      }
    }
    if (pool_ != nullptr) {
      vkDestroyCommandPool(dev_->device(), pool_, nullptr);
      pool_ = nullptr;
    }
  }
  cmds_.fill(nullptr);
  dev_ = nullptr;
  sc_ = nullptr;
  frame_ = 0;
  image_valid_ = false;
  valid_ = false;
  pending_recreate_ = false;
#else
  dev_ = nullptr;
  sc_ = nullptr;
  valid_ = false;
#endif
}

auto FrameContext::begin_frame() noexcept -> FrameBegin {
  FrameBegin out{};
#if !defined(ENGINE_RENDERER_HAS_VULKAN)
  return out;
#else
  if (!valid_ || dev_ == nullptr || sc_ == nullptr) {
    return out;
  }
  if (pending_recreate_) {
    pending_recreate_ = false;
    (void)sc_->recreate(width_, height_);
    image_valid_ = false;
  }
  std::uint32_t slot = frame_ % kFramesInFlight;
  VkDevice device = dev_->device();
  if (vkWaitForFences(device, 1, &fences_[slot], VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
    return out;
  }
  if (vkResetFences(device, 1, &fences_[slot]) != VK_SUCCESS) {
    return out;
  }
  std::uint32_t idx = 0;
  VkResult acq = vkAcquireNextImageKHR(device, sc_->handle(), UINT64_MAX, acquire_[slot],
                                      nullptr, &idx);
  if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
    (void)sc_->recreate(width_, height_);
    image_valid_ = false;
    return out;
  }
  if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
    return out;
  }
  if (vkResetCommandBuffer(cmds_[slot], 0) != VK_SUCCESS) {
    return out;
  }
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cmds_[slot], &bi) != VK_SUCCESS) {
    return out;
  }
  image_index_ = idx;
  image_valid_ = true;
  out.ok = true;
  out.image_index = idx;
  out.cmd = cmds_[slot];
  return out;
#endif
}

auto FrameContext::end_frame(VkImageLayout current_layout) noexcept -> Status {
#if !defined(ENGINE_RENDERER_HAS_VULKAN)
  (void)current_layout;
  return MakeFail(StatusCode::kVulkanLoaderFailed, "Vulkan SDK unavailable");
#else
  if (!valid_ || dev_ == nullptr || sc_ == nullptr || !image_valid_) {
    return MakeFail(StatusCode::kNotInitialized, "no frame in flight");
  }
  std::uint32_t slot = frame_ % kFramesInFlight;
  VkCommandBuffer cb = cmds_[slot];
  // Transition the recorded image to PRESENT_SRC from whatever layout the
  // caller left it in (test path: TRANSFER_SRC after capture_rgba8).
  VkImageMemoryBarrier2 barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
  barrier.dstAccessMask = 0;
  barrier.oldLayout = current_layout;
  barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = sc_->image(image_index_);
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = 1;
  VkDependencyInfo dep{};
  dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cb, &dep);
  if (vkEndCommandBuffer(cb) != VK_SUCCESS) {
    image_valid_ = false;
    frame_ += 1;
    return MakeFail(StatusCode::kDeviceCreateFailed, "frame command end failed");
  }
  VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.waitSemaphoreCount = 1;
  si.pWaitSemaphores = &acquire_[slot];
  si.pWaitDstStageMask = &wait_stage;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cb;
  si.signalSemaphoreCount = 1;
  si.pSignalSemaphores = &submit_[slot];
  if (vkQueueSubmit(dev_->graphics_queue(), 1, &si, fences_[slot]) != VK_SUCCESS) {
    image_valid_ = false;
    frame_ += 1;
    return MakeFail(StatusCode::kDeviceCreateFailed, "frame queue submit failed");
  }
  VkPresentInfoKHR pi{};
  pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  pi.waitSemaphoreCount = 1;
  pi.pWaitSemaphores = &submit_[slot];
  pi.swapchainCount = 1;
  VkSwapchainKHR handle = sc_->handle();
  pi.pSwapchains = &handle;
  pi.pImageIndices = &image_index_;
  VkResult present = vkQueuePresentKHR(dev_->graphics_queue(), &pi);
  image_valid_ = false;
  frame_ += 1;
  if (present == VK_ERROR_OUT_OF_DATE_KHR || present == VK_SUBOPTIMAL_KHR) {
    pending_recreate_ = true;
    return MakeOk();
  }
  if (present != VK_SUCCESS) {
    return MakeFail(StatusCode::kDeviceCreateFailed, "frame present failed");
  }
  return MakeOk();
#endif
}

}  // namespace engine::renderer

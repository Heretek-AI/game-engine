// swapchain.cpp — P1-04: Swapchain implementation.
//
// Headless-surface rules (parity spec §4.3): 0xFFFFFFFF currentExtent ->
// window size; image count max(minImageCount, 2); FIFO always; usage
// COLOR_ATTACHMENT | TRANSFER_SRC. No sRGB swapchain format on lavapipe,
// so prefer B8G8R8A8_UNORM then R8G8B8A8_UNORM and let the final shader
// encode sRGB (P1-06+). The image index is 0 on the headless surface —
// never assumed, only handled through acquire.
#if defined(ENGINE_RENDERER_HAS_VULKAN)
#include <volk.h>
#endif
#include "engine/renderer/swapchain.hpp"

#include <algorithm>
#include <string_view>

namespace engine::renderer {

namespace {

#if defined(ENGINE_RENDERER_HAS_VULKAN)
// Bounded compare: true when `name` NUL-terminates inside `limit` bytes
// and equals `want`. Rejects unterminated input without over-reading
// (Sonar S5813: no strlen on caller-owned pointers).
[[maybe_unused]] auto NameEquals(const char* name, std::size_t limit, std::string_view want) noexcept -> bool {
  if (name == nullptr) {
    return false;
  }
  std::size_t len = 0;
  while (len < limit && len < want.size() + 1) {
    if (name[len] == '\0') {
      break;
    }
    ++len;
  }
  if (len > want.size()) {
    return false;
  }
  if (name[len] != '\0') {
    // Either longer than want, or unterminated within the window probed:
    // both are "not equal" without reading further.
    return false;
  }
  for (std::size_t i = 0; i < len; ++i) {
    if (name[i] != want[i]) {
      return false;
    }
  }
  return len == want.size();
}
#endif

}  // namespace

Swapchain::~Swapchain() noexcept { destroy(); }

auto Swapchain::create(VulkanDevice& dev, VkSurfaceKHR surface,
                       const SwapchainDesc& d) noexcept -> Status {
#if !defined(ENGINE_RENDERER_HAS_VULKAN)
  (void)dev;
  (void)surface;
  (void)d;
  return MakeFail(StatusCode::kVulkanLoaderFailed, "Vulkan SDK unavailable");
#else
  if (!dev.valid() || surface == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "device or surface null");
  }
  if (swapchain_ != nullptr) {
    return MakeFail(StatusCode::kAlreadyInitialized, "swapchain already created");
  }
  if (d.width == 0 || d.height == 0) {
    return MakeFail(StatusCode::kInvalidArgument, "swapchain extent must be non-zero");
  }
  dev_ = &dev;
  surface_ = surface;
  vsync_ = d.vsync;
  width_ = d.width;
  height_ = d.height;
  return build(nullptr);
#endif
}

auto Swapchain::recreate(std::uint32_t width, std::uint32_t height) noexcept -> Status {
#if !defined(ENGINE_RENDERER_HAS_VULKAN)
  (void)width;
  (void)height;
  return MakeFail(StatusCode::kVulkanLoaderFailed, "Vulkan SDK unavailable");
#else
  if (swapchain_ == nullptr || dev_ == nullptr) {
    return MakeFail(StatusCode::kNotInitialized, "swapchain not created");
  }
  if (width == 0 || height == 0) {
    return MakeFail(StatusCode::kInvalidArgument, "swapchain extent must be non-zero");
  }
  vkDeviceWaitIdle(dev_->device());
  width_ = width;
  height_ = height;
  VkSwapchainKHR old = swapchain_;
  swapchain_ = nullptr;
  // P7-01 (Win32/Lavapipe): old image views must die before build() fills
  // views_[] with the new swapchain's views; otherwise the recreated
  // swapchain leaks stale views and view(i) aliases a destroyed image.
  destroy_views();
  image_count_ = 0;
  images_.fill(nullptr);
  auto st = build(old);
  vkDestroySwapchainKHR(dev_->device(), old, nullptr);
  return st;
#endif
}

auto Swapchain::destroy() noexcept -> void {
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  if (swapchain_ != nullptr && dev_ != nullptr) {
    vkDeviceWaitIdle(dev_->device());
    destroy_views();
    vkDestroySwapchainKHR(dev_->device(), swapchain_, nullptr);
  }
  swapchain_ = nullptr;
  images_.fill(nullptr);
  views_.fill(nullptr);
  image_count_ = 0;
  dev_ = nullptr;
  surface_ = nullptr;
  format_ = VK_FORMAT_UNDEFINED;
  extent_ = VkExtent2D{};
  width_ = 0;
  height_ = 0;
#else
  images_.fill(nullptr);
  views_.fill(nullptr);
  image_count_ = 0;
  dev_ = nullptr;
  surface_ = nullptr;
#endif
}

auto Swapchain::format() const noexcept -> VkFormat {
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  return format_;
#else
  return format_;
#endif
}

auto Swapchain::extent() const noexcept -> VkExtent2D { return extent_; }

auto Swapchain::image_count() const noexcept -> std::uint32_t { return image_count_; }

auto Swapchain::image(std::uint32_t i) const noexcept -> VkImage {
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  if (i >= image_count_) {
    return nullptr;
  }
  return images_[i];
#else
  (void)i;
  return nullptr;
#endif
}

auto Swapchain::view(std::uint32_t i) const noexcept -> VkImageView {
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  if (i >= image_count_) {
    return nullptr;
  }
  return views_[i];
#else
  (void)i;
  return nullptr;
#endif
}

auto Swapchain::handle() const noexcept -> VkSwapchainKHR { return swapchain_; }

#if defined(ENGINE_RENDERER_HAS_VULKAN)
auto Swapchain::destroy_views() noexcept -> void {
  if (dev_ == nullptr) {
    return;
  }
  for (std::uint32_t i = 0; i < image_count_; ++i) {
    if (views_[i] != nullptr) {
      vkDestroyImageView(dev_->device(), views_[i], nullptr);
      views_[i] = nullptr;
    }
  }
}

auto Swapchain::build(VkSwapchainKHR old) noexcept -> Status {
  VkDevice device = dev_->device();
  VkPhysicalDevice physical = dev_->physical_device();

  VkSurfaceCapabilitiesKHR caps{};
  if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface_, &caps) != VK_SUCCESS) {
    return MakeFail(StatusCode::kSurfaceCreateFailed, "surface capabilities query failed");
  }
  // Headless: currentExtent is 0xFFFFFFFF — use the window size instead.
  VkExtent2D extent{};
  if (caps.currentExtent.width == 0xFFFFFFFFu) {
    extent.width = width_;
    extent.height = height_;
  } else {
    extent.width = std::max(caps.currentExtent.width, 1u);
    extent.height = std::max(caps.currentExtent.height, 1u);
  }
  // Clamp to the surface limits when they are finite.
  if (caps.maxImageExtent.width != 0xFFFFFFFFu) {
    extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
  }

  std::uint32_t fmt_count = 0;
  if (vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface_, &fmt_count, nullptr) !=
          VK_SUCCESS ||
      fmt_count == 0) {
    return MakeFail(StatusCode::kSurfaceCreateFailed, "surface formats query failed");
  }
  constexpr std::uint32_t kMaxFormats = 64;
  std::array<VkSurfaceFormatKHR, kMaxFormats> formats{};
  std::uint32_t fetch = (fmt_count < kMaxFormats) ? fmt_count : kMaxFormats;
  if (vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface_, &fetch, formats.data()) !=
      VK_SUCCESS) {
    return MakeFail(StatusCode::kSurfaceCreateFailed, "surface formats fetch failed");
  }
  // No sRGB format on the headless surface: prefer B8G8R8A8 then R8G8B8A8.
  VkSurfaceFormatKHR picked{};
  bool have_pick = false;
  for (std::uint32_t i = 0; i < fetch; ++i) {
    if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
      picked = formats[i];
      have_pick = true;
      break;
    }
  }
  if (!have_pick) {
    for (std::uint32_t i = 0; i < fetch; ++i) {
      if (formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
        picked = formats[i];
        have_pick = true;
        break;
      }
    }
  }
  if (!have_pick) {
    picked = formats[0];
  }

  std::uint32_t mode_count = 0;
  if (vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface_, &mode_count, nullptr) !=
          VK_SUCCESS ||
      mode_count == 0) {
    return MakeFail(StatusCode::kSurfaceCreateFailed, "present modes query failed");
  }
  constexpr std::uint32_t kMaxModes = 8;
  std::array<VkPresentModeKHR, kMaxModes> modes{};
  std::uint32_t fetch_modes = (mode_count < kMaxModes) ? mode_count : kMaxModes;
  if (vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface_, &fetch_modes,
                                                modes.data()) != VK_SUCCESS) {
    return MakeFail(StatusCode::kSurfaceCreateFailed, "present modes fetch failed");
  }
  VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;  // always available
  if (!vsync_) {
    for (std::uint32_t i = 0; i < fetch_modes; ++i) {
      if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
        mode = VK_PRESENT_MODE_MAILBOX_KHR;
        break;
      }
      if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) {
        mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
      }
    }
  }

  if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0) {
    return MakeFail(StatusCode::kSurfaceCreateFailed, "surface lacks COLOR_ATTACHMENT usage");
  }
  VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  if ((caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0) {
    usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  }

  std::uint32_t want_count = caps.minImageCount;
  if (want_count < 2) {
    want_count = 2;
  }
  if (caps.maxImageCount != 0 && want_count > caps.maxImageCount) {
    want_count = caps.maxImageCount;
  }

  VkSwapchainCreateInfoKHR sci{};
  sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  sci.surface = surface_;
  sci.minImageCount = want_count;
  sci.imageFormat = picked.format;
  sci.imageColorSpace = picked.colorSpace;
  sci.imageExtent = extent;
  sci.imageArrayLayers = 1;
  sci.imageUsage = usage;
  sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  sci.queueFamilyIndexCount = 0;
  sci.pQueueFamilyIndices = nullptr;
  sci.preTransform = caps.currentTransform;
  sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  sci.presentMode = mode;
  sci.clipped = VK_TRUE;
  sci.oldSwapchain = old;

  VkSwapchainKHR sc = nullptr;
  if (vkCreateSwapchainKHR(device, &sci, nullptr, &sc) != VK_SUCCESS || sc == nullptr) {
    return MakeFail(StatusCode::kSurfaceCreateFailed, "vkCreateSwapchainKHR failed");
  }

  std::uint32_t got = 0;
  if (vkGetSwapchainImagesKHR(device, sc, &got, nullptr) != VK_SUCCESS || got == 0) {
    vkDestroySwapchainKHR(device, sc, nullptr);
    return MakeFail(StatusCode::kSurfaceCreateFailed, "swapchain images query failed");
  }
  if (got > kMaxSwapImages) {
    vkDestroySwapchainKHR(device, sc, nullptr);
    return MakeFail(StatusCode::kOutOfMemory, "swapchain image count exceeds static slots");
  }
  std::array<VkImage, kMaxSwapImages> imgs{};
  if (vkGetSwapchainImagesKHR(device, sc, &got, imgs.data()) != VK_SUCCESS) {
    vkDestroySwapchainKHR(device, sc, nullptr);
    return MakeFail(StatusCode::kSurfaceCreateFailed, "swapchain images fetch failed");
  }

  // Views are created once per image, not per frame.
  std::array<VkImageView, kMaxSwapImages> views{};
  for (std::uint32_t i = 0; i < got; ++i) {
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = imgs[i];
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = picked.format;
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.baseMipLevel = 0;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.baseArrayLayer = 0;
    vci.subresourceRange.layerCount = 1;
    if (vkCreateImageView(device, &vci, nullptr, &views[i]) != VK_SUCCESS) {
      for (std::uint32_t j = 0; j < i; ++j) {
        vkDestroyImageView(device, views[j], nullptr);
      }
      vkDestroySwapchainKHR(device, sc, nullptr);
      return MakeFail(StatusCode::kSurfaceCreateFailed, "swapchain image view failed");
    }
  }

  swapchain_ = sc;
  format_ = picked.format;
  extent_ = extent;
  images_ = imgs;
  views_ = views;
  image_count_ = got;
  return MakeOk();
}
#endif

}  // namespace engine::renderer

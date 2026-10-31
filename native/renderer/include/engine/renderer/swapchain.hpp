// swapchain.hpp — P1-04: swapchain + frame loop + screenshot.
//
// Swapchain wraps VkSwapchainKHR with the headless-surface rules from the
// parity spec §4.3 (0xFFFFFFFF extent fallback, max(minImageCount, 2),
// FIFO, COLOR_ATTACHMENT | TRANSFER_SRC). FrameContext owns the 2-frame
// sync set (command pool + buffers, acquire/submit semaphores, fences).
// screenshot.* (companion TU) implements capture_rgba8 + write_png.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "engine/core/engine_core_export.hpp"
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
#include "engine/renderer/engine_status.hpp"
#include "engine/renderer/vulkan_device.hpp"

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#else
// Opaque stand-ins so this header compiles without the Vulkan SDK.
struct SwapExtentStub {
  std::uint32_t width{0};
  std::uint32_t height{0};
};
using VkCommandBuffer = void*;
using VkExtent2D = SwapExtentStub;
using VkFormat = std::uint32_t;
using VkImage = void*;
using VkImageLayout = std::uint32_t;
using VkImageView = void*;
using VkSurfaceKHR = void*;
using VkSwapchainKHR = void*;
#endif

namespace engine::renderer {

struct SwapchainDesc {
  std::uint32_t width{1280};
  std::uint32_t height{720};
  bool vsync{true};
};

class ENGINE_CORE_API Swapchain {
 public:
  Swapchain() noexcept = default;
  Swapchain(const Swapchain&) = delete;
  auto operator=(const Swapchain&) -> Swapchain& = delete;
  ~Swapchain() noexcept;

  // Create the swapchain for `surface` (requires VulkanDevice init with
  // enable_swapchain=true). FIFO always; window size when the headless
  // surface reports 0xFFFFFFFF extent; image count max(minImageCount, 2).
  auto create(VulkanDevice& dev, VkSurfaceKHR surface,
              const SwapchainDesc& d) noexcept -> Status;
  // Wait idle and rebuild for a new window size.
  auto recreate(std::uint32_t width, std::uint32_t height) noexcept -> Status;
  auto destroy() noexcept -> void;

  [[nodiscard]] auto format() const noexcept -> VkFormat;
  [[nodiscard]] auto extent() const noexcept -> VkExtent2D;
  [[nodiscard]] auto image_count() const noexcept -> std::uint32_t;
  [[nodiscard]] auto image(std::uint32_t i) const noexcept -> VkImage;
  [[nodiscard]] auto view(std::uint32_t i) const noexcept -> VkImageView;
  [[nodiscard]] auto handle() const noexcept -> VkSwapchainKHR;

 private:
  auto build(VkSwapchainKHR old) noexcept -> Status;
  auto destroy_views() noexcept -> void;

  VulkanDevice* dev_{nullptr};
  VkSurfaceKHR surface_{nullptr};
  bool vsync_{true};
  std::uint32_t width_{0};
  std::uint32_t height_{0};
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  static constexpr std::uint32_t kMaxSwapImages = 8;
  VkSwapchainKHR swapchain_{nullptr};
  VkFormat format_{VK_FORMAT_UNDEFINED};
  VkExtent2D extent_{};
  std::array<VkImage, kMaxSwapImages> images_{};
  std::array<VkImageView, kMaxSwapImages> views_{};
  std::uint32_t image_count_{0};
#else
  VkSwapchainKHR swapchain_{nullptr};
  VkFormat format_{0};
  VkExtent2D extent_{};
  std::array<VkImage, 1> images_{};
  std::array<VkImageView, 1> views_{};
  std::uint32_t image_count_{0};
#endif
};

struct FrameBegin {
  bool ok{false};
  std::uint32_t image_index{0};
  VkCommandBuffer cmd{nullptr};
};

class ENGINE_CORE_API FrameContext {
 public:
  static constexpr std::uint32_t kFramesInFlight = 2;

  FrameContext() noexcept = default;
  FrameContext(const FrameContext&) = delete;
  auto operator=(const FrameContext&) -> FrameContext& = delete;
  ~FrameContext() noexcept;

  auto create(VulkanDevice& dev, Swapchain& sc) noexcept -> Status;
  auto destroy() noexcept -> void;

  // Wait fence, acquire next image, reset + begin the frame command buffer.
  // On OUT_OF_DATE: recreates the swapchain and reports ok=false (caller
  // retries next frame — never fails the frame loop on resize).
  auto begin_frame() noexcept -> FrameBegin;
  // Barrier to PRESENT_SRC (from current_layout), submit, present.
  // On OUT_OF_DATE/SUBOPTIMAL from present: recreates and reports Ok.
  auto end_frame(VkImageLayout current_layout) noexcept -> Status;

 private:
  VulkanDevice* dev_{nullptr};
  Swapchain* sc_{nullptr};
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  VkCommandPool pool_{nullptr};
  std::array<VkCommandBuffer, kFramesInFlight> cmds_{};
  std::array<VkSemaphore, kFramesInFlight> acquire_{};
  std::array<VkSemaphore, kFramesInFlight> submit_{};
  std::array<VkFence, kFramesInFlight> fences_{};
  std::uint32_t frame_{0};
  std::uint32_t image_index_{0};
  bool image_valid_{false};
  bool valid_{false};
  bool pending_recreate_{false};
  std::uint32_t width_{0};
  std::uint32_t height_{0};
#else
  bool valid_{false};
#endif
};

// One-shot copy of `image` (expected in `current_layout`) to host RGBA8.
// Swizzles BGRA to RGBA so PNGs and goldens are always RGBA byte order.
ENGINE_CORE_API auto capture_rgba8(VulkanDevice& dev, VkImage image, VkFormat fmt,
                   VkExtent2D ext, VkImageLayout current_layout,
                   std::vector<std::uint8_t>& out) noexcept -> Status;
// Write an RGBA8 buffer as PNG (stb_image_write, single implementation TU).
ENGINE_CORE_API auto write_png(const char* path, std::span<const std::uint8_t> rgba,
               std::uint32_t w, std::uint32_t h) noexcept -> Status;

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif

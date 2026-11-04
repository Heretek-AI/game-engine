// viewport_share.hpp — P3-13: shareable Vulkan viewport image + timing.
//
// Long pole: the editor viewport currently pays a full CPU readback per
// frame (capture_rgba8: barrier to TRANSFER_SRC, copy to a host-visible
// staging buffer, map + swizzle). For zero-copy presentation the engine's
// viewport VkImage must instead be created with exportable external
// memory so its handle can be handed to Avalonia's compositor
// (ICompositionGpuInterop.ImportImage), which imports it without a copy.
//
// This header exposes that native half:
//   ViewportShare  — owns a VkImage at a fixed extent whose backing
//     memory was allocated with export info, and exports one OS handle
//     per share() call. Linux: opaque POSIX fd (the type Avalonia's
//     VulkanOpaquePosixFileDescriptor import consumes). Ownership passes
//     to the caller: each returned fd must be closed (or handed to the
//     compositor import) by the caller; the image stays alive across
//     share() calls. When the driver lacks external-memory support,
//     create() fails closed (kUnsupported) — the CPU readback path
//     (capture_rgba8) stays the default and is untouched.
//   ShareHandle    — the exported OS handle (fd on Linux, -1 invalid).
//
// Threading: single render thread only (same rule as the upload ring).
// No new device extensions are enabled on VulkanDevice: the share image
// is allocated with its own VkImage + VkDeviceMemory so the existing
// device init (M3 feature set) is untouched; create() probes the
// VK_KHR_external_memory(+_fd) entry points through
// VulkanDevice::proc()/instance_proc() and fails closed when absent.
// When ENGINE_RENDERER_HAS_VULKAN is undefined the class compiles to a
// stub whose create() reports kVulkanLoaderFailed.
#pragma once

#include <cstdint>

#include "engine/core/engine_core_export.hpp"
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
#include "engine/renderer/engine_status.hpp"
#include "engine/renderer/vk_extent_stub.hpp"
#include "engine/renderer/vulkan_device.hpp"

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#endif

namespace engine::renderer {

// Exported share handle. Linux: opaque POSIX fd from
// vkGetMemoryFdKHR (OPAQUE_FD, the handle type Avalonia imports as
// VulkanOpaquePosixFileDescriptor). -1 = invalid / share unsupported.
struct ViewportShareHandle {
  int fd{-1};
  [[nodiscard]] auto valid() const noexcept -> bool { return fd >= 0; }
};

// Shareable viewport image: fixed extent/format, exportable memory.
// Format is fixed to R8G8B8A8_UNORM (the readback path's canonical RGBA8;
// the swapchain may be BGRA, the share target never is).
class ENGINE_CORE_API ViewportShare {
 public:
  static constexpr std::uint32_t kShareFormatRgba8 = 37;  // VK_FORMAT_R8G8B8A8_UNORM

  ViewportShare() noexcept = default;
  ViewportShare(const ViewportShare&) = delete;
  auto operator=(const ViewportShare&) -> ViewportShare& = delete;
  ~ViewportShare() noexcept;

  // Create the exportable image at `width`x`height`. Requires an
  // initialized device; zero extent -> kInvalidArgument; device without
  // external-memory fd export -> kUnsupported (fail closed, CPU path
  // stays default). Not re-entrant: second call -> kAlreadyInitialized.
  auto create(VulkanDevice& dev, std::uint32_t width, std::uint32_t height) noexcept
      -> Status;
  auto destroy() noexcept -> void;

  [[nodiscard]] auto valid() const noexcept -> bool;
  [[nodiscard]] auto width() const noexcept -> std::uint32_t { return width_; }
  [[nodiscard]] auto height() const noexcept -> std::uint32_t { return height_; }

  // Export one OS handle for the current frame (caller owns it; each
  // call exports a fresh fd). Invalid share -> {fd:-1} + kUnsupported.
  // The image content is the caller's to render into before sharing.
  auto share() noexcept -> ViewportShareHandle;

#if defined(ENGINE_RENDERER_HAS_VULKAN)
  // Raw image for record-time use (valid after create()).
  [[nodiscard]] auto image() const noexcept -> VkImage { return image_; }
#endif

 private:
  VulkanDevice* dev_{nullptr};
  std::uint32_t width_{0};
  std::uint32_t height_{0};
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  VkImage image_{nullptr};
  VkDeviceMemory memory_{nullptr};
  std::uint32_t memory_type_{0xFFFFFFFFu};
  VkDeviceSize memory_size_{0};
  bool external_{false};
#else
  bool live_{false};
#endif
};

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif

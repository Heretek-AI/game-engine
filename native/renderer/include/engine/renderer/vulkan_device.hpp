// vulkan_device.hpp — M3-01: Vulkan 1.3 bootstrap (volk + VMA).
//
// VulkanDevice owns, in order:
//   1. volk loader init (volkInitialize + volkLoadInstance).
//   2. VkInstance (Vulkan 1.3, validation layers optional/off by default).
//   3. VkPhysicalDevice selection (discrete GPU preferred, Lavapipe OK).
//   4. VkDevice with the M3 feature set: dynamic rendering, sync2,
//      bufferDeviceAddress, descriptor indexing (for M3-02 bindless),
//      drawIndirectCount (for M3-02 vkCmdDrawIndexedIndirectCount).
//   5. VmaAllocator (VulkanMemoryAllocator) with a dedicated ReBAR-style
//      host-visible ring for per-frame transform uploads.
//
// Real Vulkan needs <vulkan/vulkan.h> (+ volk + VMA sources). When
// ENGINE_RENDERER_HAS_VULKAN is NOT defined (SDK absent, e.g. minimal CI
// images), this header still compiles: the device reports
// kVulkanLoaderFailed on init() and the CPU-side RenderGraph tests still
// run. Native handles are void* aliases in that mode.
//
// Threading: init/shutdown are NOT thread-safe (call once at startup /
// teardown). Ring-buffer upload (ring_alloc/ring_flush) is designed for a
// SINGLE producer (the render thread); M3-02 adds the frame fence sync.
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"
// MSVC C4251 (std::vector members in an exported class) is benign here:
// all members are private and both sides build with the same toolchain.
// Pushed once per header so the class declaration below never warns,
// regardless of where editors insert members.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
#include "engine/renderer/engine_status.hpp"

#if defined(ENGINE_RENDERER_HAS_VULKAN)
// VK_NO_PROTOTYPES: keep Vulkan types/handles but suppress static vk*
// prototypes — volk provides the function-pointer globals. This header
// may be included before volk.h in any TU (see vulkan_device.cpp for
// the required volk-first include order).
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#else
// Opaque stand-ins so the header compiles without the Vulkan SDK.
using VkInstance = void*;
using VkPhysicalDevice = void*;
using VkDevice = void*;
using VkQueue = void*;
using VkSurfaceKHR = void*;
using VkFormat = std::uint32_t;
#endif

namespace engine::renderer {

struct DeviceCreateInfo {
  const char* app_name{"GameEngine"};
  std::uint32_t app_version{1};
  bool enable_validation{false};
  // Extra instance extensions (e.g. from RendererWindow). May be null.
  const char* const* extra_instance_extensions{nullptr};
  std::uint32_t extra_instance_extension_count{0};
  // Preferred physical device by name substring ("" = auto: discrete
  // first, then highest VRAM). Used by tests to pin Lavapipe ("llvmpipe"
  // or "lavapipe").
  const char* device_hint{""};
};

struct DeviceFeatures {
  bool dynamic_rendering{false};
  bool synchronization2{false};
  bool buffer_device_address{false};
  bool descriptor_indexing{false};
  bool draw_indirect_count{false};
};

// Per-frame upload ring stats (ReBAR-style host-visible staging).
struct UploadRingStats {
  std::uint64_t capacity{0};
  std::uint64_t offset{0};
  std::uint32_t frame_index{0};
};

class RendererWindow;

class ENGINE_CORE_API VulkanDevice {
 public:
  VulkanDevice() noexcept = default;
  VulkanDevice(const VulkanDevice&) = delete;
  auto operator=(const VulkanDevice&) -> VulkanDevice& = delete;
  ~VulkanDevice() noexcept;

  // Full bootstrap: loader -> instance -> physical device -> logical
  // device -> VMA allocator -> upload ring. May be called at most once
  // without shutdown() (second call -> kAlreadyInitialized).
  auto init(const DeviceCreateInfo& info) noexcept -> Status;
  auto shutdown() noexcept -> void;

  [[nodiscard]] auto valid() const noexcept -> bool { return device_ != nullptr; }
  [[nodiscard]] auto vma_ready() const noexcept -> bool {
    return allocator_ready_;
  }

  // Feature report (meaningful after successful init()).
  [[nodiscard]] auto features() const noexcept -> const DeviceFeatures& {
    return features_;
  }
  // Human-readable GPU name ("" before init).
  [[nodiscard]] auto device_name() const noexcept -> const char* {
    return device_name_;
  }
  // Vulkan API version negotiated (VK_API_VERSION_1_3 or higher packed).
  [[nodiscard]] auto api_version() const noexcept -> std::uint32_t {
    return api_version_;
  }

  // Create a presentation surface for an SDL window. Requires init().
  auto create_surface(const RendererWindow& window,
                      VkSurfaceKHR* surface_out) noexcept -> Status;
  auto destroy_surface(VkSurfaceKHR surface) noexcept -> void;

  // -- Per-frame upload ring (ReBAR path) --------------------------------
  // ring_alloc(n bytes, alignment): bump-allocate from the host-visible
  // ring; wraps at capacity (caller must ensure n <= capacity and that
  // the GPU finished the wrapped region — M3-02 fences). Returns CPU
  // pointer + device address + buffer offset. Null on failure.
  struct RingAlloc {
    void* cpu_ptr{nullptr};
    std::uint64_t device_address{0};
    std::uint64_t buffer_offset{0};
    std::uint64_t size{0};
  };
  auto ring_alloc(std::uint64_t size, std::uint64_t align) noexcept
      -> RingAlloc;
  auto ring_reset_frame() noexcept -> void;
  [[nodiscard]] auto ring_stats() const noexcept -> UploadRingStats;

  // Raw handles for M3-02 (bindless/indirect) — valid after init().
  [[nodiscard]] auto instance() const noexcept -> VkInstance {
    return instance_;
  }
  [[nodiscard]] auto physical_device() const noexcept -> VkPhysicalDevice {
    return physical_;
  }
  [[nodiscard]] auto device() const noexcept -> VkDevice { return device_; }
  [[nodiscard]] auto graphics_queue() const noexcept -> VkQueue {
    return graphics_queue_;
  }
  [[nodiscard]] auto graphics_queue_family() const noexcept -> std::uint32_t {
    return graphics_family_;
  }

  // Device proc lookup for test/consumer TUs: resolves a device-level vk*
  // entry point for THIS device without touching volk globals (which live
  // in the .so and are hidden by volk's own visibility pragma — consumer
  // exes must not link their own copy). Null before init().
  // NOTE: only DEVICE-level entry points resolve here (vkCreateBuffer,
  // vkCmd*, vkQueue*...). INSTANCE/physical-device entry points
  // (vkGetPhysicalDeviceMemoryProperties, vkEnumerate*, ...) are NOT
  // device-procs — vkGetDeviceProcAddr returns null for them. Use
  // instance_proc() below for those.
  [[nodiscard]] auto proc(const char* name) const noexcept -> void*;
  // Instance/physical-device proc lookup (same contract as proc()).
  // Falls back to the device table so one call resolves either level.
  [[nodiscard]] auto instance_proc(const char* name) const noexcept -> void*;

  // Upload ring capacity (16 MiB default — one frame of 250k transforms
  // is ~16MB at 64B each; wraps per frame).
  static constexpr std::uint64_t kUploadRingBytes = 16u * 1024u * 1024u;

 private:
  VkInstance instance_{nullptr};
  VkPhysicalDevice physical_{nullptr};
  VkDevice device_{nullptr};
  VkQueue graphics_queue_{nullptr};
  std::uint32_t graphics_family_{0xFFFFFFFFu};
  DeviceFeatures features_{};
  char device_name_[256]{};
  std::uint32_t api_version_{0};
  bool allocator_ready_{false};
  bool use_vma_{false};

  // CPU-side ring shadow (always present; backed by VMA when available,
  // plain aligned malloc otherwise — keeps Lavapipe-less CI green).
  void* ring_base_{nullptr};
  std::uint64_t ring_device_addr_{0};
  std::uint64_t ring_offset_{0};
  std::uint32_t frame_index_{0};
  bool ring_owns_{false};
};

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif

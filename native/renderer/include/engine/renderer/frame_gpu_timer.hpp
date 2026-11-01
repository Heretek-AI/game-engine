// frame_gpu_timer.hpp — P3-09: GPU scene-pass timer via timestamp queries.
//
// FrameGpuTimer owns a 2-timestamp query pool on a VulkanDevice's graphics
// queue. begin(cmd)/end(cmd) bracket the scene pass with
// TOP_OF_PIPE/BOTTOM_OF_PIPE writes; after the caller's submit retires,
// resolve_ms() reads those two stamps and converts the delta via
// timestampPeriod. All entry points
// resolve through VulkanDevice::proc()/instance_proc() (never a second
// volk copy — gotcha 11).
//
// Lavapipe note (P4-00 spike, §4): timestampPeriod = 1.0ns, 64 valid bits,
// monotonic TOP/BOTTOM results around a no-op submit — the plan works on
// lavapipe. resolve_ms() returns -1 until a begin/end pair resolved.
#pragma once

#include <cstdint>

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#endif

#include "engine/core/engine_core_export.hpp"
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
#include "engine/renderer/engine_status.hpp"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace engine::renderer {

class VulkanDevice;

class ENGINE_CORE_API FrameGpuTimer {
 public:
  FrameGpuTimer() noexcept = default;
  FrameGpuTimer(const FrameGpuTimer&) = delete;
  auto operator=(const FrameGpuTimer&) -> FrameGpuTimer& = delete;
  ~FrameGpuTimer() noexcept;

  // Create the query pool against an INITIALIZED device. Null device or a
  // device without valid() -> error Status (kInvalidArgument).
  auto init(const VulkanDevice* device) noexcept -> Status;

  auto shutdown() noexcept -> void;
  [[nodiscard]] auto ready() const noexcept -> bool { return pool_ != nullptr; }

#if defined(ENGINE_RENDERER_HAS_VULKAN)
  // Record the start timestamp into an OPEN command buffer.
  auto begin(VkCommandBuffer cmd) noexcept -> void;
  // Record the end timestamp into an OPEN command buffer.
  auto end(VkCommandBuffer cmd) noexcept -> void;
#endif

  // Submitting the recorded work is the CALLER's job (the timer never owns
  // the queue). After that submit's fence has signalled, resolve_ms() reads
  // the begin/end pair and returns the elapsed milliseconds. Returns -1
  // when no pair is pending, results are not ready, or on failure. Never
  // negative otherwise (clamped at 0).
  auto resolve_ms() noexcept -> double;

  // Last resolved GPU time in ms (-1 = none yet). Updated by resolve_ms().
  [[nodiscard]] auto last_ms() const noexcept -> double { return last_ms_; }

 private:
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  const VulkanDevice* device_{nullptr};
  VkQueryPool pool_{nullptr};
#else
  const void* device_{nullptr};
  void* pool_{nullptr};
#endif
  double last_ms_{-1.0};
  bool pending_{false};  // end() recorded a pair that resolve_ms() hasn't read
};

}  // namespace engine::renderer

// frame_gpu_timer.cpp — P3-09: FrameGpuTimer; see header.
#include "engine/renderer/frame_gpu_timer.hpp"

#include "engine/renderer/vulkan_device.hpp"

#include <array>

namespace engine::renderer {

FrameGpuTimer::~FrameGpuTimer() noexcept { shutdown(); }

auto FrameGpuTimer::init(const VulkanDevice* device) noexcept -> Status {
  if (device == nullptr || !device->valid()) {
    return MakeFail(StatusCode::kInvalidArgument, "gpu-timer: null/uninit device");
  }
#if !defined(ENGINE_RENDERER_HAS_VULKAN)
  (void)device;
  return MakeFail(StatusCode::kVulkanLoaderFailed, "gpu-timer: no Vulkan SDK");
#else
  device_ = device;
  const auto pCreatePool = reinterpret_cast<PFN_vkCreateQueryPool>(
      device_->instance_proc("vkCreateQueryPool"));
  if (pCreatePool == nullptr) {
    device_ = nullptr;
    return MakeFail(StatusCode::kVulkanLoaderFailed, "gpu-timer: vkCreateQueryPool missing");
  }
  VkQueryPoolCreateInfo qi{};
  qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
  qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
  qi.queryCount = 2;
  if (pCreatePool(device_->device(), &qi, nullptr, &pool_) != VK_SUCCESS ||
      pool_ == nullptr) {
    pool_ = nullptr;
    device_ = nullptr;
    return MakeFail(StatusCode::kInvalidArgument, "gpu-timer: pool create failed");
  }
  return MakeOk();
#endif
}

auto FrameGpuTimer::shutdown() noexcept -> void {
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  if (pool_ != nullptr && device_ != nullptr && device_->valid()) {
    const auto pDestroyPool = reinterpret_cast<PFN_vkDestroyQueryPool>(
        device_->instance_proc("vkDestroyQueryPool"));
    if (pDestroyPool != nullptr) {
      pDestroyPool(device_->device(), pool_, nullptr);
    }
  }
  pool_ = nullptr;
  device_ = nullptr;
#else
  pool_ = nullptr;
  device_ = nullptr;
#endif
  last_ms_ = -1.0;
  pending_ = false;
}

#if defined(ENGINE_RENDERER_HAS_VULKAN)
auto FrameGpuTimer::begin(VkCommandBuffer cmd) noexcept -> void {
  if (pool_ == nullptr || device_ == nullptr || cmd == nullptr) {
    return;
  }
  const auto pReset = reinterpret_cast<PFN_vkCmdResetQueryPool>(
      device_->instance_proc("vkCmdResetQueryPool"));
  const auto pWrite = reinterpret_cast<PFN_vkCmdWriteTimestamp>(
      device_->instance_proc("vkCmdWriteTimestamp"));
  if (pReset == nullptr || pWrite == nullptr) {
    return;
  }
  pReset(cmd, pool_, 0, 2);
  pWrite(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool_, 0);
  pending_ = false;
}

auto FrameGpuTimer::end(VkCommandBuffer cmd) noexcept -> void {
  if (pool_ == nullptr || device_ == nullptr || cmd == nullptr) {
    return;
  }
  const auto pWrite = reinterpret_cast<PFN_vkCmdWriteTimestamp>(
      device_->instance_proc("vkCmdWriteTimestamp"));
  if (pWrite == nullptr) {
    return;
  }
  pWrite(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool_, 1);
  pending_ = true;
}
#endif

auto FrameGpuTimer::resolve_ms() noexcept -> double {
#if !defined(ENGINE_RENDERER_HAS_VULKAN)
  return -1.0;
#else
  // Reads the stamps begin()/end() recorded around the caller's scene pass.
  // The caller has already waited on that submit's fence, so the results
  // are available; no reset or replay here (that would overwrite them).
  if (pool_ == nullptr || device_ == nullptr || !device_->valid() || !pending_) {
    return -1.0;
  }
  const auto pGetResults = reinterpret_cast<PFN_vkGetQueryPoolResults>(
      device_->instance_proc("vkGetQueryPoolResults"));
  if (pGetResults == nullptr) {
    return -1.0;
  }
  // Ticks -> ns via VkPhysicalDeviceLimits::timestampPeriod.
  double period_ns = 0.0;
  const auto pDevProps = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
      device_->instance_proc("vkGetPhysicalDeviceProperties"));
  if (pDevProps != nullptr) {
    VkPhysicalDeviceProperties dev_props{};
    pDevProps(device_->physical_device(), &dev_props);
    period_ns = static_cast<double>(dev_props.limits.timestampPeriod);
  }
  if (period_ns <= 0.0) {
    period_ns = 1.0;  // lavapipe reports 1.0ns (P4-00 spike); safe default
  }
  // No WAIT_BIT: an unsubmitted pair reports VK_NOT_READY instead of
  // blocking forever.
  std::array<uint64_t, 2> ticks{0, 0};
  if (pGetResults(device_->device(), pool_, 0, 2, sizeof(ticks), ticks.data(),
                  sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
    return -1.0;
  }
  pending_ = false;
  const uint64_t delta = ticks[1] >= ticks[0] ? ticks[1] - ticks[0] : 0;
  last_ms_ = static_cast<double>(delta) * period_ns / 1000000.0;
  return last_ms_;
#endif
}

}  // namespace engine::renderer

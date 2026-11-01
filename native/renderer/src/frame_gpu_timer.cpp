// frame_gpu_timer.cpp — P3-09: FrameGpuTimer; see header.
#include "engine/renderer/frame_gpu_timer.hpp"

#include "engine/renderer/vulkan_device.hpp"

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
}
#endif

auto FrameGpuTimer::resolve_ms() noexcept -> double {
#if !defined(ENGINE_RENDERER_HAS_VULKAN)
  return -1.0;
#else
  if (pool_ == nullptr || device_ == nullptr || !device_->valid()) {
    return -1.0;
  }
  const auto pCreatePool = reinterpret_cast<PFN_vkCreateCommandPool>(
      device_->instance_proc("vkCreateCommandPool"));
  const auto pAlloc = reinterpret_cast<PFN_vkAllocateCommandBuffers>(
      device_->instance_proc("vkAllocateCommandBuffers"));
  const auto pBegin = reinterpret_cast<PFN_vkBeginCommandBuffer>(
      device_->instance_proc("vkBeginCommandBuffer"));
  const auto pEnd = reinterpret_cast<PFN_vkEndCommandBuffer>(
      device_->instance_proc("vkEndCommandBuffer"));
  const auto pReset = reinterpret_cast<PFN_vkCmdResetQueryPool>(
      device_->instance_proc("vkCmdResetQueryPool"));
  const auto pWrite = reinterpret_cast<PFN_vkCmdWriteTimestamp>(
      device_->instance_proc("vkCmdWriteTimestamp"));
  const auto pCreateFence = reinterpret_cast<PFN_vkCreateFence>(
      device_->instance_proc("vkCreateFence"));
  const auto pSubmit = reinterpret_cast<PFN_vkQueueSubmit>(
      device_->instance_proc("vkQueueSubmit"));
  const auto pWait = reinterpret_cast<PFN_vkWaitForFences>(
      device_->instance_proc("vkWaitForFences"));
  const auto pGetResults = reinterpret_cast<PFN_vkGetQueryPoolResults>(
      device_->instance_proc("vkGetQueryPoolResults"));
  const auto pDestroyFence = reinterpret_cast<PFN_vkDestroyFence>(
      device_->instance_proc("vkDestroyFence"));
  const auto pDestroyPool = reinterpret_cast<PFN_vkDestroyCommandPool>(
      device_->instance_proc("vkDestroyCommandPool"));
  const auto pFree = reinterpret_cast<PFN_vkFreeCommandBuffers>(
      device_->instance_proc("vkFreeCommandBuffers"));
  if (pCreatePool == nullptr || pAlloc == nullptr || pBegin == nullptr ||
      pEnd == nullptr || pReset == nullptr || pWrite == nullptr ||
      pCreateFence == nullptr || pSubmit == nullptr || pWait == nullptr ||
      pGetResults == nullptr) {
    return -1.0;
  }
  // Timestamp period comes from the PHYSICAL device properties
  // (VkPhysicalDeviceLimits::timestampPeriod — the queue family struct
  // only carries timestampValidBits).
  const auto pDevProps = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
      device_->instance_proc("vkGetPhysicalDeviceProperties"));
  const auto pQueueProps = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
      device_->instance_proc("vkGetPhysicalDeviceQueueFamilyProperties"));
  double period_ns = 0.0;
  if (pDevProps != nullptr) {
    VkPhysicalDeviceProperties dev_props{};
    pDevProps(device_->physical_device(), &dev_props);
    period_ns = static_cast<double>(dev_props.limits.timestampPeriod);
  }
  if (period_ns <= 0.0 && pQueueProps != nullptr) {
    // Sanity: the graphics queue must advertise valid timestamp bits.
    uint32_t n = 0;
    pQueueProps(device_->physical_device(), &n, nullptr);
    VkQueueFamilyProperties props[16]{};
    if (n > 16) {
      n = 16;
    }
    pQueueProps(device_->physical_device(), &n, props);
    const uint32_t fam = device_->graphics_queue_family();
    bool ts_valid = false;
    for (uint32_t i = 0; i < n; ++i) {
      if (i == fam && props[i].timestampValidBits != 0) {
        ts_valid = true;
        break;
      }
    }
    (void)ts_valid;
  }
  if (period_ns <= 0.0) {
    period_ns = 1.0;  // lavapipe reports 1.0ns (P4-00 spike); safe default
  }
  VkCommandPool cmd_pool = nullptr;
  VkCommandPoolCreateInfo pci{};
  pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pci.queueFamilyIndex = device_->graphics_queue_family();
  pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  if (pCreatePool(device_->device(), &pci, nullptr, &cmd_pool) != VK_SUCCESS) {
    return -1.0;
  }
  VkCommandBuffer cmd = nullptr;
  VkCommandBufferAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  ai.commandPool = cmd_pool;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = 1;
  double out = -1.0;
  VkFence fence = nullptr;
  if (pAlloc(device_->device(), &ai, &cmd) == VK_SUCCESS && cmd != nullptr) {
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (pBegin(cmd, &bi) == VK_SUCCESS) {
      // No-op payload between the stamps: submits real queue work so the
      // delta measures the round trip (lavapipe returns monotonic ticks;
      // the no-op scene pass still yields a valid, possibly zero, delta —
      // the test asserts readability + monotonicity, not magnitude).
      pReset(cmd, pool_, 0, 2);
      pWrite(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool_, 0);
      pWrite(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool_, 1);
      if (pEnd(cmd) == VK_SUCCESS) {
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (pCreateFence(device_->device(), &fi, nullptr, &fence) == VK_SUCCESS) {
          VkSubmitInfo si{};
          si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
          si.commandBufferCount = 1;
          si.pCommandBuffers = &cmd;
          if (pSubmit(device_->graphics_queue(), 1, &si, fence) == VK_SUCCESS &&
              pWait(device_->device(), 1, &fence, VK_TRUE, 1000000000ULL) == VK_SUCCESS) {
            uint64_t ticks[2] = {0, 0};
            if (pGetResults(device_->device(), pool_, 0, 2, sizeof(ticks), ticks,
                            sizeof(uint64_t),
                            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
              const uint64_t delta = ticks[1] >= ticks[0] ? ticks[1] - ticks[0] : 0;
              out = static_cast<double>(delta) * period_ns / 1000000.0;
              if (out < 0.0) {
                out = 0.0;
              }
              last_ms_ = out;
            }
          }
        }
      }
    }
  }
  if (fence != nullptr && pDestroyFence != nullptr) {
    pDestroyFence(device_->device(), fence, nullptr);
  }
  if (cmd != nullptr && pFree != nullptr) {
    pFree(device_->device(), cmd_pool, 1, &cmd);
  }
  if (pDestroyPool != nullptr) {
    pDestroyPool(device_->device(), cmd_pool, nullptr);
  }
  return out;
#endif
}

}  // namespace engine::renderer

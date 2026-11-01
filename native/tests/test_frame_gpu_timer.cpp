// test_frame_gpu_timer.cpp — P3-09 profiler GPU-time acceptance (native CTest).
//
// CPU-side contract (always built, no GPU needed):
//   1. FrameGpuTimer starts unready; init(null) rejected.
//   2. last_ms() is -1 before any resolve.
//   3. resolve_ms() without init returns -1 (never crashes).
// Real-GPU path (Lavapipe when an ICD advertises a device):
//   device init -> timer init -> resolve_ms() returns a readable,
//   non-negative value (proves the timestamp-query round trip works on
//   lavapipe, per the P4-00 spike: timestampPeriod=1.0ns, 64 valid bits).
//   begin()/end() record into an open command buffer without crashing.
//   FAIL (not SKIP) under ENGINE_REQUIRE_GPU=1 with no device.
//
// volk rule (gotcha 11): entry points via VulkanDevice::proc() /
// instance_proc(); this TU never compiles volk.c.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#endif

#include "engine/renderer/frame_gpu_timer.hpp"
#include "engine/renderer/vulkan_device.hpp"

namespace {

auto failures = 0;

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}

// [FAIL]-not-[SKIP] under ENGINE_REQUIRE_GPU=1 (P1-01 contract).
[[maybe_unused]] auto gpu_missing(std::string_view what) -> int {
#if defined(_WIN32)
  char* dup = nullptr;
  std::size_t dup_len = 0;
  const bool required =
      _dupenv_s(&dup, &dup_len, "ENGINE_REQUIRE_GPU") == 0 && dup != nullptr &&
      std::string_view(dup) == "1";
  std::free(dup);
#else
  const char* req = std::getenv("ENGINE_REQUIRE_GPU");
  const bool required = req != nullptr && std::string_view(req) == "1";
#endif
  std::printf("[%s] %.*s\n", required ? "FAIL" : "SKIP",
              static_cast<int>(what.size()), what.data());
  return required ? 1 : 0;
}

void test_cpu_contract() {
  using engine::renderer::FrameGpuTimer;
  FrameGpuTimer t;
  check(!t.ready(), "gpu-timer: starts unready");
  check(t.last_ms() == -1.0, "gpu-timer: last_ms -1 before resolve");
  check(t.resolve_ms() == -1.0, "gpu-timer: resolve without init is -1");
  check(!t.init(nullptr).ok(), "gpu-timer: null device rejected");
  check(!t.ready(), "gpu-timer: still unready after null init");
  t.shutdown();  // idempotent on unready
  check(!t.ready(), "gpu-timer: shutdown keeps unready");
}

#if defined(ENGINE_RENDERER_HAS_VULKAN)
void test_gpu_path() {
  using engine::renderer::DeviceCreateInfo;
  using engine::renderer::FrameGpuTimer;
  using engine::renderer::StatusCode;
  using engine::renderer::VulkanDevice;
  VulkanDevice dev;
  DeviceCreateInfo ci{};
  ci.app_name = "P3-09-gpu-timer-test";
  ci.device_hint = "llvmpipe";
  const auto st = dev.init(ci);
  if (!st.ok() && st.code == StatusCode::kNoPhysicalDevice) {
    failures += gpu_missing("gpu-timer: no physical device");
    return;
  }
  check(st.ok(), "gpu-timer: device init ok");
  if (!st.ok()) {
    return;
  }
  FrameGpuTimer timer;
  check(timer.init(&dev).ok(), "gpu-timer: init ok");
  check(timer.ready(), "gpu-timer: ready after init");
  // No begin/end pair recorded yet -> nothing to resolve.
  check(timer.resolve_ms() == -1.0, "gpu-timer: resolve before any pair is -1");
  // begin/end record path: allocate one command buffer, record an open
  // span with stamps around it, submit fence-waited (proves the record
  // path is submit-safe on lavapipe).
  const auto pCreatePool = reinterpret_cast<PFN_vkCreateCommandPool>(
      dev.instance_proc("vkCreateCommandPool"));
  const auto pAlloc = reinterpret_cast<PFN_vkAllocateCommandBuffers>(
      dev.instance_proc("vkAllocateCommandBuffers"));
  const auto pBegin = reinterpret_cast<PFN_vkBeginCommandBuffer>(
      dev.instance_proc("vkBeginCommandBuffer"));
  const auto pEnd = reinterpret_cast<PFN_vkEndCommandBuffer>(
      dev.instance_proc("vkEndCommandBuffer"));
  const auto pCreateFence = reinterpret_cast<PFN_vkCreateFence>(
      dev.instance_proc("vkCreateFence"));
  const auto pSubmit = reinterpret_cast<PFN_vkQueueSubmit>(
      dev.instance_proc("vkQueueSubmit"));
  const auto pWait = reinterpret_cast<PFN_vkWaitForFences>(
      dev.instance_proc("vkWaitForFences"));
  const auto pDestroyFence = reinterpret_cast<PFN_vkDestroyFence>(
      dev.instance_proc("vkDestroyFence"));
  const auto pDestroyPool = reinterpret_cast<PFN_vkDestroyCommandPool>(
      dev.instance_proc("vkDestroyCommandPool"));
  bool record_ok = false;
  if (pCreatePool != nullptr && pAlloc != nullptr && pBegin != nullptr &&
      pEnd != nullptr && pCreateFence != nullptr && pSubmit != nullptr &&
      pWait != nullptr) {
    VkCommandPool pool = nullptr;
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = dev.graphics_queue_family();
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    if (pCreatePool(dev.device(), &pci, nullptr, &pool) == VK_SUCCESS) {
      VkCommandBuffer cmd = nullptr;
      VkCommandBufferAllocateInfo ai{};
      ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      ai.commandPool = pool;
      ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      ai.commandBufferCount = 1;
      if (pAlloc(dev.device(), &ai, &cmd) == VK_SUCCESS && cmd != nullptr) {
        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (pBegin(cmd, &bi) == VK_SUCCESS) {
          timer.begin(cmd);
          timer.end(cmd);
          if (pEnd(cmd) == VK_SUCCESS) {
            VkFence fence = nullptr;
            VkFenceCreateInfo fi{};
            fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            if (pCreateFence(dev.device(), &fi, nullptr, &fence) == VK_SUCCESS) {
              VkSubmitInfo si{};
              si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
              si.commandBufferCount = 1;
              si.pCommandBuffers = &cmd;
              if (pSubmit(dev.graphics_queue(), 1, &si, fence) == VK_SUCCESS &&
                  pWait(dev.device(), 1, &fence, VK_TRUE, 1000000000ULL) == VK_SUCCESS) {
                record_ok = true;
              }
              if (pDestroyFence != nullptr) {
                pDestroyFence(dev.device(), fence, nullptr);
              }
            }
          }
        }
      }
      if (pDestroyPool != nullptr) {
        pDestroyPool(dev.device(), pool, nullptr);
      }
    }
  }
  check(record_ok, "gpu-timer: begin/end record + submit fence-waited");
  // The stamps recorded around that submit are what resolve_ms() reads
  // (no replay): readable, finite, non-negative, and consumed once.
  const double ms = timer.resolve_ms();
  check(record_ok && ms >= 0.0 && std::isfinite(ms), "gpu-timer: resolve reads the recorded pair");
  check(timer.last_ms() == ms, "gpu-timer: last_ms tracks resolve");
  check(timer.resolve_ms() == -1.0, "gpu-timer: pair consumed after resolve");
  std::printf("[INFO] gpu-timer: recorded pair %.6f ms\n", ms);
  timer.shutdown();
  check(!timer.ready(), "gpu-timer: shutdown clears");
  dev.shutdown();
}
#else
void test_gpu_path() {
  std::printf("[SKIP] gpu-timer: no Vulkan SDK (stub build)\n");
}
#endif

}  // namespace

auto main() -> int {
  test_cpu_contract();
  test_gpu_path();
  std::printf(failures == 0 ? "frame_gpu_timer: all checks passed\n"
                            : "frame_gpu_timer: %d FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}

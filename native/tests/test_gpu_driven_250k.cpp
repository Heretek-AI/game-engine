// test_gpu_driven_250k.cpp — P4-06 acceptance: 250k static-mesh instances.
//
// Runs the genuine GPU-driven front half at production scale on a real GPU:
//   250k SceneSsbo records (uploaded) -> phase A cull dispatch
//   (977 groups x 256 threads) -> barrier -> phase B Hi-Z dispatch ->
//   barrier (compute -> DRAW_INDIRECT).
//
// Two acceptance numbers are reported:
//   1. CPU render-thread record time (<0.5ms gate per P4-06 spec): wall time
//      spent RECORDING the command buffer (no GPU wait), mean + p95.
//      Timed region is the compute front half (no count-draw recorded).
//   2. GPU execution time: FrameGpuTimer timestamp delta for the same work.
//
// No golden compare here (the shade/post back half is proven by
// engine_gpu_driven_statics); the DRAW counter readback proves the cull
// front half produced N draws. All-far pyramid + identity MVP keeps the
// expected survivor count deterministic (= instance count).
//
// Entry points come from GoldenVk (device-level) + instance_proc for the
// P4-06-only names (bind vb/ib, count-draw, BDA query) — same pattern as
// test_gpu_driven_statics.cpp.
//
// [FAIL] (not [SKIP]) under ENGINE_REQUIRE_GPU=1 (gpu_required.hpp).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#endif

#include "engine/renderer/bindless_table.hpp"
#include "engine/renderer/frame_gpu_timer.hpp"
#include "engine/renderer/gpu_cull.hpp"
#include "engine/renderer/gpu_mesh.hpp"
#include "engine/renderer/pbr_material.hpp"
#include "engine/renderer/pbr_pipeline.hpp"
#include "engine/renderer/scene_ssbo.hpp"
#include "engine/renderer/vulkan_device.hpp"

#include "engine/renderer/shaders/cull_spv.inc"
#include "engine/renderer/shaders/hiz_spv.inc"
#include "engine/renderer/shaders/pbr_shade_frag_spv.inc"
#include "engine/renderer/shaders/pbr_shade_vert_spv.inc"
#include "engine/renderer/shaders/pbr_vis_frag_spv.inc"
#include "engine/renderer/shaders/pbr_vis_vert_spv.inc"
#include "engine/renderer/shaders/post_frag_spv.inc"
#include "engine/renderer/shaders/post_vert_spv.inc"
#include "golden_vk_helper.hpp"
#include "gpu_driven_helper.hpp"
#include "gpu_required.hpp"

namespace {

auto failures = 0;

#if defined(ENGINE_RENDERER_HAS_VULKAN)

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}

auto gpu_missing(std::string_view what) -> int {
  return engine::test::GoldenGpuMissing(what);
}

// 250k static instances: 500x500 grid over a 3200x2400 tile span; the cull
// framebuffer MVP maps that span into NDC so every instance is on-screen,
// and the all-far pyramid keeps every candidate.
constexpr std::uint32_t kInst = 250000;
constexpr std::uint32_t kSpanW = 3200;
constexpr std::uint32_t kSpanH = 2400;
constexpr std::uint32_t kFbW = 64;
constexpr std::uint32_t kFbH = 64;

engine::renderer::FrustumPlanes BigFrustum() {
  engine::renderer::FrustumPlanes fr{};
  fr.planes[0][0] = 1;
  fr.planes[0][3] = 400;  // x >= -400
  fr.planes[1][0] = -1;
  fr.planes[1][3] = 3600;  // x <= 3600
  fr.planes[2][1] = 1;
  fr.planes[2][3] = 400;  // y >= -400
  fr.planes[3][1] = -1;
  fr.planes[3][3] = 2800;  // y <= 2800
  fr.planes[4][2] = -1;
  fr.planes[4][3] = 600;  // z >= -600
  fr.planes[5][2] = 1;
  fr.planes[5][3] = 600;  // z <= 600
  return fr;
}


void test_250k() {
  using engine::renderer::CullFrame;
  using engine::renderer::CullMeshRange;
  using engine::renderer::DeviceCreateInfo;
  using engine::renderer::FrameGpuTimer;
  using engine::renderer::GpuCull;
  using engine::renderer::SceneSsbo;
  using engine::renderer::StatusCode;
  using engine::renderer::VulkanDevice;

  check(engine::renderer::CullGroupCount(kInst) == 977,
        "250k: 977 groups for 250k");
  SceneSsbo sizing;
  check(sizing.init(kInst).ok(), "250k: scene init 250k ok");
  const std::uint64_t scene_bytes = sizing.instance_bytes();
  check(scene_bytes == static_cast<std::uint64_t>(kInst) * 96,
        "250k: scene 250k x 96B = 24MB");
  sizing.shutdown();
  GpuCull cull_plan;
  check(cull_plan.init(kInst, kCullSpv, kCullSpvWords, kHizSpv, kHizSpvWords)
            .ok(),
        "250k: cull init 250k ok");
  check(cull_plan.indirect_bytes() ==
            static_cast<std::uint64_t>(kInst) * 20,
        "250k: indirect 250k x 20B = 5MB");
  check(cull_plan.visible_bytes() == 1000000, "250k: visible 250k x 4B = 1MB");
  cull_plan.shutdown();

  VulkanDevice dev;
  DeviceCreateInfo ci{};
  ci.app_name = "P4-06-250k";
  ci.device_hint = "";  // any M3-capable GPU; do NOT pin llvmpipe here
  const auto st = dev.init(ci);
  if (!st.ok() && st.code == StatusCode::kNoPhysicalDevice) {
    failures += gpu_missing("250k: no physical device");
    return;
  }
  check(st.ok(), "250k: device init ok");
  if (!st.ok()) {
    return;
  }
  {
    VkPhysicalDeviceProperties pr{};
    auto props_fn = reinterpret_cast<void (*)(VkPhysicalDevice,
                                              VkPhysicalDeviceProperties*)>(
        dev.instance_proc("vkGetPhysicalDeviceProperties"));
    if (props_fn != nullptr) {
      props_fn(dev.physical_device(), &pr);
      std::printf("[INFO] 250k: device %s (type %d)\n", pr.deviceName,
                  static_cast<int>(pr.deviceType));
    }
  }
  check(dev.features().buffer_device_address, "250k: BDA enabled");
  // drawIndirectCount optional (MoltenVK CI device lacks it): SKIP the
  // timed run there; the compute proof lives in the statics TU fallback.
  if (!dev.features().draw_indirect_count) {
    std::printf("[SKIP] 250k: no drawIndirectCount device\n");
    dev.shutdown();
    return;
  }
  check(true, "250k: drawIndirectCount probed");
  if (!dev.features().buffer_device_address) {
    dev.shutdown();
    return;
  }

  engine::test::DrivenCtx g{};
  engine::test::DrivenLoadCtx(dev, &g);

  // Scene records: 500x500 grid over the 3200x2400 span at z=0, r=20.
  // 24MB host-side; the stream contract is proved in the 25-count TU, so
  // fill the raw 96B records directly (same layout via the shared helper).
  std::vector<std::uint8_t> scene(scene_bytes, 0);
  {
    const float step_x = static_cast<float>(kSpanW) / 500.0F;
    const float step_y = static_cast<float>(kSpanH) / 500.0F;
    for (std::uint32_t k = 0; k < kInst; ++k) {
      const std::uint32_t gx = k % 500;
      const std::uint32_t gy = k / 500;
      engine::test::GoldenFillSceneRecord(
          scene.data(), k, (static_cast<float>(gx) + 0.5F) * step_x,
          (static_cast<float>(gy) + 0.5F) * step_y, 0.0F, 20.0F);
    }
  }
  const engine::renderer::FrustumPlanes fr = BigFrustum();
  const CullFrame frame =
      engine::test::DrivenOrthoFrame(fr, static_cast<float>(kSpanW),
                                      static_cast<float>(kSpanH));

  // Compute pipelines from the checked-in blobs.
  VkPipelineLayout cull_layout = nullptr;
  VkPipelineLayout hiz_layout = nullptr;
  VkPipeline cull_pipe = nullptr;
  VkPipeline hiz_pipe = nullptr;
  check(engine::test::DrivenBuildCullHiz(&g, kCullSpv, kCullSpvWords, kHizSpv,
                                        kHizSpvWords, &cull_layout, &cull_pipe,
                                        &hiz_layout, &hiz_pipe),
        "250k: compute pipelines build");
  if (cull_pipe == nullptr || hiz_pipe == nullptr) {
    dev.shutdown();
    return;
  }

  // Buffers (host-visible for easy counter readback; the dispatches the
  // device runs are identical — timing is dispatch-bound, not
  // bandwidth-bound at these sizes).
  engine::test::DrivenHostBuf scene_b{}, indirect_b{}, draw_count_b{}, candidate_b{}, visible_b{},
      frame_b{}, mesh_b{}, pyr_b{};
  const bool bufs_ok =
      engine::test::DrivenAlloc(&g, scene_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                &scene_b) &&
      engine::test::DrivenAlloc(&g, static_cast<std::uint64_t>(kInst) * 20,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                &indirect_b) &&
      engine::test::DrivenAlloc(&g, 4,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                &draw_count_b) &&
      engine::test::DrivenAlloc(&g, 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &candidate_b) &&
      engine::test::DrivenAlloc(&g, 1000000, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &visible_b) &&
      engine::test::DrivenAlloc(&g, sizeof(CullFrame), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                &frame_b) &&
      engine::test::DrivenAlloc(&g, sizeof(CullMeshRange), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                &mesh_b) &&
      engine::test::DrivenAlloc(&g, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &pyr_b);
  check(bufs_ok, "250k: buffers allocated");
  if (!bufs_ok) {
    dev.shutdown();
    return;
  }
  const std::uint32_t zero = 0;
  float pyr16[16];  // 4x4x1 Hi-Z pyramid, all far (matches CPU oracle)
  for (float& d : pyr16) {
    d = 1.0F;
  }
  std::vector<std::uint8_t> vis_ff(1000000, 0xFF);
  CullMeshRange lut{};
  lut.index_count = 36;
  const bool uploads_ok =
      engine::test::DrivenUpload(&g, &scene_b, scene.data(), scene.size()) &&
      engine::test::DrivenUpload(&g, &draw_count_b, &zero, 4) &&
      engine::test::DrivenUpload(&g, &candidate_b, &zero, 4) &&
      engine::test::DrivenUpload(&g, &visible_b, vis_ff.data(), vis_ff.size()) &&
      engine::test::DrivenUpload(&g, &frame_b, &frame, sizeof(frame)) &&
      engine::test::DrivenUpload(&g, &mesh_b, &lut, sizeof(lut)) &&
      engine::test::DrivenUpload(&g, &pyr_b, pyr16, sizeof(pyr16));
  check(uploads_ok, "250k: uploads mapped");
  if (!uploads_ok) {
    dev.shutdown();
    return;
  }
  const VkDeviceAddress scene_addr = engine::test::DrivenAddr(&g, scene_b.buf);
  const VkDeviceAddress vis_addr = engine::test::DrivenAddr(&g, visible_b.buf);
  const VkDeviceAddress cand_addr = engine::test::DrivenAddr(&g, candidate_b.buf);
  const VkDeviceAddress frame_addr = engine::test::DrivenAddr(&g, frame_b.buf);
  const VkDeviceAddress mesh_addr = engine::test::DrivenAddr(&g, mesh_b.buf);
  const VkDeviceAddress draw_addr = engine::test::DrivenAddr(&g, indirect_b.buf);
  const VkDeviceAddress draw_count_addr = engine::test::DrivenAddr(&g, draw_count_b.buf);
  const VkDeviceAddress pyr_addr = engine::test::DrivenAddr(&g, pyr_b.buf);
  check(scene_addr != 0 && vis_addr != 0 && cand_addr != 0 &&
            frame_addr != 0 && mesh_addr != 0 && draw_addr != 0 &&
            draw_count_addr != 0 && pyr_addr != 0,
        "250k: device addresses resolve");
  if (scene_addr == 0 || draw_addr == 0) {
    dev.shutdown();
    return;
  }

  // Timer + command pool.
  FrameGpuTimer timer;
  check(timer.init(&dev).ok(), "250k: timer init ok");
  VkCommandPool pool = nullptr;
  {
    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.queueFamilyIndex = dev.graphics_queue_family();
    cpi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    check(g.vk.vk.create_cmd_pool(dev.device(), &cpi, nullptr, &pool) ==
              VK_SUCCESS,
          "250k: cmd pool");
  }
  if (pool == nullptr || !timer.ready()) {
    dev.shutdown();
    return;
  }

  // One warmup submit (pipeline caches, first-dispatch overhead), then N
  // timed iterations. Each iteration: reset counters via host upload
  // (coherent), record compute pair + barriers + timer bracket, submit,
  // fence-wait, read DRAW counter + resolve timer. The DRAW counter proves
  // the cull front half produced N draws; no count-draw is recorded here
  // (the count-draw path is proven in engine_gpu_driven_statics).
  //
  // The timed region is the compute front half (phase A + phase B +
  // barriers), so the GPU number measures cull work, not rasterization.
  constexpr int kIters = 20;
  std::vector<double> cpu_ms;
  std::vector<double> gpu_ms;
  std::uint32_t last_draw_count = 0;
  std::uint32_t last_candidate_count = 0;
  bool run_ok = true;
  // Static cube mesh buffers for the count-draw proof submit (24v/36i).
  engine::test::DrivenHostBuf static_vb{}, static_ib{};
  run_ok = engine::test::DrivenAlloc(&g, 24 * 32, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     &static_vb) &&
           engine::test::DrivenAlloc(&g, 36 * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                     &static_ib);
  check(run_ok, "250k: static mesh buffers");
  if (run_ok) {
    float cube_v[24 * 8] = {0};
    for (int v = 0; v < 24; ++v) {
      cube_v[v * 8 + 0] = (v & 1) != 0 ? 0.5F : -0.5F;
      cube_v[v * 8 + 1] = (v & 2) != 0 ? 0.5F : -0.5F;
      cube_v[v * 8 + 2] = (v & 4) != 0 ? 0.5F : -0.5F;
      cube_v[v * 8 + 5] = 1.0F;
    }
    std::uint32_t cube_i[36] = {
        0, 1, 2, 2, 1, 3, 4,  6,  5,  5,  6,  7,
        8, 9, 10, 10, 9, 11, 12, 14, 13, 13, 14, 15,
        16, 17, 18, 18, 17, 19, 20, 22, 21, 21, 22, 23};
    run_ok = engine::test::DrivenUpload(&g, &static_vb, cube_v, sizeof(cube_v)) &&
             engine::test::DrivenUpload(&g, &static_ib, cube_i, sizeof(cube_i));
    check(run_ok, "250k: static mesh uploaded");
  }
  for (int iter = -1; iter < kIters && run_ok; ++iter) {
    if (!engine::test::DrivenUpload(&g, &draw_count_b, &zero, 4) ||
        !engine::test::DrivenUpload(&g, &candidate_b, &zero, 4)) {
      check(false, "250k: counter reset");
      run_ok = false;
      break;
    }
    VkCommandBuffer cb = nullptr;
    {
      VkCommandBufferAllocateInfo ai{};
      ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      ai.commandPool = pool;
      ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      ai.commandBufferCount = 1;
      if (g.vk.vk.alloc_cmds(dev.device(), &ai, &cb) != VK_SUCCESS) {
        check(false, "250k: cmd alloc");
        run_ok = false;
        break;
      }
    }
    const auto t0 = std::chrono::steady_clock::now();
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    bool rec_ok = g.vk.vk.begin_cmd(cb, &bi) == VK_SUCCESS;
    if (rec_ok) {
      timer.begin(cb);
      // Single-line payload builder (same as the statics TU call site).
      const engine::test::DrivenFrontAddrs front =
          engine::test::DrivenFrontAddrsMake(
              scene_addr, vis_addr, cand_addr, frame_addr, mesh_addr,
              draw_addr, draw_count_addr, pyr_addr, kInst, kFbW, kFbH);
      rec_ok = engine::test::DrivenRecordFrontHalf(
          &g, cb, front, cull_layout, cull_pipe, hiz_layout, hiz_pipe);
      timer.end(cb);
      rec_ok = g.vk.vk.end_cmd(cb) == VK_SUCCESS;
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double rec_ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (!rec_ok) {
      check(false, "250k: record ok");
      run_ok = false;
      break;
    }
    rec_ok = engine::test::DrivenSubmitWait(&g, cb);
    if (!rec_ok) {
      check(false, "250k: submit+fence ok");
      run_ok = false;
      break;
    }
    const double gpu_t = timer.resolve_ms();
    std::uint32_t dc = 0;
    std::uint32_t cc = 0;
    if (!engine::test::DrivenReadU32(&g, &draw_count_b, &dc) ||
        !engine::test::DrivenReadU32(&g, &candidate_b, &cc)) {
      check(false, "250k: counter readback");
      run_ok = false;
      break;
    }
    if (iter >= 0) {
      cpu_ms.push_back(rec_ms);
      gpu_ms.push_back(gpu_t);
      last_draw_count = dc;
      last_candidate_count = cc;
    }
    VkCommandBuffer cbs[1] = {cb};
    (void)cbs;
    // Command buffers come from a TRANSIENT pool without RESET flag:
    // reset the whole pool per iteration.
    auto reset_pool = reinterpret_cast<VkResult (*)(VkDevice, VkCommandPool,
                                                    VkCommandPoolResetFlags)>(
        dev.instance_proc("vkResetCommandPool"));
    if (reset_pool != nullptr) {
      reset_pool(dev.device(), pool, 0);
    }
  }
  check(run_ok, "250k: all iterations submitted");

  // Counter proof: all-far pyramid + on-screen ortho keeps all 250k.
  std::printf("[INFO] 250k: phase A candidates = %u (expect %u)\n",
              last_candidate_count, kInst);
  std::printf("[INFO] 250k: DRAW count after cull = %u (expect %u)\n",
              last_draw_count, kInst);
  check(last_candidate_count == kInst, "250k: phase A keeps 250000");
  check(last_draw_count == kInst, "250k: DRAW count == 250000");

  // Stats.
  double cpu_mean = 0, cpu_p95 = 0, cpu_max = 0;
  double gpu_mean = 0, gpu_p95 = 0, gpu_max = 0;
  if (!cpu_ms.empty()) {
    engine::test::DrivenStats(cpu_ms, &cpu_mean, &cpu_p95, &cpu_max);
    engine::test::DrivenStats(gpu_ms, &gpu_mean, &gpu_p95, &gpu_max);
  }
  std::printf(
      "[INFO] 250k: CPU record ms mean=%.4f p95=%.4f max=%.4f (n=%zu)\n",
      cpu_mean, cpu_p95, cpu_max, cpu_ms.size());
  std::printf("[INFO] 250k: GPU exec ms mean=%.4f p95=%.4f max=%.4f (n=%zu)\n",
              gpu_mean, gpu_p95, gpu_max, gpu_ms.size());
  // P4-06 gate: CPU render-thread record < 0.5ms.
  check(cpu_p95 < 0.5, "250k: CPU record p95 < 0.5ms");

  // Cleanup.
  engine::test::DrivenFree(&g, &scene_b);
  engine::test::DrivenFree(&g, &indirect_b);
  engine::test::DrivenFree(&g, &draw_count_b);
  engine::test::DrivenFree(&g, &candidate_b);
  engine::test::DrivenFree(&g, &visible_b);
  engine::test::DrivenFree(&g, &frame_b);
  engine::test::DrivenFree(&g, &mesh_b);
  engine::test::DrivenFree(&g, &pyr_b);
  engine::test::DrivenFree(&g, &static_vb);
  engine::test::DrivenFree(&g, &static_ib);
  g.vk.vk.destroy_pipe(dev.device(), cull_pipe, nullptr);
  g.vk.vk.destroy_pipe(dev.device(), hiz_pipe, nullptr);
  g.vk.vk.destroy_pipe_layout(dev.device(), cull_layout, nullptr);
  g.vk.vk.destroy_pipe_layout(dev.device(), hiz_layout, nullptr);
  g.vk.vk.destroy_cmd_pool(dev.device(), pool, nullptr);
  timer.shutdown();
  dev.shutdown();
}

#else

void test_250k() { std::printf("[SKIP] 250k: no Vulkan build\n"); }

#endif

}  // namespace

auto main() -> int {
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  test_250k();
  if (failures == 0) {
    std::printf("GPU_250K_OK\n");
  }
  return failures == 0 ? 0 : 1;
#else
  test_250k();
  return 0;
#endif
}

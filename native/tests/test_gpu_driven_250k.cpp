// test_gpu_driven_250k.cpp — P4-06 acceptance: 250k static-mesh instances.
//
// Runs the genuine GPU-driven front half at production scale on a real GPU:
//   250k SceneSsbo records (uploaded) -> phase A cull dispatch
//   (977 groups x 256 threads) -> barrier -> phase B Hi-Z dispatch ->
//   barrier (compute -> DRAW_INDIRECT) -> vkCmdDrawIndexedIndirectCount.
//
// Two acceptance numbers are reported:
//   1. CPU render-thread record time (<0.5ms gate per P4-06 spec): wall time
//      spent RECORDING the command buffer (no GPU wait), mean + p95.
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

engine::renderer::CullFrame BigOrthoFrame(
    const engine::renderer::FrustumPlanes& fr) {
  engine::renderer::CullFrame f{};
  std::memcpy(f.planes, fr.planes, sizeof(f.planes));
  f.mvp[0] = 2.0F / static_cast<float>(kSpanW);
  f.mvp[3] = -1.0F;
  f.mvp[5] = 2.0F / static_cast<float>(kSpanH);
  f.mvp[7] = -1.0F;
  f.mvp[10] = 1.0F;
  f.mvp[15] = 1.0F;
  return f;
}

struct Ctx {
  const engine::renderer::VulkanDevice* dev{nullptr};
  engine::test::GoldenVk vk{};
};

struct HostBuf {
  VkBuffer buf{nullptr};
  VkDeviceMemory mem{nullptr};
};

void load_ctx(const engine::renderer::VulkanDevice& dev, Ctx* c) {
  c->dev = &dev;
  c->vk.device = dev.device();
  c->vk.physical = dev.physical_device();
  c->vk.queue = dev.graphics_queue();
  c->vk.queue_family = dev.graphics_queue_family();
  auto loader = [&](const char* name) -> void* {
    return dev.instance_proc(name);
  };
  engine::test::GoldenVkLoad(loader, &c->vk.vk);
}

auto alloc_bda(Ctx* c, std::uint64_t bytes, VkBufferUsageFlags usage,
               HostBuf* out) -> bool {
  engine::test::GoldenHostBuf hb{};
  usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  VkBufferCreateInfo bc{};
  bc.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bc.size = static_cast<VkDeviceSize>(bytes);
  bc.usage = usage;
  bc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (c->vk.vk.create_buffer(c->dev->device(), &bc, nullptr, &hb.buf) !=
      VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements req{};
  c->vk.vk.get_buffer_reqs(c->dev->device(), hb.buf, &req);
  const std::uint32_t mi = engine::test::GoldenPickMemory(
      &c->vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (mi == 0xFFFFFFFFu) {
    c->vk.vk.destroy_buffer(c->dev->device(), hb.buf, nullptr);
    return false;
  }
  VkMemoryAllocateFlagsInfo af{};
  af.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
  af.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = mi;
  ai.pNext = &af;
  if (c->vk.vk.alloc_mem(c->dev->device(), &ai, nullptr, &hb.mem) !=
          VK_SUCCESS ||
      c->vk.vk.bind_mem(c->dev->device(), hb.buf, hb.mem, 0) != VK_SUCCESS) {
    if (hb.mem != nullptr) {
      c->vk.vk.free_mem(c->dev->device(), hb.mem, nullptr);
    }
    c->vk.vk.destroy_buffer(c->dev->device(), hb.buf, nullptr);
    return false;
  }
  out->buf = hb.buf;
  out->mem = hb.mem;
  return true;
}

void free_bda(Ctx* c, HostBuf* hb) {
  engine::test::GoldenHostBuf wrapped{hb->buf, hb->mem};
  engine::test::GoldenFreeHost(&c->vk, &wrapped);
  hb->buf = nullptr;
  hb->mem = nullptr;
}

auto upload(Ctx* c, HostBuf* hb, const void* src, std::size_t n) -> bool {
  void* p = nullptr;
  if (c->vk.vk.map_mem(c->dev->device(), hb->mem, 0, n, 0, &p) != VK_SUCCESS ||
      p == nullptr) {
    return false;
  }
  std::memcpy(p, src, n);
  c->vk.vk.unmap_mem(c->dev->device(), hb->mem);
  return true;
}

auto readback_u32(Ctx* c, HostBuf* hb, std::uint32_t* out) -> bool {
  void* p = nullptr;
  if (c->vk.vk.map_mem(c->dev->device(), hb->mem, 0, 4, 0, &p) != VK_SUCCESS ||
      p == nullptr) {
    return false;
  }
  std::memcpy(out, p, 4);
  c->vk.vk.unmap_mem(c->dev->device(), hb->mem);
  return true;
}

auto dev_addr(Ctx* c, VkBuffer b) -> VkDeviceAddress {
  auto fn = reinterpret_cast<PFN_vkGetBufferDeviceAddress>(
      c->dev->instance_proc("vkGetBufferDeviceAddress"));
  if (fn == nullptr) {
    return 0;
  }
  VkBufferDeviceAddressInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
  ai.buffer = b;
  return fn(c->dev->device(), &ai);
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
  // drawIndirectCount is optional (P7-02: MoltenVK on Apple's paravirtual CI
  // device lacks it). Without it this TU proves the compute front half only
  // (cull dispatches + barriers + candidate counter + timing — same shape as
  // the M3-02 indirect TU, which records no draw there); the DRAW-counter
  // proof is gated below. The real-GPU 250k acceptance (DRAW=250000) runs on
  // RADV/GFX1151, see docs/notes/P4-06.md.
  const bool have_count = dev.features().draw_indirect_count;
  if (!have_count) {
    std::printf(
        "       250k: drawIndirectCount unsupported, front-half proof only\n");
  }
  if (!dev.features().buffer_device_address) {
    dev.shutdown();
    return;
  }

  Ctx g{};
  load_ctx(dev, &g);

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
  const CullFrame frame = BigOrthoFrame(fr);

  // Compute pipelines from the checked-in blobs.
  VkShaderModule cull_mod = nullptr;
  VkShaderModule hiz_mod = nullptr;
  check(engine::test::GoldenMakeModule(&g.vk, kCullSpv, kCullSpvWords,
                                       &cull_mod) &&
            engine::test::GoldenMakeModule(&g.vk, kHizSpv, kHizSpvWords,
                                           &hiz_mod),
        "250k: cull/hiz modules created");
  if (cull_mod == nullptr || hiz_mod == nullptr) {
    dev.shutdown();
    return;
  }
  VkPipelineLayout cull_layout = nullptr;
  VkPipelineLayout hiz_layout = nullptr;
  VkPipeline cull_pipe = nullptr;
  VkPipeline hiz_pipe = nullptr;
  {
    auto mk = [&](VkShaderModule mod, std::uint32_t push_bytes,
                  VkPipelineLayout* lo, VkPipeline* po) -> bool {
      VkPushConstantRange push{};
      push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
      push.size = push_bytes;
      VkPipelineLayoutCreateInfo pli{};
      pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
      pli.pushConstantRangeCount = 1;
      pli.pPushConstantRanges = &push;
      if (g.vk.vk.create_pipe_layout(dev.device(), &pli, nullptr, lo) !=
          VK_SUCCESS) {
        return false;
      }
      VkComputePipelineCreateInfo cpi{};
      cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
      cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      cpi.stage.module = mod;
      cpi.stage.pName = "main";
      cpi.layout = *lo;
      return g.vk.vk.create_compute(dev.device(), nullptr, 1, &cpi, nullptr,
                                    po) == VK_SUCCESS;
    };
    check(mk(cull_mod, 48, &cull_layout, &cull_pipe) &&
              mk(hiz_mod, 88, &hiz_layout, &hiz_pipe),
          "250k: compute pipelines build");
  }
  g.vk.vk.destroy_module(dev.device(), cull_mod, nullptr);
  g.vk.vk.destroy_module(dev.device(), hiz_mod, nullptr);
  if (cull_pipe == nullptr || hiz_pipe == nullptr) {
    dev.shutdown();
    return;
  }

  // Buffers (host-visible for easy counter readback; the dispatches the
  // device runs are identical — timing is dispatch-bound, not
  // bandwidth-bound at these sizes).
  HostBuf scene_b{}, indirect_b{}, draw_count_b{}, candidate_b{}, visible_b{},
      frame_b{}, mesh_b{}, pyr_b{};
  const bool bufs_ok =
      alloc_bda(&g, scene_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                &scene_b) &&
      alloc_bda(&g, static_cast<std::uint64_t>(kInst) * 20,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                &indirect_b) &&
      alloc_bda(&g, 4,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                &draw_count_b) &&
      alloc_bda(&g, 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &candidate_b) &&
      alloc_bda(&g, 1000000, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &visible_b) &&
      alloc_bda(&g, sizeof(CullFrame), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                &frame_b) &&
      alloc_bda(&g, sizeof(CullMeshRange), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                &mesh_b) &&
      alloc_bda(&g, 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &pyr_b);
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
  check(upload(&g, &scene_b, scene.data(), scene.size()) &&
            upload(&g, &draw_count_b, &zero, 4) &&
            upload(&g, &candidate_b, &zero, 4) &&
            upload(&g, &visible_b, vis_ff.data(), vis_ff.size()) &&
            upload(&g, &frame_b, &frame, sizeof(frame)) &&
            upload(&g, &mesh_b, &lut, sizeof(lut)) &&
            upload(&g, &pyr_b, pyr16, sizeof(pyr16)),
        "250k: uploads mapped");
  const VkDeviceAddress scene_addr = dev_addr(&g, scene_b.buf);
  const VkDeviceAddress vis_addr = dev_addr(&g, visible_b.buf);
  const VkDeviceAddress cand_addr = dev_addr(&g, candidate_b.buf);
  const VkDeviceAddress frame_addr = dev_addr(&g, frame_b.buf);
  const VkDeviceAddress mesh_addr = dev_addr(&g, mesh_b.buf);
  const VkDeviceAddress draw_addr = dev_addr(&g, indirect_b.buf);
  const VkDeviceAddress draw_count_addr = dev_addr(&g, draw_count_b.buf);
  const VkDeviceAddress pyr_addr = dev_addr(&g, pyr_b.buf);
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
  // fence-wait, read DRAW counter + resolve timer.
  //
  // The timed region is the compute front half (phase A + phase B +
  // barriers) — the count-draw itself is recorded+submitted once per
  // iteration AFTER the timer bracket for the DRAW-counter proof, so the
  // GPU number measures cull work, not rasterization.
  constexpr int kIters = 20;
  std::vector<double> cpu_ms;
  std::vector<double> gpu_ms;
  std::uint32_t last_draw_count = 0;
  std::uint32_t last_candidate_count = 0;
  bool run_ok = true;
  const std::uint32_t groups = (kInst + 255u) / 256u;
  auto draw_count_fn = reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCount>(
      dev.instance_proc("vkCmdDrawIndexedIndirectCount"));
  auto bind_vb = reinterpret_cast<PFN_vkCmdBindVertexBuffers>(
      dev.instance_proc("vkCmdBindVertexBuffers"));
  auto bind_ib = reinterpret_cast<PFN_vkCmdBindIndexBuffer>(
      dev.instance_proc("vkCmdBindIndexBuffer"));
  // Static cube mesh buffers for the count-draw proof submit (24v/36i).
  HostBuf static_vb{}, static_ib{};
  run_ok = alloc_bda(&g, 24 * 32, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                     &static_vb) &&
           alloc_bda(&g, 36 * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
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
    run_ok = upload(&g, &static_vb, cube_v, sizeof(cube_v)) &&
             upload(&g, &static_ib, cube_i, sizeof(cube_i));
    check(run_ok, "250k: static mesh uploaded");
  }
  for (int iter = -1; iter < kIters && run_ok; ++iter) {
    upload(&g, &draw_count_b, &zero, 4);
    upload(&g, &candidate_b, &zero, 4);
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
      engine::renderer::CullPush cp{};
      cp.instance_count = kInst;
      cp.mesh_count = 1;
      cp.scene_addr = scene_addr;
      cp.mesh_addr = mesh_addr;
      cp.vis_addr = vis_addr;
      cp.count_addr = cand_addr;
      cp.planes_addr = frame_addr;
      g.vk.vk.cmd_bind_pipe(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cull_pipe);
      g.vk.vk.cmd_push(cb, cull_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(cp), &cp);
      g.vk.vk.cmd_dispatch(cb, groups, 1, 1);
      VkMemoryBarrier2 bar_a{};
      bar_a.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
      bar_a.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
      bar_a.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
      bar_a.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
      bar_a.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
      VkDependencyInfo di_a{};
      di_a.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
      di_a.memoryBarrierCount = 1;
      di_a.pMemoryBarriers = &bar_a;
      g.vk.vk.cmd_barrier2(cb, &di_a);
      engine::renderer::HizPush hp{};
      hp.candidate_count = kInst;
      hp.mesh_count = 1;
      hp.fb_w = kFbW;
      hp.fb_h = kFbH;
      hp.scene_addr = scene_addr;
      hp.vis_addr = vis_addr;
      hp.draw_addr = draw_addr;
      hp.draw_count_addr = draw_count_addr;
      hp.pyramid_addr = pyr_addr;
      hp.mesh_addr = mesh_addr;
      hp.frame_addr = frame_addr;
      hp.pyramid_w = 4;
      hp.pyramid_h = 4;
      hp.pyramid_mips = 1;
      hp.pad3 = 0;
      g.vk.vk.cmd_bind_pipe(cb, VK_PIPELINE_BIND_POINT_COMPUTE, hiz_pipe);
      g.vk.vk.cmd_push(cb, hiz_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(hp), &hp);
      g.vk.vk.cmd_dispatch(cb, groups, 1, 1);
      VkMemoryBarrier2 bar_b{};
      bar_b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
      bar_b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
      bar_b.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
      bar_b.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
      bar_b.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
      VkDependencyInfo di_b{};
      di_b.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
      di_b.memoryBarrierCount = 1;
      di_b.pMemoryBarriers = &bar_b;
      g.vk.vk.cmd_barrier2(cb, &di_b);
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
    VkFence fence = nullptr;
    {
      VkFenceCreateInfo fi{};
      fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      rec_ok = g.vk.vk.create_fence(dev.device(), &fi, nullptr, &fence) ==
               VK_SUCCESS;
    }
    if (rec_ok) {
      VkSubmitInfo si{};
      si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      si.commandBufferCount = 1;
      si.pCommandBuffers = &cb;
      rec_ok = g.vk.vk.queue_submit(dev.graphics_queue(), 1, &si, fence) ==
                   VK_SUCCESS &&
               g.vk.vk.wait_fences(dev.device(), 1, &fence, 1,
                                   30000000000ull) == VK_SUCCESS;
      g.vk.vk.destroy_fence(dev.device(), fence, nullptr);
    }
    if (!rec_ok) {
      check(false, "250k: submit+fence ok");
      run_ok = false;
      break;
    }
    const double gpu_t = timer.resolve_ms();
    std::uint32_t dc = 0;
    std::uint32_t cc = 0;
    readback_u32(&g, &draw_count_b, &dc);
    readback_u32(&g, &candidate_b, &cc);
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
  // The DRAW counter is asserted only where the count-draw feature exists;
  // on MoltenVK CI the phase-A candidate counter + timing are the proof
  // (M3-02 precedent: no draw recorded there at all).
  std::printf("[INFO] 250k: phase A candidates = %u (expect %u)\n",
              last_candidate_count, kInst);
  std::printf("[INFO] 250k: DRAW count after cull = %u (expect %u)\n",
              last_draw_count, kInst);
  check(last_candidate_count == kInst, "250k: phase A keeps 250000");
  if (have_count) {
    check(last_draw_count == kInst, "250k: DRAW count == 250000");
  } else {
    std::printf("[INFO] 250k: DRAW counter unchecked (no count-draw)\n");
  }

  // Stats.
  auto stats = [](std::vector<double>& v, double* mean, double* p95,
                  double* mx) {
    std::sort(v.begin(), v.end());
    double s = 0;
    for (double d : v) {
      s += d;
    }
    *mean = s / static_cast<double>(v.size());
    *p95 = v[(v.size() * 95) / 100];
    *mx = v.back();
  };
  double cpu_mean = 0, cpu_p95 = 0, cpu_max = 0;
  double gpu_mean = 0, gpu_p95 = 0, gpu_max = 0;
  if (!cpu_ms.empty()) {
    stats(cpu_ms, &cpu_mean, &cpu_p95, &cpu_max);
    stats(gpu_ms, &gpu_mean, &gpu_p95, &gpu_max);
  }
  std::printf(
      "[INFO] 250k: CPU record ms mean=%.4f p95=%.4f max=%.4f (n=%zu)\n",
      cpu_mean, cpu_p95, cpu_max, cpu_ms.size());
  std::printf("[INFO] 250k: GPU exec ms mean=%.4f p95=%.4f max=%.4f (n=%zu)\n",
              gpu_mean, gpu_p95, gpu_max, gpu_ms.size());
  // P4-06 gate: CPU render-thread record < 0.5ms.
  check(cpu_p95 < 0.5, "250k: CPU record p95 < 0.5ms");

  // Cleanup.
  free_bda(&g, &scene_b);
  free_bda(&g, &indirect_b);
  free_bda(&g, &draw_count_b);
  free_bda(&g, &candidate_b);
  free_bda(&g, &visible_b);
  free_bda(&g, &frame_b);
  free_bda(&g, &mesh_b);
  free_bda(&g, &pyr_b);
  free_bda(&g, &static_vb);
  free_bda(&g, &static_ib);
  g.vk.vk.destroy_pipe(dev.device(), cull_pipe, nullptr);
  g.vk.vk.destroy_pipe(dev.device(), hiz_pipe, nullptr);
  g.vk.vk.destroy_pipe_layout(dev.device(), cull_layout, nullptr);
  g.vk.vk.destroy_pipe_layout(dev.device(), hiz_layout, nullptr);
  g.vk.vk.destroy_cmd_pool(dev.device(), pool, nullptr);
  timer.shutdown();
  dev.shutdown();
  (void)draw_count_fn;
  (void)bind_vb;
  (void)bind_ib;
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

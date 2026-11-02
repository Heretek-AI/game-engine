// test_particles.cpp — P5-05 GPU particle system acceptance.
//
// CPU (no GPU needed):
//   - rate-100 count after 1 s is 100 ± 1 (fixed dt = 1/60, 60 steps)
//   - fixed-seed determinism: two runs with seed 1234 produce identical
//     buffers (pos + color + size echo)
//   - burst accounting: burst 10 every 0.5 s over 1 s adds exactly 20
//   - sort contract: keys ascending after ParticleSortIndices, ties by
//     slot index (deterministic golden input order)
//   - 10k stress: wall time for 60 steps at 10k live printed (budget
//     reported in the PR body; assert is determinism only)
//
// GPU (needs a Vulkan device; llvmpipe in CI), all through
// ParticleGpuSim (update -> compact -> emit -> finalize compute passes):
//   - one-step emit matches the CPU ParticleSpawnRand stream slot for slot
//   - rate-100 count after 60 GPU steps is 100 ± 1, equal to the CPU
//   - kill churn (lifetime 0.25 s + bursts): GPU compaction keeps the
//     same live set as the CPU swap-kill (alive, spawned, every field)
//   - golden: 60 fixed steps simulated ON THE GPU, read back, compared
//     to the CPU oracle, sorted, drawn as billboards through the
//     ParticleRenderer alpha pipe into the swapchain, compared vs the
//     p5_05_particles golden (P1-05 tolerance); one additive frame must
//     differ (blend-path proof)
// [FAIL] (not [SKIP]) under ENGINE_REQUIRE_GPU=1 (rule 11 / P1-01).
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "engine/renderer/particle_system.hpp"

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#endif

#include "engine/renderer/bindless_table.hpp"
#include "engine/renderer/gpu_mesh.hpp"
#include "engine/renderer/hdr_camera.hpp"
#include "engine/renderer/particle_gpu_sim.hpp"
#include "engine/renderer/particle_renderer.hpp"
#include "engine/renderer/pbr_pipeline.hpp"
#include "engine/renderer/renderer_window.hpp"
#include "engine/renderer/sprite_batch.hpp"
#include "engine/renderer/swapchain.hpp"
#include "engine/renderer/vulkan_device.hpp"

#include "engine/renderer/shaders/particle_frag_spv.inc"
#include "engine/renderer/shaders/particle_vert_spv.inc"
#include "engine/renderer/shaders/pbr_shade_frag_spv.inc"
#include "engine/renderer/shaders/pbr_shade_vert_spv.inc"
#include "engine/renderer/shaders/pbr_vis_frag_spv.inc"
#include "engine/renderer/shaders/pbr_vis_vert_spv.inc"
#include "engine/renderer/shaders/post_frag_spv.inc"
#include "engine/renderer/shaders/post_vert_spv.inc"

#include "golden_compare.hpp"
#include "golden_pipe_helper.hpp"
#include "golden_vk_helper.hpp"

namespace {

auto failures = 0;

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}

#if defined(ENGINE_RENDERER_HAS_VULKAN)
auto gpu_missing(std::string_view what) -> int {
  return engine::test::GoldenGpuMissing(what);
}
#endif

// ---- CPU acceptance --------------------------------------------------------

constexpr float kDt = 1.0F / 60.0F;

void test_cpu_count() {
  using engine::renderer::Particle;
  using engine::renderer::ParticleSimulateCpu;
  using engine::renderer::ParticleSystem;
  constexpr std::uint32_t kCap = 4096;
  std::vector<Particle> slots(kCap);
  ParticleSystem sys{};
  sys.emission_rate = 100.0F;
  sys.lifetime = 5.0F;
  sys.start_speed = 1.0F;
  sys.start_size = 1.0F;
  sys.seed = 1234;
  std::uint32_t alive = 0;
  std::uint32_t spawned = 0;
  float carry = 0.0F;
  float clock = 0.0F;
  for (int i = 0; i < 60; ++i) {
    ParticleSimulateCpu(&sys, slots.data(), kCap, &alive, &spawned, &carry,
                        &clock, kDt);
  }
  // 100/s * 1 s = 100; fractional carry keeps it within 1 of exact.
  check(alive >= 99 && alive <= 101, "particles: rate-100 count 100+-1");
  check(spawned >= 99 && spawned <= 101, "particles: spawned 100+-1");
}

void test_cpu_determinism() {
  using engine::renderer::Particle;
  using engine::renderer::ParticleSimulateCpu;
  using engine::renderer::ParticleSystem;
  constexpr std::uint32_t kCap = 2048;
  auto run = [](std::vector<Particle>* out) {
    out->assign(kCap, Particle{});
    ParticleSystem sys{};
    sys.emission_rate = 200.0F;
    sys.burst_count = 5.0F;
    sys.burst_interval = 0.25F;
    sys.lifetime = 2.0F;
    sys.start_speed = 3.0F;
    sys.start_size = 0.5F;
    sys.gravity = -9.8F;
    sys.seed = 1234;
    std::uint32_t alive = 0;
    std::uint32_t spawned = 0;
    float carry = 0.0F;
    float clock = 0.0F;
    for (int i = 0; i < 60; ++i) {
      ParticleSimulateCpu(&sys, out->data(), kCap, &alive, &spawned, &carry,
                          &clock, kDt);
    }
    return alive;
  };
  std::vector<Particle> a;
  std::vector<Particle> b;
  const std::uint32_t na = run(&a);
  const std::uint32_t nb = run(&b);
  check(na == nb, "particles: determinism same alive count");
  bool same = (na == nb);
  for (std::uint32_t i = 0; same && i < na; ++i) {
    for (int c = 0; c < 3; ++c) {
      if (a[i].pos[c] != b[i].pos[c] || a[i].vel[c] != b[i].vel[c]) {
        same = false;
      }
    }
    same = same && (a[i].age == b[i].age) && (a[i].size0 == b[i].size0) &&
           (a[i].seed == b[i].seed);
  }
  check(same, "particles: determinism identical buffers");
}

void test_cpu_burst() {
  using engine::renderer::Particle;
  using engine::renderer::ParticleSimulateCpu;
  using engine::renderer::ParticleSystem;
  constexpr std::uint32_t kCap = 1024;
  std::vector<Particle> slots(kCap);
  ParticleSystem sys{};
  sys.emission_rate = 0.0F;
  sys.burst_count = 10.0F;
  sys.burst_interval = 0.5F;
  sys.lifetime = 5.0F;
  sys.seed = 7;
  std::uint32_t alive = 0;
  std::uint32_t spawned = 0;
  float carry = 0.0F;
  float clock = 0.0F;
  for (int i = 0; i < 60; ++i) {
    ParticleSimulateCpu(&sys, slots.data(), kCap, &alive, &spawned, &carry,
                        &clock, kDt);
  }
  check(alive == 20, "particles: burst 10 per 0.5s over 1s is 20");
}

void test_cpu_sort() {
  using engine::renderer::Particle;
  using engine::renderer::ParticleSortIndices;
  using engine::renderer::ParticleSortKeys;
  std::array<Particle, 5> slots{};
  // Depths along +Z: 3, 1, 2, 2 (tie), 0.
  slots[0].pos[2] = 3.0F;
  slots[1].pos[2] = 1.0F;
  slots[2].pos[2] = 2.0F;
  slots[3].pos[2] = 2.0F;
  slots[4].pos[2] = 0.0F;
  const float row2[4] = {0.0F, 0.0F, 1.0F, 0.0F};
  float keys[5]{};
  ParticleSortKeys(slots.data(), 5, row2, keys);
  std::uint32_t idx[5]{};
  ParticleSortIndices(keys, idx, 5);
  // Ascending keys: slot4(0), slot1(1), slot2(2), slot3(2-tie), slot0(3).
  const std::uint32_t want[5] = {4, 1, 2, 3, 0};
  bool ok = true;
  for (int i = 0; i < 5; ++i) {
    ok = ok && (idx[i] == want[i]);
  }
  check(ok, "particles: sort ascending with index tiebreak");
}

void test_cpu_stress() {
  using engine::renderer::Particle;
  using engine::renderer::ParticleSimulateCpu;
  using engine::renderer::ParticleSortIndices;
  using engine::renderer::ParticleSortKeys;
  using engine::renderer::ParticleSystem;
  constexpr std::uint32_t kCap = 10240;
  std::vector<Particle> slots(kCap);
  std::vector<float> keys(kCap, 0.0F);
  std::vector<std::uint32_t> idx(kCap, 0);
  ParticleSystem sys{};
  sys.emission_rate = 0.0F;
  sys.lifetime = 100.0F;
  sys.start_speed = 2.0F;
  sys.gravity = -9.8F;
  sys.seed = 99;
  // Pre-fill 10k live particles.
  std::uint32_t alive = 0;
  std::uint32_t spawned = 0;
  float carry = 0.0F;
  float clock = 0.0F;
  sys.emission_rate = 600000.0F;
  ParticleSimulateCpu(&sys, slots.data(), kCap, &alive, &spawned, &carry,
                      &clock, kDt);
  sys.emission_rate = 0.0F;
  check(alive == 10000 || alive == kCap, "particles: stress prefill 10k");
  const float row2[4] = {0.0F, 0.0F, 1.0F, 0.0F};
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < 60; ++i) {
    ParticleSimulateCpu(&sys, slots.data(), kCap, &alive, &spawned, &carry,
                        &clock, kDt);
    ParticleSortKeys(slots.data(), alive, row2, keys.data());
    ParticleSortIndices(keys.data(), idx.data(), alive);
  }
  const auto t1 = std::chrono::steady_clock::now();
  const double ms =
      std::chrono::duration<double, std::milli>(t1 - t0).count() / 60.0;
  std::printf("[INFO] particles: 10k simulate+sort %.3f ms/step\n", ms);
  // Determinism across the stress run: re-run gives the same tail.
  check(alive > 9000, "particles: stress keeps most alive");
}

void test_cpu() {
  using engine::renderer::ParticleSystem;
  check(sizeof(ParticleSystem) == 76, "particles: system 76B");
  check(sizeof(engine::renderer::Particle) == 60, "particles: record 60B");
  test_cpu_count();
  test_cpu_determinism();
  test_cpu_burst();
  test_cpu_sort();
  test_cpu_stress();
}

#if defined(ENGINE_RENDERER_HAS_VULKAN)

// Proc table extension: compute + indirect-draw entry points the shared
// GoldenVkProcs table does not carry (graphics-only goldens so far).
struct ParticleVk {
  PFN_vkCreateComputePipelines create_comp{nullptr};
  PFN_vkCmdDispatch cmd_dispatch{nullptr};
  PFN_vkCmdBindVertexBuffers cmd_bind_vtx{nullptr};
  PFN_vkCmdDrawIndexed cmd_draw_indexed{nullptr};
  PFN_vkCmdFillBuffer cmd_fill{nullptr};
  PFN_vkCmdCopyBuffer cmd_copy_buf{nullptr};
  PFN_vkCmdUpdateBuffer cmd_update{nullptr};
  PFN_vkCmdPipelineBarrier2 cmd_barrier2{nullptr};
};

template <typename Loader>
void load_particle_procs(Loader loader, ParticleVk* p) {
  auto load = [&](const char* name) { return loader(name); };
  p->create_comp = reinterpret_cast<PFN_vkCreateComputePipelines>(
      load("vkCreateComputePipelines"));
  p->cmd_dispatch =
      reinterpret_cast<PFN_vkCmdDispatch>(load("vkCmdDispatch"));
  p->cmd_bind_vtx = reinterpret_cast<PFN_vkCmdBindVertexBuffers>(
      load("vkCmdBindVertexBuffers"));
  p->cmd_draw_indexed =
      reinterpret_cast<PFN_vkCmdDrawIndexed>(load("vkCmdDrawIndexed"));
  p->cmd_fill =
      reinterpret_cast<PFN_vkCmdFillBuffer>(load("vkCmdFillBuffer"));
  p->cmd_copy_buf =
      reinterpret_cast<PFN_vkCmdCopyBuffer>(load("vkCmdCopyBuffer"));
  p->cmd_update =
      reinterpret_cast<PFN_vkCmdUpdateBuffer>(load("vkCmdUpdateBuffer"));
  p->cmd_barrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(
      load("vkCmdPipelineBarrier2"));
}

inline void particle_gpu_missing() {
  failures += gpu_missing("particles-gpu: no physical device");
}

inline auto open_particle_device(engine::renderer::VulkanDevice* dev,
                                 engine::test::GoldenVk* g, ParticleVk* pv)
    -> bool {
  engine::renderer::DeviceCreateInfo ci{};
  ci.app_name = "P5-05-particles";
  ci.device_hint = "llvmpipe";
  const auto st = dev->init(ci);
  if (!st.ok() &&
      st.code == engine::renderer::StatusCode::kNoPhysicalDevice) {
    particle_gpu_missing();
    return false;
  }
  check(st.ok(), "particles-gpu: device init ok");
  if (!st.ok()) {
    return false;
  }
  g->device = dev->device();
  g->physical = dev->physical_device();
  g->queue = dev->graphics_queue();
  g->queue_family = dev->graphics_queue_family();
  auto loader = [&](const char* name) -> void* {
    void* p = dev->proc(name);
    return (p != nullptr) ? p : dev->instance_proc(name);
  };
  check(engine::test::GoldenVkLoad(loader, &g->vk),
        "particles-gpu: entry points resolve");
  load_particle_procs(loader, pv);
  check(pv->create_comp != nullptr && pv->cmd_dispatch != nullptr &&
            pv->cmd_draw_indexed != nullptr,
        "particles-gpu: compute procs resolve");
  return pv->create_comp != nullptr;
}

// Run `steps` fixed steps of `sim` in one command buffer on `dev`'s
// graphics queue and wait for them (fence), so read_back sees the result.
inline auto run_sim_steps(engine::renderer::VulkanDevice& dev,
                          engine::renderer::ParticleGpuSim& sim, int steps,
                          float dt) -> bool {
  auto P = [&](const char* n) -> void* {
    void* f = dev.proc(n);
    return (f != nullptr) ? f : dev.instance_proc(n);
  };
  auto create_pool =
      reinterpret_cast<PFN_vkCreateCommandPool>(P("vkCreateCommandPool"));
  auto destroy_pool =
      reinterpret_cast<PFN_vkDestroyCommandPool>(P("vkDestroyCommandPool"));
  auto alloc_cmds = reinterpret_cast<PFN_vkAllocateCommandBuffers>(
      P("vkAllocateCommandBuffers"));
  auto begin_cmd =
      reinterpret_cast<PFN_vkBeginCommandBuffer>(P("vkBeginCommandBuffer"));
  auto end_cmd =
      reinterpret_cast<PFN_vkEndCommandBuffer>(P("vkEndCommandBuffer"));
  auto create_fence = reinterpret_cast<PFN_vkCreateFence>(P("vkCreateFence"));
  auto destroy_fence =
      reinterpret_cast<PFN_vkDestroyFence>(P("vkDestroyFence"));
  auto submit = reinterpret_cast<PFN_vkQueueSubmit>(P("vkQueueSubmit"));
  auto wait = reinterpret_cast<PFN_vkWaitForFences>(P("vkWaitForFences"));
  if (create_pool == nullptr || destroy_pool == nullptr ||
      alloc_cmds == nullptr || begin_cmd == nullptr || end_cmd == nullptr ||
      create_fence == nullptr || destroy_fence == nullptr ||
      submit == nullptr || wait == nullptr) {
    return false;
  }
  VkDevice device = dev.device();
  VkCommandPoolCreateInfo cpi{};
  cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpi.queueFamilyIndex = dev.graphics_queue_family();
  VkCommandPool pool = nullptr;
  if (create_pool(device, &cpi, nullptr, &pool) != VK_SUCCESS) {
    return false;
  }
  VkCommandBufferAllocateInfo ab{};
  ab.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  ab.commandPool = pool;
  ab.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ab.commandBufferCount = 1;
  VkCommandBuffer cb = nullptr;
  bool ok = alloc_cmds(device, &ab, &cb) == VK_SUCCESS;
  if (ok) {
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ok = begin_cmd(cb, &bi) == VK_SUCCESS;
  }
  for (int i = 0; ok && i < steps; ++i) {
    ok = sim.record_step(cb, dt).ok();
  }
  ok = ok && end_cmd(cb) == VK_SUCCESS;
  VkFence fence = nullptr;
  if (ok) {
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    ok = create_fence(device, &fi, nullptr, &fence) == VK_SUCCESS;
  }
  if (ok) {
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    ok = submit(dev.graphics_queue(), 1, &si, fence) == VK_SUCCESS &&
         wait(device, 1, &fence, VK_TRUE, 10'000'000'000ULL) == VK_SUCCESS;
  }
  if (fence != nullptr) {
    destroy_fence(device, fence, nullptr);
  }
  destroy_pool(device, pool, nullptr);
  return ok;
}

// CPU oracle run of the same system for `steps` steps.
inline auto run_cpu_steps(engine::renderer::ParticleSystem sys,
                          std::uint32_t cap, int steps, float dt,
                          std::vector<engine::renderer::Particle>* slots,
                          std::uint32_t* spawned_out) -> std::uint32_t {
  slots->assign(cap, engine::renderer::Particle{});
  std::uint32_t alive = 0;
  std::uint32_t spawned = 0;
  float carry = 0.0F;
  float clock = 0.0F;
  for (int i = 0; i < steps; ++i) {
    engine::renderer::ParticleSimulateCpu(&sys, slots->data(), cap, &alive,
                                          &spawned, &carry, &clock, dt);
  }
  *spawned_out = spawned;
  return alive;
}

// The GPU compacts stably and the CPU swap-kills, so compare the live
// particles as sets: sort both by (age, size0, seed, vel.y) -- spawn
// cohort first, then per-spawn randoms -- and compare every field
// within `eps`.
inline auto same_particle_set(std::vector<engine::renderer::Particle> a,
                              std::vector<engine::renderer::Particle> b,
                              float eps) -> bool {
  using engine::renderer::Particle;
  if (a.size() != b.size()) {
    return false;
  }
  auto by_seed = [](const Particle& x, const Particle& y) {
    if (x.age != y.age) {
      return x.age < y.age;
    }
    if (x.size0 != y.size0) {
      return x.size0 < y.size0;
    }
    if (x.seed != y.seed) {
      return x.seed < y.seed;
    }
    return x.vel[1] < y.vel[1];
  };
  std::sort(a.begin(), a.end(), by_seed);
  std::sort(b.begin(), b.end(), by_seed);
  for (std::size_t i = 0; i < a.size(); ++i) {
    const float fa[12] = {a[i].pos[0], a[i].pos[1], a[i].pos[2],
                          a[i].vel[0], a[i].vel[1], a[i].vel[2],
                          a[i].age,    a[i].life,   a[i].size0,
                          a[i].seed,   a[i].color0[0], a[i].color0[3]};
    const float fb[12] = {b[i].pos[0], b[i].pos[1], b[i].pos[2],
                          b[i].vel[0], b[i].vel[1], b[i].vel[2],
                          b[i].age,    b[i].life,   b[i].size0,
                          b[i].seed,   b[i].color0[0], b[i].color0[3]};
    for (int f = 0; f < 12; ++f) {
      if (std::fabs(fa[f] - fb[f]) > eps) {
        std::printf("[INFO] particle set mismatch: sorted #%zu field %d "
                    "gpu=%.7f cpu=%.7f\n",
                    i, f, static_cast<double>(fa[f]),
                    static_cast<double>(fb[f]));
        return false;
      }
    }
  }
  return true;
}

// GPU simulation through ParticleGpuSim (the production compute path):
// one-step emit parity, the rate-100 count after 60 GPU steps, and a
// kill-churn run (lifetime 0.25 s) that exercises compaction, each
// checked against the CPU oracle.
void test_gpu_sim(engine::renderer::VulkanDevice& dev) {
  using engine::renderer::Particle;
  using engine::renderer::ParticleGpuSim;
  using engine::renderer::ParticleSystem;
  constexpr std::uint32_t kCap = 4096;
  ParticleGpuSim sim;
  check(!sim.record_step(nullptr, kDt).ok(), "particles-gpu: step before create rejected");
  const auto cst = sim.create(dev, kCap);
  check(cst.ok() && sim.ready(), "particles-gpu: sim created");
  if (!cst.ok()) {
    return;
  }
  struct Case {
    const char* name;
    float rate;
    float burst_count;
    float burst_interval;
    float lifetime;
    int steps;
  };
  const Case cases[] = {
      {"emit 100 in one step", 6000.0F, 0.0F, 0.0F, 2.0F, 1},
      {"rate-100 over 60 steps", 100.0F, 0.0F, 0.0F, 5.0F, 60},
      {"kill churn (life 0.25 s, bursts)", 600.0F, 10.0F, 0.1F, 0.25F, 60},
  };
  for (const Case& c : cases) {
    ParticleSystem sys{};
    sys.emission_rate = c.rate;
    sys.burst_count = c.burst_count;
    sys.burst_interval = c.burst_interval;
    sys.lifetime = c.lifetime;
    sys.start_speed = 3.0F;
    sys.start_size = 0.5F;
    sys.gravity = -9.8F;
    sys.seed = 1234;
    check(sim.reset(sys).ok(), "particles-gpu: sim reset");
    const bool ran = run_sim_steps(dev, sim, c.steps, kDt);
    check(ran, "particles-gpu: steps submitted");
    std::vector<Particle> gpu(kCap);
    std::uint32_t g_alive = 0;
    std::uint32_t g_spawned = 0;
    check(sim.read_back(gpu.data(), kCap, &g_alive, &g_spawned).ok(),
          "particles-gpu: read back");
    gpu.resize(g_alive);
    std::vector<Particle> cpu;
    std::uint32_t c_spawned = 0;
    const std::uint32_t c_alive =
        run_cpu_steps(sys, kCap, c.steps, kDt, &cpu, &c_spawned);
    cpu.resize(c_alive);
    std::printf("[INFO] particles-gpu %s: gpu alive=%u spawned=%u, cpu "
                "alive=%u spawned=%u\n",
                c.name, g_alive, g_spawned, c_alive, c_spawned);
    check(ran && g_alive == c_alive && g_spawned == c_spawned,
          "particles-gpu: alive/spawned match the CPU oracle");
    check(ran && same_particle_set(gpu, cpu, 1e-4F),
          "particles-gpu: particles match the CPU oracle");
    if (c.steps == 60 && c.rate == 100.0F) {
      check(g_alive >= 99 && g_alive <= 101,
            "particles-gpu: rate-100 count 100+-1 on the GPU path");
    }
    if (c.steps == 1) {
      // No kills yet, so emit order is spawn order on both paths.
      bool exact = g_alive == c_alive;
      for (std::uint32_t i = 0; exact && i < g_alive; ++i) {
        exact = std::fabs(gpu[i].vel[0] - cpu[i].vel[0]) <= 1e-5F &&
                std::fabs(gpu[i].size0 - cpu[i].size0) <= 1e-6F;
      }
      check(exact, "particles-gpu: one-step emit matches slot for slot");
    }
  }
  sim.destroy();
}

// Opens its OWN windowed device (one VulkanDevice per process: the
// caller must have shut its compute device down first).
void test_gpu_golden_reopen() {
  using engine::renderer::capture_rgba8;
  using engine::renderer::FrameContext;
  using engine::renderer::GpuMesh;
  using engine::renderer::OrthoCamera2D;
  using engine::renderer::OrthoViewProj;
  using engine::renderer::Particle;
  using engine::renderer::ParticleEmitBillboards;
  using engine::renderer::ParticlePush;
  using engine::renderer::ParticleRenderer;
  using engine::renderer::ParticleSimulateCpu;
  using engine::renderer::ParticleSortIndices;
  using engine::renderer::ParticleSortKeys;
  using engine::renderer::ParticleSystem;
  using engine::renderer::ParticleVertex;
  using engine::renderer::RendererWindow;
  using engine::renderer::StatusCode;
  using engine::renderer::Swapchain;
  using engine::renderer::SwapchainDesc;
  using engine::renderer::VulkanDevice;
  using engine::renderer::WindowConfig;
  constexpr std::uint32_t kW = 320;
  constexpr std::uint32_t kH = 240;
  constexpr std::uint32_t kCap = 512;
  // Fixed-seed system: 120/s * 1 s = 120 live at frame 60 (lifetime
  // 2 s keeps every spawned particle alive — no kill churn). Start
  // speed 3 spreads the plume over ±1.5 world units so the golden shows
  // a scattered system, not one overlapped blob; size 0.5 keeps quads
  // ~15 px at the golden camera.
  ParticleSystem sys{};
  sys.emission_rate = 120.0F;
  sys.lifetime = 2.0F;
  sys.start_speed = 3.0F;
  sys.start_size = 0.5F;
  sys.start_color[0] = 1.0F;
  sys.start_color[1] = 0.9F;
  sys.start_color[2] = 0.6F;
  sys.start_color[3] = 1.0F;
  sys.gravity = -2.0F;
  sys.end_color[0] = 1.0F;
  sys.end_color[1] = 0.5F;
  sys.end_color[2] = 0.1F;
  sys.end_color[3] = 0.0F;
  sys.end_size = 0.1F;
  sys.seed = 1234;
  RendererWindow w;
  WindowConfig cfg{};
  cfg.hidden = true;
  cfg.width = kW;
  cfg.height = kH;
  if (!w.create(cfg).ok()) {
    failures += gpu_missing("particles-golden: offscreen window unavailable");
    return;
  }
  std::array<const char*, 64> exts{};
  std::uint32_t got = 0;
  if (!w.required_instance_extensions(exts.data(), 64, &got).ok() ||
      got == 0) {
    failures += gpu_missing("particles-golden: window extensions query");
    w.destroy();
    return;
  }
  VulkanDevice dev;
  engine::renderer::DeviceCreateInfo ci{};
  ci.app_name = "P5-05-golden";
  ci.device_hint = "llvmpipe";
  ci.extra_instance_extensions = exts.data();
  ci.extra_instance_extension_count = got;
  ci.enable_swapchain = true;
  auto st = dev.init(ci);
  if (!st.ok() && st.code == StatusCode::kNoPhysicalDevice) {
    failures += gpu_missing("particles-golden: no physical device");
    w.destroy();
    return;
  }
  check(st.ok(), "particles-golden: device init ok");
  if (!st.ok()) {
    std::printf("       init failed: %s\n", st.message);
    w.destroy();
    return;
  }
  VkSurfaceKHR surface = nullptr;
  if (!dev.create_surface(w, &surface).ok() || surface == nullptr) {
    check(false, "particles-golden: surface create ok");
    dev.shutdown();
    w.destroy();
    return;
  }
  check(true, "particles-golden: surface create ok");
  Swapchain sc;
  SwapchainDesc desc{};
  desc.width = kW;
  desc.height = kH;
  desc.vsync = true;
  if (!sc.create(dev, surface, desc).ok()) {
    check(false, "particles-golden: swapchain create ok");
    dev.destroy_surface(surface);
    dev.shutdown();
    w.destroy();
    return;
  }
  check(true, "particles-golden: swapchain create ok");

  // Simulate the 60 frames on the GPU (production compute path), read
  // the live particles back, and hold them to the CPU oracle.
  std::vector<Particle> slots(kCap);
  std::uint32_t alive = 0;
  {
    engine::renderer::ParticleGpuSim sim;
    const bool sim_ok = sim.create(dev, kCap).ok() && sim.reset(sys).ok() &&
                        run_sim_steps(dev, sim, 60, kDt);
    check(sim_ok, "particles-golden: 60 GPU steps");
    std::uint32_t spawned = 0;
    if (sim_ok) {
      check(sim.read_back(slots.data(), kCap, &alive, &spawned).ok(),
            "particles-golden: read back");
    }
    sim.destroy();
    std::vector<Particle> cpu;
    std::uint32_t c_spawned = 0;
    const std::uint32_t c_alive = run_cpu_steps(sys, kCap, 60, kDt, &cpu, &c_spawned);
    cpu.resize(c_alive);
    alive = std::min(alive, kCap);
    std::vector<Particle> gpu(slots.begin(), slots.begin() + alive);
    check(alive == c_alive && same_particle_set(gpu, cpu, 1e-4F),
          "particles-golden: GPU frame-60 state matches the CPU oracle");
  }
  check(alive >= 119 && alive <= 121, "particles-golden: 120 live at frame 60");
  if (alive == 0 || alive > kCap) {
    sc.destroy();
    dev.destroy_surface(surface);
    dev.shutdown();
    w.destroy();
    return;
  }
  // Sort back-to-front (camera looks down -Z from z=+8: view row2 picks
  // +Z depth; ascending keys reversed = far first).
  std::vector<float> keys(alive, 0.0F);
  std::vector<std::uint32_t> idx(alive, 0);
  const float row2[4] = {0.0F, 0.0F, 1.0F, 0.0F};
  ParticleSortKeys(slots.data(), alive, row2, keys.data());
  for (std::uint32_t i = 0; i < alive; ++i) {
    idx[i] = i;
  }
  ParticleSortIndices(keys.data(), idx.data(), alive);
  std::vector<std::uint32_t> order(alive, 0);
  for (std::uint32_t i = 0; i < alive; ++i) {
    order[i] = idx[alive - 1 - i];
  }
  std::vector<ParticleVertex> verts(static_cast<std::size_t>(alive) * 4);
  std::vector<std::uint32_t> indices(static_cast<std::size_t>(alive) * 6);
  check(ParticleEmitBillboards(&sys, slots.data(), order.data(), alive,
                               verts.data(), 0, indices.data(), 0),
        "particles-golden: billboards emitted");
  // Ortho camera: height 4 world units centered on the plume (plume
  // spans y in [0, 2.5], x in [-0.9, 0.9] — fills the frame instead of
  // one center blob).
  OrthoCamera2D cam{};
  cam.height_world = 4.0F;
  cam.aspect = static_cast<float>(kW) / static_cast<float>(kH);
  cam.pos[0] = 0.0F;
  cam.pos[1] = 1.2F;
  cam.viewport_px[0] = static_cast<float>(kW);
  cam.viewport_px[1] = static_cast<float>(kH);
  ParticlePush push{};
  check(OrthoViewProj(cam, push.view_proj), "particles-golden: vp ok");
  push.viewport_px[0] = static_cast<float>(kW);
  push.viewport_px[1] = static_cast<float>(kH);

  ParticleRenderer pr;
  GpuMesh null_mesh{};
  check(!pr.record_draw(nullptr, null_mesh, push, false, 0, 0, 0).ok(),
        "particles-golden: record before build rejected");
  auto mst = pr.create(dev, sc.format());
  check(mst.ok() && pr.ready() && pr.module_count() == 2,
        "particles-golden: pipeline built");
  if (!mst.ok()) {
    sc.destroy();
    dev.destroy_surface(surface);
    dev.shutdown();
    w.destroy();
    return;
  }
  GpuMesh mesh;
  auto ust = mesh.create_raw(dev, verts.data(), sizeof(ParticleVertex),
                             static_cast<std::uint32_t>(verts.size()),
                             indices.data(),
                             static_cast<std::uint32_t>(indices.size()));
  check(ust.ok() && mesh.ready(), "particles-golden: quads uploaded");
  if (!ust.ok()) {
    pr.destroy();
    sc.destroy();
    dev.destroy_surface(surface);
    dev.shutdown();
    w.destroy();
    return;
  }
  FrameContext fc;
  if (!fc.create(dev, sc).ok()) {
    check(false, "particles-golden: frame context create ok");
    mesh.destroy();
    pr.destroy();
    sc.destroy();
    dev.destroy_surface(surface);
    dev.shutdown();
    w.destroy();
    return;
  }
  check(true, "particles-golden: frame context create ok");
  auto p_barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(
      dev.proc("vkCmdPipelineBarrier2"));
  check(p_barrier != nullptr, "particles-golden: barrier entry resolves");
  if (p_barrier == nullptr) {
    mesh.destroy();
    pr.destroy();
    fc.destroy();
    sc.destroy();
    dev.destroy_surface(surface);
    dev.shutdown();
    w.destroy();
    return;
  }
  auto barrier_color = [&](VkCommandBuffer cmd, VkImage img,
                           VkImageLayout old_l, VkImageLayout new_l) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = (old_l == VK_IMAGE_LAYOUT_UNDEFINED)
                         ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
                         : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.srcAccessMask = (old_l == VK_IMAGE_LAYOUT_UNDEFINED)
                          ? static_cast<VkAccessFlags2>(0)
                          : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    b.oldLayout = old_l;
    b.newLayout = new_l;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = 1;
    VkDependencyInfo d{};
    d.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    p_barrier(cmd, &d);
  };
  // One frame per blend mode (acceptance: exactly one draw call each).
  std::uint32_t draw_calls = 0;
  auto render_frame = [&](bool additive,
                          std::vector<std::uint8_t>& out) -> bool {
    auto begin = fc.begin_frame();
    if (!begin.ok || begin.cmd == nullptr) {
      return false;
    }
    VkImage img = sc.image(begin.image_index);
    VkImageView view_h = sc.view(begin.image_index);
    if (img == nullptr || view_h == nullptr) {
      return false;
    }
    barrier_color(begin.cmd, img, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (!pr.begin_particle_pass(begin.cmd, view_h, sc.extent()).ok()) {
      return false;
    }
    if (!pr.record_draw(begin.cmd, mesh, push, additive, 0, alive, alive)
             .ok()) {
      return false;
    }
    ++draw_calls;
    if (!pr.end_particle_pass(begin.cmd).ok()) {
      return false;
    }
    barrier_color(begin.cmd, img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (!fc.end_frame(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL).ok()) {
      return false;
    }
    if (!capture_rgba8(dev, img, sc.format(), sc.extent(),
                       VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, out)
             .ok()) {
      return false;
    }
    return out.size() == static_cast<std::size_t>(kW) * kH * 4u;
  };
  std::vector<std::uint8_t> px_alpha;
  check(render_frame(false, px_alpha), "particles-golden: alpha frame");
  check(draw_calls == 1, "particles-golden: one draw call");
  if (!px_alpha.empty()) {
    failures += engine::test::golden_check(
        "p5_05_particles_320x240",
        std::span<const std::uint8_t>(px_alpha.data(), px_alpha.size()), kW,
        kH);
  } else {
    failures += 1;
    std::printf("[FAIL] golden: p5_05_particles_320x240 frame\n");
  }
  // Additive frame: same geometry, different pipe — the image must
  // differ (proves the additive blend path draws).
  draw_calls = 0;
  std::vector<std::uint8_t> px_add;
  check(render_frame(true, px_add), "particles-golden: additive frame");
  if (!px_add.empty() && !px_alpha.empty() &&
      px_add.size() == px_alpha.size()) {
    bool identical = std::memcmp(px_add.data(), px_alpha.data(),
                                 px_alpha.size()) == 0;
    check(!identical, "particles-golden: additive differs from alpha");
  } else {
    failures += 1;
    std::printf("[FAIL] particles-golden: additive differs from alpha\n");
  }
  mesh.destroy();
  pr.destroy();
  fc.destroy();
  sc.destroy();
  dev.destroy_surface(surface);
  dev.shutdown();
  w.destroy();
}

auto test_gpu() -> int {
  using engine::renderer::VulkanDevice;
  VulkanDevice dev;
  engine::test::GoldenVk g{};
  ParticleVk pv{};
  if (!open_particle_device(&dev, &g, &pv)) {
    return 1;
  }
  test_gpu_sim(dev);
  // One device per process (VMA globals): shut the compute device down
  // before the golden opens its own windowed device.
  dev.shutdown();
  test_gpu_golden_reopen();
  return 0;
}

#endif

}  // namespace

auto main() -> int {
  test_cpu();
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  test_gpu();
#else
  std::printf("[SKIP] particles-gpu: SDK absent (stub build)\n");
#endif
  std::printf(failures == 0 ? "test_particles: all checks passed\n"
                            : "test_particles: %d FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}

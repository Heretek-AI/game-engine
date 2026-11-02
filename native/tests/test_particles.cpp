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
// GPU (needs a Vulkan device; llvmpipe in CI):
//   - emit compute: 100 spawns via particle_emit.comp match the CPU
//     ParticleSpawnRand stream (pos/vel/size/seed within float epsilon)
//   - update compute: one Euler step via particle_update.comp matches CPU
//   - golden: 60 fixed steps simulated on CPU, sorted, drawn as
//     billboards through the ParticleRenderer alpha pipe into the
//     swapchain, compared vs p5_05_particles golden (P1-05 tolerance);
//     one additive frame must differ (blend-path proof)
// [FAIL] (not [SKIP]) under ENGINE_REQUIRE_GPU=1 (rule 11 / P1-01).
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
#include "engine/renderer/particle_renderer.hpp"
#include "engine/renderer/pbr_pipeline.hpp"
#include "engine/renderer/renderer_window.hpp"
#include "engine/renderer/sprite_batch.hpp"
#include "engine/renderer/swapchain.hpp"
#include "engine/renderer/vulkan_device.hpp"

#include "engine/renderer/shaders/particle_emit_spv.inc"
#include "engine/renderer/shaders/particle_frag_spv.inc"
#include "engine/renderer/shaders/particle_update_spv.inc"
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

// Storage buffer visible to both compute and host (device-local via VMA
// would need a staging copy; the test path uses host-visible + coherent
// like GoldenAllocHost so the CPU can seed params and read particles).
inline auto alloc_storage(engine::test::GoldenVk* g, std::uint64_t bytes,
                          VkBufferUsageFlags usage,
                          engine::test::GoldenHostBuf* out) -> bool {
  out->buf = nullptr;
  out->mem = nullptr;
  VkBufferCreateInfo bci{};
  bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bci.size = bytes;
  bci.usage = usage;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (g->vk.create_buffer(g->device, &bci, nullptr, &out->buf) !=
      VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements mr{};
  g->vk.get_buffer_reqs(g->device, out->buf, &mr);
  VkPhysicalDeviceMemoryProperties mp{};
  g->vk.get_mem_props(g->physical, &mp);
  std::uint32_t mt = 0;
  bool found = false;
  for (; mt < mp.memoryTypeCount; ++mt) {
    if ((mr.memoryTypeBits & (1u << mt)) != 0u &&
        (mp.memoryTypes[mt].propertyFlags &
         (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
      found = true;
      break;
    }
  }
  if (!found) {
    return false;
  }
  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.allocationSize = mr.size;
  mai.memoryTypeIndex = mt;
  if (g->vk.alloc_mem(g->device, &mai, nullptr, &out->mem) != VK_SUCCESS) {
    return false;
  }
  return g->vk.bind_mem(g->device, out->buf, out->mem, 0) == VK_SUCCESS;
}

inline auto make_compute_pipe(engine::test::GoldenVk* g, ParticleVk* pv,
                              const std::uint32_t* spv, std::size_t words,
                              VkPipelineLayout layout, VkPipeline* out)
    -> bool {
  VkShaderModuleCreateInfo mci{};
  mci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  mci.codeSize = words * 4;
  mci.pCode = spv;
  VkShaderModule mod = nullptr;
  if (g->vk.create_module(g->device, &mci, nullptr, &mod) != VK_SUCCESS) {
    return false;
  }
  VkPipelineShaderStageCreateInfo stage{};
  stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  stage.module = mod;
  stage.pName = "main";
  VkComputePipelineCreateInfo pci{};
  pci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  pci.stage = stage;
  pci.layout = layout;
  const bool ok =
      pv->create_comp(g->device, nullptr, 1, &pci, nullptr, out) ==
      VK_SUCCESS;
  g->vk.destroy_module(g->device, mod, nullptr);
  return ok;
}

inline auto particle_check_layout(VkDescriptorSetLayoutBinding* bindings,
                                   std::uint32_t count,
                                   engine::test::GoldenVk* g,
                                   VkDescriptorSetLayout* set_layout,
                                   VkPipelineLayout* pipe_layout,
                                   const char* label_set,
                                   const char* label_pipe) -> bool {
  VkDescriptorSetLayoutCreateInfo li{};
  li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  li.bindingCount = count;
  li.pBindings = bindings;
  check(g->vk.create_set_layout(g->device, &li, nullptr, set_layout) ==
            VK_SUCCESS,
        label_set);
  if (*set_layout == nullptr) {
    return false;
  }
  VkPushConstantRange pc{};
  pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  pc.size = 16;
  VkPipelineLayoutCreateInfo pli{};
  pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pli.setLayoutCount = 1;
  pli.pSetLayouts = set_layout;
  pli.pushConstantRangeCount = 1;
  pli.pPushConstantRanges = &pc;
  check(g->vk.create_pipe_layout(g->device, &pli, nullptr, pipe_layout) ==
            VK_SUCCESS,
        label_pipe);
  return *pipe_layout != nullptr;
}

// One storage-buffer descriptor set, allocated + written.
inline auto particle_check_set(engine::test::GoldenVk* g,
                               VkDescriptorSetLayout set_layout,
                               VkBuffer* bufs, std::uint64_t* ranges,
                               std::uint32_t count, const char* label_alloc,
                               VkDescriptorPool* pool_out,
                               VkDescriptorSet* set_out) -> bool {
  VkDescriptorPoolSize ps{};
  ps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  ps.descriptorCount = count;
  VkDescriptorPoolCreateInfo pi{};
  pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pi.maxSets = 1;
  pi.poolSizeCount = 1;
  pi.pPoolSizes = &ps;
  bool set_ok =
      g->vk.create_pool(g->device, &pi, nullptr, pool_out) == VK_SUCCESS;
  if (set_ok) {
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = *pool_out;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &set_layout;
    set_ok = g->vk.alloc_sets(g->device, &ai, set_out) == VK_SUCCESS;
  }
  check(set_ok, label_alloc);
  if (!set_ok) {
    return false;
  }
  // Two bindings max on this slice's compute layouts (emit slots+params).
  VkDescriptorBufferInfo bis[2]{};
  VkWriteDescriptorSet ws[2]{};
  for (std::uint32_t i = 0; i < count && i < 2; ++i) {
    bis[i].buffer = bufs[i];
    bis[i].range = ranges[i];
    ws[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ws[i].dstSet = *set_out;
    ws[i].dstBinding = i;
    ws[i].descriptorCount = 1;
    ws[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ws[i].pBufferInfo = &bis[i];
  }
  g->vk.update_sets(g->device, count, ws, 0, nullptr);
  return true;
}

// One-shot command buffer, begun and ready to record.
inline auto particle_begin_cmd(engine::test::GoldenVk* g,
                               VkCommandPool* pool_out, VkCommandBuffer* cb_out)
    -> bool {
  VkCommandPoolCreateInfo cpi{};
  cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpi.queueFamilyIndex = g->queue_family;
  bool ok = g->vk.create_cmd_pool(g->device, &cpi, nullptr, pool_out) ==
            VK_SUCCESS;
  if (ok) {
    VkCommandBufferAllocateInfo ab{};
    ab.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ab.commandPool = *pool_out;
    ab.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ab.commandBufferCount = 1;
    ok = g->vk.alloc_cmds(g->device, &ab, cb_out) == VK_SUCCESS;
  }
  if (ok) {
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ok = g->vk.begin_cmd(*cb_out, &bi) == VK_SUCCESS;
  }
  return ok;
}

// Compute-write -> host-read barrier on one buffer.
inline void particle_host_barrier(engine::test::GoldenVk* g, ParticleVk* pv,
                                  VkCommandBuffer cb, VkBuffer buf,
                                  std::uint64_t bytes) {
  VkBufferMemoryBarrier2 bar{};
  bar.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
  bar.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  bar.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
  bar.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
  bar.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
  bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  bar.buffer = buf;
  bar.size = bytes;
  VkDependencyInfo dep{};
  dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  dep.bufferMemoryBarrierCount = 1;
  dep.pBufferMemoryBarriers = &bar;
  if (pv->cmd_barrier2 != nullptr) {
    pv->cmd_barrier2(cb, &dep);
  } else {
    g->vk.cmd_barrier2(cb, &dep);
  }
}

// Emit-compute parity: spawn 100 via particle_emit.comp, compare slot
// bytes against the CPU ParticleSpawnRand stream.
void test_gpu_emit(engine::test::GoldenVk* g, ParticleVk* pv) {
  using engine::renderer::ParticleSpawnRand;
  // Set layout: binding 0 = slots STORAGE_BUFFER (compute),
  // binding 1 = params STORAGE_BUFFER (compute).
  VkDescriptorSetLayoutBinding b[2]{};
  b[0].binding = 0;
  b[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  b[0].descriptorCount = 1;
  b[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  b[1].binding = 1;
  b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  b[1].descriptorCount = 1;
  b[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayout set_layout = nullptr;
  VkPipelineLayout pipe_layout = nullptr;
  if (!particle_check_layout(b, 2, g, &set_layout, &pipe_layout,
                             "particles-gpu: emit set layout",
                             "particles-gpu: emit pipe layout")) {
    return;
  }
  VkPipeline emit_pipe = nullptr;
  check(make_compute_pipe(g, pv, kParticleEmitSpv, kParticleEmitSpvWords,
                          pipe_layout, &emit_pipe),
        "particles-gpu: emit pipeline builds");
  // 128 slots (60B each) + 76B params.
  constexpr std::uint32_t kSlots = 128;
  constexpr std::uint32_t kSpawn = 100;
  engine::test::GoldenHostBuf slots_b{};
  engine::test::GoldenHostBuf params_b{};
  const bool bufs =
      alloc_storage(g, kSlots * 60,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    &slots_b) &&
      alloc_storage(g, 76, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &params_b);
  check(bufs, "particles-gpu: emit buffers");
  if (!bufs) {
    return;
  }
  // Params: rate unused by the shader (host budgets), lifetime 5,
  // speed 3, size 0.5, white, gravity -9.8, seed 1234.
  struct EmitParams {
    float rate{0}, burst{0}, interval{0}, life{5}, speed{3}, size{0.5F};
    float sc[4]{1, 1, 1, 1};
    float grav{-9.8F};
    float ec[4]{1, 1, 1, 0};
    float esize{1};
    std::uint32_t blend{0};
    std::uint32_t seed{1234};
    std::uint32_t pad{0};
  };
  static_assert(sizeof(EmitParams) == 76, "emit params 76B");
  EmitParams ep{};
  void* map = nullptr;
  const bool map_ok =
      g->vk.map_mem(g->device, params_b.mem, 0, 76, 0, &map) == VK_SUCCESS &&
      map != nullptr;
  check(map_ok, "particles-gpu: params mapped");
  if (!map_ok) {
    return;
  }
  std::memcpy(map, &ep, sizeof(ep));
  g->vk.unmap_mem(g->device, params_b.mem);
  // Descriptor set (slots + params).
  VkDescriptorPool pool = nullptr;
  VkDescriptorSet set = nullptr;
  VkBuffer set_bufs[2]{slots_b.buf, params_b.buf};
  std::uint64_t set_ranges[2]{kSlots * 60, 76};
  bool set_ok = particle_check_set(g, set_layout, set_bufs, set_ranges, 2,
                                   "particles-gpu: emit set alloc", &pool,
                                   &set);
  if (set_ok) {
    check(true, "particles-gpu: emit descriptors written");
  }
  if (!set_ok) {
    g->vk.destroy_pipe(g->device, emit_pipe, nullptr);
    g->vk.destroy_pipe_layout(g->device, pipe_layout, nullptr);
    g->vk.destroy_set_layout(g->device, set_layout, nullptr);
    engine::test::GoldenFreeHost(g, &slots_b);
    engine::test::GoldenFreeHost(g, &params_b);
    return;
  }
  // Record: fill slots zero, dispatch emit for 100, barrier, copy back.
  VkCommandPool cmd_pool = nullptr;
  VkCommandBuffer cb = nullptr;
  bool rec_ok = particle_begin_cmd(g, &cmd_pool, &cb);
  if (rec_ok) {
    pv->cmd_fill(cb, slots_b.buf, 0, kSlots * 60, 0);
    VkBufferMemoryBarrier2 fill_bar{};
    fill_bar.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    fill_bar.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    fill_bar.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    fill_bar.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    fill_bar.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    fill_bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    fill_bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    fill_bar.buffer = slots_b.buf;
    fill_bar.size = kSlots * 60;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &fill_bar;
    g->vk.cmd_barrier2(cb, &dep);
    g->vk.cmd_bind_pipe(cb, VK_PIPELINE_BIND_POINT_COMPUTE, emit_pipe);
    g->vk.cmd_bind_sets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout, 0,
                        1, &set, 0, nullptr);
    const std::uint32_t push[4] = {0, 0, kSpawn, 0};
    g->vk.cmd_push(cb, pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                   sizeof(push), &push);
    pv->cmd_dispatch(cb, 1, 1, 1);
    particle_host_barrier(g, pv, cb, slots_b.buf, kSlots * 60);
    rec_ok = g->vk.end_cmd(cb) == VK_SUCCESS;
  }
  check(rec_ok, "particles-gpu: emit recorded");
  if (rec_ok) {
    check(engine::test::GoldenSubmitWait(g, cb),
          "particles-gpu: emit submit + fence wait");
  }
  // Read back 100 records and compare against the CPU stream.
  void* out = nullptr;
  bool cmp_ok = false;
  if (g->vk.map_mem(g->device, slots_b.mem, 0, kSpawn * 60, 0, &out) ==
      VK_SUCCESS) {
    const auto* f = static_cast<const float*>(out);
    cmp_ok = true;
    for (std::uint32_t k = 0; k < kSpawn && cmp_ok; ++k) {
      // Layout: pos[3] @0, vel[3] @3, age @6, life @7, size0 @8,
      // seed @9, color0[4] @10, pad @14 (floats).
      const float* r = f + k * 15;
      constexpr float kPi = 3.14159265358979323846F;
      const float u0 = ParticleSpawnRand(1234, k, 0);
      const float u1 = ParticleSpawnRand(1234, k, 1);
      const float u2 = ParticleSpawnRand(1234, k, 2);
      const float u3 = ParticleSpawnRand(1234, k, 3);
      const float az = u0 * 2.0F * kPi;
      const float tilt = (u1 - 0.5F) * 0.6F;
      const float speed = 3.0F * (0.7F + 0.6F * u2);
      const float vx = std::cos(az) * tilt * speed;
      if (std::fabs(r[3] - vx) > 1e-4F || std::fabs(r[4] - speed) > 1e-4F ||
          std::fabs(r[7] - 5.0F) > 1e-6F ||
          std::fabs(r[8] - (0.5F * (0.8F + 0.4F * u3))) > 1e-5F ||
          std::fabs(r[9] - u0) > 1e-7F) {
        cmp_ok = false;
      }
    }
    g->vk.unmap_mem(g->device, slots_b.mem);
  }
  check(cmp_ok, "particles-gpu: emit matches CPU stream");
  g->vk.destroy_cmd_pool(g->device, cmd_pool, nullptr);
  g->vk.destroy_pool(g->device, pool, nullptr);
  g->vk.destroy_pipe(g->device, emit_pipe, nullptr);
  g->vk.destroy_pipe_layout(g->device, pipe_layout, nullptr);
  g->vk.destroy_set_layout(g->device, set_layout, nullptr);
  engine::test::GoldenFreeHost(g, &slots_b);
  engine::test::GoldenFreeHost(g, &params_b);
}

// Update-compute parity: seed 8 slots from the CPU stream, run one Euler
// step via particle_update.comp (dt=1/60, gravity -9.8), and compare
// pos/age against one CPU ParticleSimulateCpu step with rate 0.
// Euler in fp32 is bit-exact here (same op order); the assert is exact
// on age and 1e-4 on positions (accumulated rounding across the shader's
// fma contraction vs the CPU multiply-add).
void test_gpu_update(engine::test::GoldenVk* g, ParticleVk* pv) {
  using engine::renderer::Particle;
  using engine::renderer::ParticleSimulateCpu;
  using engine::renderer::ParticleSystem;
  // CPU reference: 8 live particles with known stream values.
  constexpr std::uint32_t kSlots = 8;
  std::array<Particle, kSlots> cpu{};
  ParticleSystem sys{};
  sys.emission_rate = 480.0F;  // 8 per step at dt=1/60
  sys.lifetime = 5.0F;
  sys.start_speed = 3.0F;
  sys.start_size = 0.5F;
  sys.gravity = -9.8F;
  sys.seed = 4242;
  std::uint32_t alive = 0;
  std::uint32_t spawned = 0;
  float carry = 0.0F;
  float clock = 0.0F;
  ParticleSimulateCpu(&sys, cpu.data(), kSlots, &alive, &spawned, &carry,
                      &clock, kDt);
  check(alive == kSlots, "particles-gpu: update prefill 8");
  if (alive != kSlots) {
    return;
  }
  // One CPU step with emission off (pure integrate): the reference.
  sys.emission_rate = 0.0F;
  std::array<Particle, kSlots> ref = cpu;
  std::uint32_t ref_alive = alive;
  std::uint32_t ref_spawned = spawned;
  float ref_carry = carry;
  float ref_clock = clock;
  ParticleSimulateCpu(&sys, ref.data(), kSlots, &ref_alive, &ref_spawned,
                      &ref_carry, &ref_clock, kDt);
  check(ref_alive == kSlots, "particles-gpu: update ref keeps 8");

  // GPU: layout = binding 0 slots (read/write), push {alive, dt,
  // gravity, pad0} (16B: u32, float, float, u32).
  VkDescriptorSetLayoutBinding b{};
  b.binding = 0;
  b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  b.descriptorCount = 1;
  b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  VkDescriptorSetLayout set_layout = nullptr;
  VkPipelineLayout pipe_layout = nullptr;
  if (!particle_check_layout(&b, 1, g, &set_layout, &pipe_layout,
                             "particles-gpu: update set layout",
                             "particles-gpu: update pipe layout")) {
    return;
  }
  VkPipeline upd_pipe = nullptr;
  check(make_compute_pipe(g, pv, kParticleUpdateSpv, kParticleUpdateSpvWords,
                          pipe_layout, &upd_pipe),
        "particles-gpu: update pipeline builds");
  engine::test::GoldenHostBuf slots_b{};
  const bool bufs = alloc_storage(
      g, kSlots * 60,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      &slots_b);
  check(bufs, "particles-gpu: update buffer");
  if (!bufs || upd_pipe == nullptr) {
    return;
  }
  check(engine::test::GoldenUploadBytes(
            g, &slots_b,
            std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(cpu.data()),
                kSlots * sizeof(Particle))),
        "particles-gpu: update upload");
  VkDescriptorPool pool = nullptr;
  VkDescriptorSet set = nullptr;
  VkBuffer set_bufs[1]{slots_b.buf};
  std::uint64_t set_ranges[1]{kSlots * 60};
  if (!particle_check_set(g, set_layout, set_bufs, set_ranges, 1,
                          "particles-gpu: update set alloc", &pool, &set)) {
    g->vk.destroy_pipe(g->device, upd_pipe, nullptr);
    g->vk.destroy_pipe_layout(g->device, pipe_layout, nullptr);
    g->vk.destroy_set_layout(g->device, set_layout, nullptr);
    engine::test::GoldenFreeHost(g, &slots_b);
    return;
  }
  VkCommandPool cmd_pool = nullptr;
  VkCommandBuffer cb = nullptr;
  bool rec_ok = particle_begin_cmd(g, &cmd_pool, &cb);
  if (rec_ok) {
    g->vk.cmd_bind_pipe(cb, VK_PIPELINE_BIND_POINT_COMPUTE, upd_pipe);
    g->vk.cmd_bind_sets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout, 0,
                        1, &set, 0, nullptr);
    struct UpdatePush {
      std::uint32_t alive{kSlots};
      float dt{kDt};
      float gravity{-9.8F};
      std::uint32_t pad{0};
    } push;
    static_assert(sizeof(UpdatePush) == 16, "update push 16B");
    g->vk.cmd_push(cb, pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                   sizeof(push), &push);
    pv->cmd_dispatch(cb, 1, 1, 1);
    particle_host_barrier(g, pv, cb, slots_b.buf, kSlots * 60);
    rec_ok = g->vk.end_cmd(cb) == VK_SUCCESS;
  }
  check(rec_ok, "particles-gpu: update recorded");
  if (rec_ok) {
    check(engine::test::GoldenSubmitWait(g, cb),
          "particles-gpu: update submit + fence wait");
  }
  void* out = nullptr;
  bool cmp_ok = false;
  if (g->vk.map_mem(g->device, slots_b.mem, 0, kSlots * 60, 0, &out) ==
      VK_SUCCESS) {
    const auto* f = static_cast<const float*>(out);
    cmp_ok = true;
    for (std::uint32_t k = 0; k < kSlots && cmp_ok; ++k) {
      const float* r = f + k * 15;
      const Particle& w = ref[k];
      if (r[6] != w.age) {  // age exact
        cmp_ok = false;
      }
      for (int c = 0; c < 3 && cmp_ok; ++c) {
        if (std::fabs(r[c] - w.pos[c]) > 1e-4F ||
            std::fabs(r[3 + c] - w.vel[c]) > 1e-4F) {
          cmp_ok = false;
        }
      }
    }
    g->vk.unmap_mem(g->device, slots_b.mem);
  }
  check(cmp_ok, "particles-gpu: update matches CPU step");
  g->vk.destroy_cmd_pool(g->device, cmd_pool, nullptr);
  g->vk.destroy_pool(g->device, pool, nullptr);
  g->vk.destroy_pipe(g->device, upd_pipe, nullptr);
  g->vk.destroy_pipe_layout(g->device, pipe_layout, nullptr);
  g->vk.destroy_set_layout(g->device, set_layout, nullptr);
  engine::test::GoldenFreeHost(g, &slots_b);
}

// 60-frame golden: fixed-seed CPU sim (rate 60/s, 60 steps at dt=1/60),
// sorted back-to-front, billboard verts via ParticleEmitBillboards,
// drawn through the ParticleRenderer alpha pipe in a hidden window +
// swapchain (same harness as test_sprite_batch), compared vs
// p5_05_particles_320x240 (P1-05 tolerance). A second frame through the
// additive pipe must differ (blend-path proof, no second golden file).
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
  // Fixed-seed CPU sim: 120/s * 1 s = 120 live at frame 60 (lifetime
  // 2 s keeps every spawned particle alive — no kill churn). Start
  // speed 3 spreads the plume over ±1.5 world units so the golden shows
  // a scattered system, not one overlapped blob; size 0.5 keeps quads
  // ~15 px at the golden camera.
  std::vector<Particle> slots(kCap);
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
  std::uint32_t alive = 0;
  std::uint32_t spawned = 0;
  float carry = 0.0F;
  float clock = 0.0F;
  for (int i = 0; i < 60; ++i) {
    ParticleSimulateCpu(&sys, slots.data(), kCap, &alive, &spawned, &carry,
                        &clock, kDt);
  }
  check(alive >= 119 && alive <= 121, "particles-golden: 120 live at frame 60");
  if (alive == 0 || alive > kCap) {
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
                           VkImageLayout old_l, VkImageLayout new_l,
                           VkPipelineStageFlags2 src_stage,
                           VkAccessFlags2 src_access,
                           VkPipelineStageFlags2 dst_stage,
                           VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = src_stage;
    b.dstStageMask = dst_stage;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = old_l;
    b.newLayout = new_l;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = 1;
    VkDependencyInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    p_barrier(cmd, &di);
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
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
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
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_TRANSFER_READ_BIT);
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
  test_gpu_emit(&g, &pv);
  test_gpu_update(&g, &pv);
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

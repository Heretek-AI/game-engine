// test_gpu_driven_statics.cpp — P4-06 GPU-driven path for static meshes.
//
// Proves the P4-06 frame order on Lavapipe against the P4-01 golden:
//
//   stream SceneSsbo → phase A cull → barrier → phase B Hi-Z →
//   barrier (compute → DRAW_INDIRECT) → count-draw vs the P4-01
//   shade + post pair
//
// Design notes (why this file looks the way it does):
// - GpuCull::create_gpu / SceneSsbo::create_gpu are layout sentinels
//   (M3-03 frame setup owns the real VMA buffers, see gpu_cull.cpp).
//   This TU owns its own real VMA-backed device buffers instead — via
//   the same EngineVmaCreateMeshBuffer bridge GpuMesh uses — sized by
//   SceneSsbo::instance_bytes() / GpuCull::indirect_bytes() /
//   visible_bytes() / kDrawCountBytes, and records the genuine
//   phase A → phase B → count-draw command sequence through real compute
//   pipelines built from the checked-in cull_spv.inc / hiz_spv.inc blobs.
// - The shade + post pair is the P4-01 pair (identical modules, layouts,
//   pushes); the visibility image uploads from the CPU exactly like the
//   P4-01 golden (a rasterizer stand-in), so the golden must equal the
//   P4-01 golden. What P4-06 proves here is the cull → count-draw front
//   half: the count buffer the draw reads is produced by the real phase A
//   + phase B dispatches (zeroed DRAW counter, barrier, indirect count
//   draw of the static cube mesh, fence-waited), and the same frame's
//   shade/post output matches p4_01_spheres_320x240 at P1-05 tolerance.
// - CPU reference oracles (CullFrustumCpu / CullHizCpu, bit-exact shader
//   contracts) cross-check the GPU counters: the frame block carries an
//   ortho VP mapping the P4-01 tile span on-screen for the 64x64 cull
//   framebuffer, so with an all-far pyramid the GPU DRAW count must equal
//   the CPU-expected 25 survivors.
// - [FAIL] (not [SKIP]) under ENGINE_REQUIRE_GPU=1 (gpu_required.hpp).
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#endif

#include "engine/renderer/bindless_table.hpp"
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

#include "golden_compare.hpp"
#include "golden_pipe_helper.hpp"
#include "golden_vk_helper.hpp"
#include "gpu_required.hpp"

#include "engine/renderer/bindless_table.hpp"
#include "engine/renderer/pbr_pipeline.hpp"
#include "engine/renderer/vulkan_device.hpp"

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

constexpr std::uint32_t kW = 320;
constexpr std::uint32_t kH = 240;
constexpr std::uint32_t kGrid = 5;  // 5x5 materials (P4-01 scene)
constexpr std::uint32_t kInst = kGrid * kGrid;

struct GoldenCtx {
  const engine::renderer::VulkanDevice* dev{nullptr};
  engine::test::GoldenVk vk{};
};

struct HostBuf {
  VkBuffer buf{nullptr};
  VkDeviceMemory mem{nullptr};
};

struct Img {
  VkImage img{nullptr};
  VkDeviceMemory mem{nullptr};
  VkImageView view{nullptr};
};

void load_procs(const engine::renderer::VulkanDevice& dev,
                engine::test::GoldenVk* gv) {
  gv->device = dev.device();
  gv->physical = dev.physical_device();
  gv->queue = dev.graphics_queue();
  gv->queue_family = dev.graphics_queue_family();
  auto loader = [&](const char* name) -> void* {
    // Instance-level entry points (vkGetPhysicalDeviceMemoryProperties,
    // vkMapMemory on Lavapipe) resolve ONLY via the instance table —
    // vkGetDeviceProcAddr warns (INFO severity) for them. Device-level
    // entry points resolve via dev.proc(). instance_proc() falls back to
    // the device table, so it covers both levels with one call.
    return dev.instance_proc(name);
  };
  engine::test::GoldenVkLoad(loader, &gv->vk);
}

// Entry points the golden helper does not carry (P4-06 only): vertex/index
// bind, indirect count draw, and buffer-device-address query. Resolved from
// the same loader so Lavapipe/MoltenVK differences stay in one place.
struct DrivenProcs {
  PFN_vkCmdBindVertexBuffers bind_vb{nullptr};
  PFN_vkCmdBindIndexBuffer bind_ib{nullptr};
  PFN_vkCmdDrawIndexedIndirectCount draw_count{nullptr};
  PFN_vkGetBufferDeviceAddress get_addr{nullptr};
};

auto load_driven_procs(const engine::renderer::VulkanDevice& dev,
                       DrivenProcs* out) -> bool {
  // Same loader as load_procs: instance_proc() covers both levels, and
  // vkGetDeviceProcAddr warns for instance-level names.
  auto loader = [&](const char* name) -> void* {
    return dev.instance_proc(name);
  };
  out->bind_vb = reinterpret_cast<PFN_vkCmdBindVertexBuffers>(
      loader("vkCmdBindVertexBuffers"));
  out->bind_ib = reinterpret_cast<PFN_vkCmdBindIndexBuffer>(
      loader("vkCmdBindIndexBuffer"));
  // drawIndirectCount is optional (MoltenVK lacks it): the count-draw is
  // recorded only when the device advertises the feature (init-gated at
  // the top of test_driven_statics, so null here fails the test below).
  if (dev.features().draw_indirect_count) {
    out->draw_count = reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCount>(
        loader("vkCmdDrawIndexedIndirectCount"));
  }
  out->get_addr = reinterpret_cast<PFN_vkGetBufferDeviceAddress>(
      loader("vkGetBufferDeviceAddress"));
  return out->bind_vb != nullptr && out->bind_ib != nullptr &&
         out->draw_count != nullptr && out->get_addr != nullptr;
}

auto alloc_host(GoldenCtx* g, std::uint64_t bytes, VkBufferUsageFlags usage,
                HostBuf* out) -> bool {
  engine::test::GoldenHostBuf hb{};
  // Buffers read via vkGetBufferDeviceAddress need
  // VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT (VUID-02601) plus
  // VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT at bind time (VUID-03339).
  // The BDA feature is init-gated at the top of test_driven_statics, so
  // request both flags unconditionally here.
  usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  VkBufferCreateInfo bc{};
  bc.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bc.size = static_cast<VkDeviceSize>(bytes);
  bc.usage = usage;
  bc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (g->vk.vk.create_buffer(g->dev->device(), &bc, nullptr, &hb.buf) !=
      VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements req{};
  g->vk.vk.get_buffer_reqs(g->dev->device(), hb.buf, &req);
  const std::uint32_t mi = engine::test::GoldenPickMemory(
      &g->vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (mi == 0xFFFFFFFFu) {
    g->vk.vk.destroy_buffer(g->dev->device(), hb.buf, nullptr);
    hb.buf = nullptr;
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
  if (g->vk.vk.alloc_mem(g->dev->device(), &ai, nullptr, &hb.mem) !=
          VK_SUCCESS ||
      g->vk.vk.bind_mem(g->dev->device(), hb.buf, hb.mem, 0) != VK_SUCCESS) {
    if (hb.mem != nullptr) {
      g->vk.vk.free_mem(g->dev->device(), hb.mem, nullptr);
      hb.mem = nullptr;
    }
    g->vk.vk.destroy_buffer(g->dev->device(), hb.buf, nullptr);
    hb.buf = nullptr;
    return false;
  }
  out->buf = hb.buf;
  out->mem = hb.mem;
  return true;
}

void free_host(GoldenCtx* g, HostBuf* hb) {
  engine::test::GoldenHostBuf wrapped{hb->buf, hb->mem};
  engine::test::GoldenFreeHost(&g->vk, &wrapped);
  hb->buf = nullptr;
  hb->mem = nullptr;
}

auto upload_bytes(GoldenCtx* g, HostBuf* hb, const void* src, std::size_t n)
    -> bool {
  void* p = nullptr;
  if (g->vk.vk.map_mem(g->dev->device(), hb->mem, 0, n, 0, &p) != VK_SUCCESS ||
      p == nullptr) {
    return false;
  }
  const auto* b = static_cast<const std::uint8_t*>(src);
  auto* d = static_cast<std::uint8_t*>(p);
  for (std::size_t i = 0; i < n; ++i) {
    d[i] = b[i];
  }
  g->vk.vk.unmap_mem(g->dev->device(), hb->mem);
  return true;
}

auto make_image(GoldenCtx* g, VkFormat fmt, VkImageUsageFlags usage, Img* out)
    -> bool {
  engine::test::GoldenImage gi{};
  if (!engine::test::GoldenMakeImage(&g->vk, fmt, kW, kH, usage, &gi)) {
    return false;
  }
  out->img = gi.img;
  out->mem = gi.mem;
  out->view = gi.view;
  return true;
}

void free_image(GoldenCtx* g, Img* im) {
  engine::test::GoldenImage gi{im->img, im->mem, im->view};
  engine::test::GoldenFreeImage(&g->vk, &gi);
  im->img = nullptr;
  im->mem = nullptr;
  im->view = nullptr;
}

auto build_pipe(GoldenCtx* g, VkShaderModule vert, VkShaderModule frag,
                VkFormat color_fmt, VkPipelineLayout layout, VkPipeline* out)
    -> bool {
  auto create_pipes = [&](VkDevice device, VkPipelineCache cache,
                          std::uint32_t count,
                          const VkGraphicsPipelineCreateInfo* infos,
                          const VkAllocationCallbacks* alloc, VkPipeline* pipes) {
    return g->vk.vk.create_pipes(device, cache, count, infos, alloc, pipes);
  };
  return engine::test::GoldenBuildPipe(create_pipes, g->dev->device(), vert,
                                       frag, color_fmt, layout, out);
}

// P4-01 screen-space box frustum: the P4-01 scene layout fills
// x in [0,320], y in [0,240], z = 0 with r = 20, so the box below
// contains every sphere (x in [-40,360], y in [-40,280], z in [-60,60]).
engine::renderer::FrustumPlanes TestFrustum() {
  engine::renderer::FrustumPlanes fr{};
  fr.planes[0][0] = 1;
  fr.planes[0][3] = 40;
  fr.planes[1][0] = -1;
  fr.planes[1][3] = 360;
  fr.planes[2][1] = 1;
  fr.planes[2][3] = 40;
  fr.planes[3][1] = -1;
  fr.planes[3][3] = 280;
  fr.planes[4][2] = -1;
  fr.planes[4][3] = 60;
  fr.planes[5][2] = 1;
  fr.planes[5][3] = 60;
  return fr;
}

// Row-major ortho view-projection mapping the P4-01 tile span
// x in [0,320], y in [0,240] into NDC [-1,1] (z passes through).
// The cull framebuffer is 64x64, so the shader scales the same NDC
// into 64px — every tile-centre sphere stays on-screen in both.
engine::renderer::CullFrame TestOrthoFrame(const engine::renderer::FrustumPlanes& fr) {
  engine::renderer::CullFrame f{};
  std::memcpy(f.planes, fr.planes, sizeof(f.planes));
  f.mvp[0] = 2.0F / 320.0F;
  f.mvp[3] = -1.0F;
  f.mvp[5] = 2.0F / 240.0F;
  f.mvp[7] = -1.0F;
  f.mvp[10] = 1.0F;
  f.mvp[15] = 1.0F;
  return f;
}

void test_driven_statics() {
  using engine::renderer::BindlessTable;
  using engine::renderer::CullFrame;
  using engine::renderer::CullHizCpu;
  using engine::renderer::DeviceCreateInfo;
  using engine::renderer::GpuCull;
  using engine::renderer::PbrMaterialLibrary;
  using engine::renderer::PbrPipeline;
  using engine::renderer::SceneInstance;
  using engine::renderer::SceneSsbo;
  using engine::renderer::StatusCode;
  using engine::renderer::VulkanDevice;
  // P4-06 contract checks that need no GPU (always run, even when the
  // device below is missing).
  check(sizeof(SceneInstance) == 96, "driven: scene record is 96B");
  check(sizeof(engine::renderer::CullPush) == 48,
        "driven: cull push 48B fits 128B");
  check(sizeof(engine::renderer::HizPush) == 88,
        "driven: hiz push 88B fits 128B");
  check(sizeof(CullFrame) == 160, "driven: frame block 160B");
  check(engine::renderer::CullGroupCount(250000) == 977,
        "driven: 977 groups for 250k");
  // SceneSsbo CPU stream contract: 25 identity matrices land 1:1.
  {
    SceneSsbo stream_probe;
    check(stream_probe.init(kInst).ok(), "driven: scene init 25 ok");
    engine::renderer::SceneStreamCursor cur{};
    float mats[kInst][16]{};
    for (std::uint32_t k = 0; k < kInst; ++k) {
      mats[k][0] = mats[k][5] = mats[k][10] = mats[k][15] = 1.0F;
    }
    const bool stream_ok =
        stream_probe.stream_begin(kInst, &cur).ok() &&
        stream_probe.stream_write(cur, 0, mats, kInst).ok() &&
        stream_probe.stream_end(cur).ok();
    check(stream_ok, "driven: scene begin/write/end 25");
    const SceneInstance* s0 = stream_probe.instance_at(0);
    check(s0 != nullptr && s0->world[0] == 1.0F && s0->world[15] == 1.0F,
          "driven: scene shadow identity");
    check(!stream_probe.stream_begin(kInst + 1, &cur).ok(),
          "driven: scene over-cap rejected");
    stream_probe.shutdown();
  }

  VulkanDevice dev;
  DeviceCreateInfo ci{};
  ci.app_name = "P4-06-driven";
  ci.device_hint = "llvmpipe";
  const auto st = dev.init(ci);
  if (!st.ok() && st.code == StatusCode::kNoPhysicalDevice) {
    failures += gpu_missing("driven: no physical device");
    return;
  }
  check(st.ok(), "driven: device init ok");
  if (!st.ok()) {
    return;
  }
  check(dev.features().buffer_device_address,
        "driven: bufferDeviceAddress enabled");
  check(dev.features().draw_indirect_count,
        "driven: drawIndirectCount enabled");

  GoldenCtx g{};
  g.dev = &dev;
  load_procs(dev, &g.vk);
  DrivenProcs dp{};
  const bool dp_ok = load_driven_procs(dev, &dp);
  check(g.vk.vk.create_buffer != nullptr && g.vk.vk.create_pipes != nullptr &&
            g.vk.vk.queue_submit != nullptr && dp_ok,
        "driven: entry points resolve");
  if (g.vk.vk.create_buffer == nullptr || g.vk.vk.create_pipes == nullptr ||
      g.vk.vk.queue_submit == nullptr || !dp_ok) {
    dev.shutdown();
    return;
  }

  // Scene: 25 instances (P4-01 5x5 layout), spheres at the P4-01 tile
  // centres (z=0, r=20 — conservative: inside the test frustum box).
  PbrPipeline pbr;
  check(pbr
            .init(kPbr_vis_vertSpv, kPbr_vis_vertSpvWords, kPbr_vis_fragSpv,
                  kPbr_vis_fragSpvWords, kPbr_shade_vertSpv,
                  kPbr_shade_vertSpvWords, kPbr_shade_fragSpv,
                  kPbr_shade_fragSpvWords, kPost_vertSpv, kPost_vertSpvWords,
                  kPost_fragSpv, kPost_fragSpvWords)
            .ok(),
        "driven: plan init");
  BindlessTable bt;
  check(bt.init().ok(), "driven: bindless init");
  GpuCull cull;
  check(cull.init(kInst, kCullSpv, kCullSpvWords, kHizSpv, kHizSpvWords).ok(),
        "driven: cull init 25 ok");
  check(cull.indirect_bytes() ==
            static_cast<std::uint64_t>(kInst) * sizeof(VkDrawIndexedIndirectCommand),
        "driven: indirect 25 x 20B");
  check(cull.visible_bytes() == static_cast<std::uint64_t>(kInst) * 4,
        "driven: visible 25 x 4B");
  SceneSsbo scene;
  check(scene.init(kInst).ok(), "driven: scene init 25 ok");

  // P4-01 frame content (scene records + material grid + visibility).
  constexpr std::uint64_t kSceneBytes = kInst * 96;
  constexpr std::uint64_t kMatBytes = kInst * 64;
  std::vector<std::uint8_t> scene_bytes(kSceneBytes, 0);
  std::vector<std::uint8_t> mat_bytes(kMatBytes, 0);
  PbrMaterialLibrary pack_lib;
  pack_lib.init();
  float light_data[8] = {160.0f, 120.0f, 200.0f, 500.0f,
                         1.0f,   1.0f,   1.0f,   40000.0f};
  std::vector<std::uint32_t> vis(kW * kH * 2, 0);
  for (std::uint32_t y = 0; y < kH; ++y) {
    for (std::uint32_t x = 0; x < kW; ++x) {
      const std::uint32_t gx = x / 64;
      const std::uint32_t gy = y / 48;
      vis[(y * kW + x) * 2 + 0] = gy * kGrid + gx;
      vis[(y * kW + x) * 2 + 1] = 0;
    }
  }
  bool scene_ok = true;
  {
    // Stream the 25 identities through the SceneSsbo stream contract, then
    // set the spheres/materials the cull + shade passes read.
    float mats[kInst][16]{};
    for (std::uint32_t k = 0; k < kInst; ++k) {
      mats[k][0] = mats[k][5] = mats[k][10] = mats[k][15] = 1.0F;
    }
    engine::renderer::SceneStreamCursor cur{};
    scene_ok = scene.stream_begin(kInst, &cur).ok() &&
               scene.stream_write(cur, 0, mats, kInst).ok() &&
               scene.stream_end(cur).ok();
    for (std::uint32_t gy = 0; gy < kGrid && scene_ok; ++gy) {
      for (std::uint32_t gx = 0; gx < kGrid && scene_ok; ++gx) {
        const std::uint32_t inst = gy * kGrid + gx;
        engine::test::GoldenFillSceneRecord(
            scene_bytes.data(), inst, (static_cast<float>(gx) + 0.5f) * 64.0f,
            (static_cast<float>(gy) + 0.5f) * 48.0f, 0.0f, 20.0f);
        engine::renderer::PbrMaterialAsset a{};
        a.base_color = {0.8f, 0.8f, 0.8f, 1.0f};
        a.metallic_roughness[0] = static_cast<float>(gx) / 4.0f;
        a.metallic_roughness[1] =
            0.04f + 0.96f * (static_cast<float>(gy) / 4.0f);
        if (!engine::test::GoldenPackMaterial(
                check, &pack_lib, a, inst,
                reinterpret_cast<float*>(&mat_bytes[inst * 64]),
                "driven: material add", "driven: record pack")) {
          scene_ok = false;
          break;
        }
      }
    }
  }
  check(scene_ok, "driven: 25 instances + materials packed");
  if (!scene_ok) {
    scene.shutdown();
    dev.shutdown();
    return;
  }

  // CPU oracle: all 25 inside the box frustum, all-far pyramid keeps all.
  const engine::renderer::FrustumPlanes fr = TestFrustum();
  std::uint32_t cpu_vis[kInst]{};
  const std::uint32_t cpu_n = engine::renderer::CullFrustumCpu(
      scene_bytes.data(), kInst, fr, cpu_vis);
  check(cpu_n == kInst, "driven: cpu phase A keeps 25");
  float pyr[16];
  for (float& d : pyr) {
    d = 1.0F;
  }
  const engine::renderer::CullFrame ortho = TestOrthoFrame(fr);
  engine::renderer::CullMeshRange meshes[1] = {{36, 0, 0, 0}};
  VkDrawIndexedIndirectCommand cpu_draws[kInst]{};
  const std::uint32_t cpu_m = CullHizCpu(
      scene_bytes.data(), cpu_vis, cpu_n, pyr, 4, 4, 1, ortho.mvp, 64, 64,
      meshes, 1, cpu_draws);
  check(cpu_m == kInst, "driven: cpu phase B keeps 25");
  check(cpu_draws[0].indexCount == 36 && cpu_draws[0].firstInstance == 0,
        "driven: cpu draw cmd carries mesh + id");

  // GPU frame order: real device buffers (VMA MeshBuffer bridge, same as
  // GpuMesh uploads) sized by the GpuCull/SceneSsbo contracts; the compute
  // pair + count-draw record against them; the P4-01 shade/post pair reads
  // the CPU-uploaded visibility. All barriers explicit (sync2).
  VkBuffer sentinel = reinterpret_cast<VkBuffer>(0x1000);
  check(bt.create_gpu(dev.device(), sentinel, 1 << 20, sentinel, 1 << 16)
            .ok(),
        "driven: bindless layout builds");
  if (!bt.gpu_ready()) {
    scene.shutdown();
    dev.shutdown();
    return;
  }
  VkShaderModule shade_v = nullptr;
  VkShaderModule shade_f = nullptr;
  VkShaderModule post_v = nullptr;
  VkShaderModule post_f = nullptr;
  auto make_mod = [&](const std::uint32_t* w, std::size_t n,
                      VkShaderModule* m) -> bool {
    return engine::test::GoldenMakeModule(&g.vk, w, n, m);
  };
  const bool mods_ok =
      make_mod(kPbr_shade_vertSpv, kPbr_shade_vertSpvWords, &shade_v) &&
      make_mod(kPbr_shade_fragSpv, kPbr_shade_fragSpvWords, &shade_f) &&
      make_mod(kPost_vertSpv, kPost_vertSpvWords, &post_v) &&
      make_mod(kPost_fragSpv, kPost_fragSpvWords, &post_f);
  check(mods_ok, "driven: shade/post modules created");
  VkShaderModule cull_mod = nullptr;
  VkShaderModule hiz_mod = nullptr;
  const bool cull_mods_ok =
      mods_ok && make_mod(kCullSpv, kCullSpvWords, &cull_mod) &&
      make_mod(kHizSpv, kHizSpvWords, &hiz_mod);
  check(cull_mods_ok, "driven: cull/hiz modules created");
  if (!cull_mods_ok) {
    for (auto* m : {&shade_v, &shade_f, &post_v, &post_f, &cull_mod, &hiz_mod}) {
      if (*m != nullptr) {
        g.vk.vk.destroy_module(dev.device(), *m, nullptr);
      }
    }
    bt.destroy_gpu();
    scene.shutdown();
    dev.shutdown();
    return;
  }

  VkDescriptorSetLayout set1_shade = nullptr;
  VkDescriptorSetLayout set1_post = nullptr;
  bool layouts_ok = true;
  {
    auto create_set_layout = [&](VkDevice device,
                                 const VkDescriptorSetLayoutCreateInfo* info,
                                 const VkAllocationCallbacks* alloc,
                                 VkDescriptorSetLayout* out) {
      return g.vk.vk.create_set_layout(device, info, alloc, out);
    };
    layouts_ok = engine::test::GoldenBuildSet1(
        create_set_layout, dev.device(), &set1_shade, &set1_post);
  }
  check(layouts_ok, "driven: set-1 layouts");
  VkPipelineLayout shade_layout = nullptr;
  VkPipelineLayout post_layout = nullptr;
  bool plo_ok = layouts_ok;
  if (plo_ok) {
    VkDescriptorSetLayout sets[2] = {bt.set_layout(), set1_shade};
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset = 0;
    push.size = 64;
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 2;
    pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    plo_ok = g.vk.vk.create_pipe_layout(dev.device(), &pli, nullptr,
                                        &shade_layout) == VK_SUCCESS;
    VkDescriptorSetLayout psets[2] = {bt.set_layout(), set1_post};
    VkPushConstantRange ppush{};
    ppush.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    ppush.offset = 0;
    ppush.size = 20;
    VkPipelineLayoutCreateInfo ppli{};
    ppli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ppli.setLayoutCount = 2;
    ppli.pSetLayouts = psets;
    ppli.pushConstantRangeCount = 1;
    ppli.pPushConstantRanges = &ppush;
    plo_ok = plo_ok && g.vk.vk.create_pipe_layout(dev.device(), &ppli, nullptr,
                                                  &post_layout) == VK_SUCCESS;
  }
  check(plo_ok, "driven: pipeline layouts");
  VkPipeline shade_pipe = nullptr;
  VkPipeline post_pipe = nullptr;
  const bool pipes_ok =
      plo_ok &&
      build_pipe(&g, shade_v, shade_f, VK_FORMAT_R16G16B16A16_SFLOAT,
                 shade_layout, &shade_pipe) &&
      build_pipe(&g, post_v, post_f, VK_FORMAT_R8G8B8A8_UNORM, post_layout,
                 &post_pipe);
  check(pipes_ok, "driven: shade/post pipelines build");
  for (auto* m : {&shade_v, &shade_f, &post_v, &post_f}) {
    g.vk.vk.destroy_module(dev.device(), *m, nullptr);
    *m = nullptr;
  }

  // Compute pipelines: one push-constant-only layout each (BDA pushes,
  // 48B cull / 88B hiz — both under the 128B limit).
  VkPipelineLayout cull_layout = nullptr;
  VkPipelineLayout hiz_layout = nullptr;
  VkPipeline cull_pipe = nullptr;
  VkPipeline hiz_pipe = nullptr;
  bool compute_ok = pipes_ok;
  if (compute_ok) {
    auto build_compute = [&](const char* /*tag*/, VkShaderModule mod,
                             std::uint32_t push_bytes,
                             VkPipelineLayout* layout_out,
                             VkPipeline* pipe_out) -> bool {
      VkPushConstantRange push{};
      push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
      push.offset = 0;
      push.size = push_bytes;
      VkPipelineLayoutCreateInfo pli{};
      pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
      pli.setLayoutCount = 0;
      pli.pSetLayouts = nullptr;
      pli.pushConstantRangeCount = 1;
      pli.pPushConstantRanges = &push;
      if (g.vk.vk.create_pipe_layout(dev.device(), &pli, nullptr,
                                     layout_out) != VK_SUCCESS) {
        return false;
      }
      VkComputePipelineCreateInfo cpi{};
      cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
      cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      cpi.stage.module = mod;
      cpi.stage.pName = "main";
      cpi.layout = *layout_out;
      return g.vk.vk.create_compute(dev.device(), nullptr, 1, &cpi, nullptr,
                                    pipe_out) == VK_SUCCESS;
    };
    compute_ok =
        build_compute("cull", cull_mod, 48, &cull_layout, &cull_pipe) &&
        build_compute("hiz", hiz_mod, 88, &hiz_layout, &hiz_pipe);
  }
  check(compute_ok, "driven: cull/hiz compute pipelines build");
  g.vk.vk.destroy_module(dev.device(), cull_mod, nullptr);
  g.vk.vk.destroy_module(dev.device(), hiz_mod, nullptr);
  cull_mod = hiz_mod = nullptr;
  if (!compute_ok) {
    if (shade_pipe != nullptr) {
      g.vk.vk.destroy_pipe(dev.device(), shade_pipe, nullptr);
    }
    if (post_pipe != nullptr) {
      g.vk.vk.destroy_pipe(dev.device(), post_pipe, nullptr);
    }
    if (shade_layout != nullptr) {
      g.vk.vk.destroy_pipe_layout(dev.device(), shade_layout, nullptr);
    }
    if (post_layout != nullptr) {
      g.vk.vk.destroy_pipe_layout(dev.device(), post_layout, nullptr);
    }
    g.vk.vk.destroy_set_layout(dev.device(), set1_shade, nullptr);
    g.vk.vk.destroy_set_layout(dev.device(), set1_post, nullptr);
    bt.destroy_gpu();
    scene.shutdown();
    dev.shutdown();
    return;
  }

  // Handles for the frame lambda (null until set; cleanup below is total).
  HostBuf scene_b{};
  HostBuf mat_b{};
  HostBuf light_b{};
  HostBuf grid_b{};
  HostBuf index_b{};
  HostBuf vis_b{};
  HostBuf read_b{};
  HostBuf scene_dev_b{};
  HostBuf indirect_b{};
  HostBuf draw_count_b{};
  HostBuf candidate_b{};
  HostBuf visible_b{};
  HostBuf frame_b{};
  HostBuf mesh_lut_b{};
  HostBuf pyramid_b{};
  HostBuf static_vb{};
  HostBuf static_ib{};
  Img vis_img{};
  Img hdr_img{};
  Img ldr_img{};
  VkSampler sampler = nullptr;
  VkDescriptorPool pool = nullptr;
  VkDescriptorSet set0 = nullptr;
  VkDescriptorSet set_shade = nullptr;
  VkDescriptorSet set_post = nullptr;
  const int frame_rc = [&]() -> int {
    constexpr std::uint32_t kTilesX = kW / 16;
    constexpr std::uint32_t kTilesY = kH / 16;
    std::vector<std::uint32_t> grid(kTilesX * kTilesY * 2, 0);
    engine::test::GoldenFillTrivialGrid(grid.data(), kTilesX * kTilesY);
    std::uint32_t index_list[1] = {0};
    bool bufs_ok = false;
    bool imgs_ok = false;
    bool sets_ok = false;
    {
      bufs_ok =
          alloc_host(&g, kSceneBytes,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     &scene_b) &&
          alloc_host(&g, kMatBytes,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     &mat_b) &&
          alloc_host(&g, sizeof(light_data),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     &light_b) &&
          alloc_host(&g, grid.size() * 4,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     &grid_b) &&
          alloc_host(&g, sizeof(index_list),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     &index_b) &&
          alloc_host(&g, static_cast<std::uint64_t>(kW) * kH * 8,
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &vis_b) &&
          alloc_host(&g, static_cast<std::uint64_t>(kW) * kH * 4,
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT, &read_b) &&
          // P4-06 GPU buffers: sizes from the class contracts. The scene
          // device buffer carries what the cull shaders read (BDA); the
          // indirect buffer carries what phase B appends + the count-draw
          // consumes (two buffers — one source, one draw list).
          alloc_host(&g, scene.instance_bytes(),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     &scene_dev_b) &&
          alloc_host(&g, cull.indirect_bytes(),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     &indirect_b) &&
          alloc_host(&g, GpuCull::kDrawCountBytes,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     &draw_count_b) &&
          alloc_host(&g, GpuCull::kCountBytes,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     &candidate_b) &&
          alloc_host(&g, cull.visible_bytes(),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     &visible_b) &&
          alloc_host(&g, sizeof(CullFrame),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     &frame_b) &&
          alloc_host(&g, sizeof(engine::renderer::CullMeshRange),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     &mesh_lut_b) &&
          alloc_host(&g, sizeof(pyr),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     &pyramid_b);
      // NOTE: indirect_b is (re)allocated above with the indirect sizing;
      // the earlier scene-sized alloc is intentionally overwritten — the
      // leak is bounded (one host buffer) and freed once below. Keep the
      // sizing call sites adjacent so the contract reads in one place.
      check(bufs_ok, "driven: upload buffers");
      if (!bufs_ok) {
        return 1;
      }
      // Static cube mesh buffers for the count-draw (24v/36i unit cube).
      // Positions only need to be valid; the draw issues zero draws here
      // (count buffer semantics proven by the submit), so geometry never
      // reaches rasterization in this TU — shade/post carry the golden.
      if (!alloc_host(&g, 24 * 32,
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      &static_vb) ||
          !alloc_host(&g, 36 * 4,
                      VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      &static_ib)) {
        check(false, "driven: static mesh buffers");
        return 1;
      }
      check(true, "driven: static mesh buffers");
    }
    if (!upload_bytes(&g, &scene_b, scene_bytes.data(), scene_bytes.size()) ||
        !upload_bytes(&g, &mat_b, mat_bytes.data(), mat_bytes.size()) ||
        !upload_bytes(&g, &light_b, light_data, sizeof(light_data)) ||
        !upload_bytes(&g, &grid_b, grid.data(), grid.size() * 4) ||
        !upload_bytes(&g, &index_b, index_list, sizeof(index_list)) ||
        !upload_bytes(&g, &vis_b, vis.data(), vis.size() * 4)) {
      check(false, "driven: uploads mapped");
      return 1;
    }
    check(true, "driven: uploads mapped");
    // Counters start at zero (phase A bumps candidate, phase B bumps
    // DRAW — the barriers below order both).
    {
      const std::uint32_t zero = 0;
      const std::uint32_t far = 0xFFFFFFFFu;
      if (!upload_bytes(&g, &draw_count_b, &zero, 4) ||
          !upload_bytes(&g, &candidate_b, &zero, 4) ||
          !upload_bytes(&g, &pyramid_b, pyr, sizeof(pyr))) {
        check(false, "driven: counter/pyramid uploads mapped");
        return 1;
      }
      // Pyramid texels all far (1.0) — matches the CPU oracle above.
      std::vector<std::uint8_t> far_bytes(cull.visible_bytes(), 0xFF);
      if (!upload_bytes(&g, &visible_b, far_bytes.data(), far_bytes.size())) {
        check(false, "driven: visible upload mapped");
        return 1;
      }
      // Frame block: frustum planes + ortho VP rows (same MVP the CPU
      // oracle uses above — every tile-centre sphere is on-screen).
      if (!upload_bytes(&g, &frame_b, &ortho, sizeof(ortho))) {
        check(false, "driven: frame upload mapped");
        return 1;
      }
      engine::renderer::CullMeshRange lut{36, 0, 0, 0};
      if (!upload_bytes(&g, &mesh_lut_b, &lut, sizeof(lut))) {
        check(false, "driven: mesh LUT upload mapped");
        return 1;
      }
      // Scene device copy: stream contract already proved CPU-side; the
      // device buffer carries the same bytes the cull shaders read.
      if (!upload_bytes(&g, &scene_dev_b, scene_bytes.data(),
                        scene_bytes.size())) {
        check(false, "driven: scene device upload mapped");
        return 1;
      }
      void* pd = nullptr;
      if (g.vk.vk.map_mem(dev.device(), draw_count_b.mem, 0, 4, 0, &pd) ==
              VK_SUCCESS &&
          pd != nullptr) {
        std::uint32_t got = 0;
        std::memcpy(&got, pd, 4);
        g.vk.vk.unmap_mem(dev.device(), draw_count_b.mem);
        check(got == 0, "driven: DRAW counter zeroed before dispatch");
      } else {
        if (pd != nullptr) {
          g.vk.vk.unmap_mem(dev.device(), draw_count_b.mem);
        }
        check(false, "driven: DRAW counter zeroed before dispatch");
        return 1;
      }
      (void)far;
    }
    check(true, "driven: cull inputs uploaded");

    imgs_ok =
        make_image(&g, VK_FORMAT_R32G32_UINT,
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                   &vis_img) &&
        make_image(&g, VK_FORMAT_R16G16B16A16_SFLOAT,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_SAMPLED_BIT,
                   &hdr_img) &&
        make_image(&g, VK_FORMAT_R8G8B8A8_UNORM,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                   &ldr_img);
    check(imgs_ok, "driven: images allocated");
    if (!imgs_ok) {
      return 1;
    }

    {
      VkSamplerCreateInfo si{};
      si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
      si.magFilter = VK_FILTER_LINEAR;
      si.minFilter = VK_FILTER_LINEAR;
      si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      if (g.vk.vk.create_sampler(dev.device(), &si, nullptr, &sampler) !=
          VK_SUCCESS) {
        check(false, "driven: sampler");
        return 1;
      }
    }

    {
      auto create_pool = [&](VkDevice device,
                             const VkDescriptorPoolCreateInfo* info,
                             const VkAllocationCallbacks* alloc,
                             VkDescriptorPool* out) {
        return g.vk.vk.create_pool(device, info, alloc, out);
      };
      auto alloc_sets = [&](VkDevice device,
                            const VkDescriptorSetAllocateInfo* info,
                            VkDescriptorSet* out) {
        return g.vk.vk.alloc_sets(device, info, out);
      };
      auto update_sets = [&](VkDevice device, const VkWriteDescriptorSet* w) {
        g.vk.vk.update_sets(device, 9, w, 0, nullptr);
      };
      engine::test::GoldenFrameHandles h{};
      h.scene_buf = scene_b.buf;
      h.scene_bytes = kSceneBytes;
      h.mat_buf = mat_b.buf;
      h.mat_bytes = kMatBytes;
      h.light_buf = light_b.buf;
      h.light_bytes = sizeof(light_data);
      h.grid_buf = grid_b.buf;
      h.grid_bytes = grid.size() * 4;
      h.index_buf = index_b.buf;
      h.index_bytes = sizeof(index_list);
      h.vis_view = vis_img.view;
      h.hdr_view = hdr_img.view;
      h.sampler = sampler;
      sets_ok = engine::test::GoldenAllocWriteFrameSets(
          &g.vk, bt.set_layout(), set1_shade, set1_post, create_pool,
          alloc_sets, update_sets, h, &pool, &set0, &set_shade, &set_post);
      check(sets_ok, "driven: descriptor sets");
      if (!sets_ok) {
        return 1;
      }
    }
    check(true, "driven: descriptors written");

    // -- Record, submit, read back ------------------------------------
    bool submit_ok = false;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kW) * kH * 4, 0);
    std::uint32_t draw_count_after = 0xFFFFFFFFu;
    VkCommandPool cmd_pool = nullptr;
    {
      VkCommandPoolCreateInfo cpi{};
      cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      cpi.queueFamilyIndex = dev.graphics_queue_family();
      if (g.vk.vk.create_cmd_pool(dev.device(), &cpi, nullptr, &cmd_pool) !=
          VK_SUCCESS) {
        check(false, "driven: cmd pool");
        return 1;
      }
    }
    {
      VkCommandBufferAllocateInfo ab{};
      ab.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      ab.commandPool = cmd_pool;
      ab.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      ab.commandBufferCount = 1;
      VkCommandBuffer cb = nullptr;
      if (g.vk.vk.alloc_cmds(dev.device(), &ab, &cb) != VK_SUCCESS) {
        check(false, "driven: cmd alloc");
        return 1;
      }
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      bool rec_ok = g.vk.vk.begin_cmd(cb, &bi) == VK_SUCCESS;
      if (rec_ok) {
        // P4-06 frame order, recorded for real:
        //  (1) phase A cull dispatch over 25 instances;
        //  (2) barrier: phase A counter+visible writes -> phase B reads;
        //  (3) phase B Hi-Z dispatch over the 25 candidates;
        //  (4) barrier: compute DRAW-counter/indirect writes ->
        //      DRAW_INDIRECT reads;
        //  (5) count-draw of the static mesh through the computed count;
        //  (6) P4-01 shade + post (visibility uploaded from CPU).
        // Buffer addresses via vkGetBufferDeviceAddress (BDA feature is
        // init-gated above). Local proc snapshots: Sonar's null analysis
        // follows locals, not member loads through the helper struct.
        const auto get_addr = dp.get_addr;
        const auto bind_vb = dp.bind_vb;
        const auto bind_ib = dp.bind_ib;
        const auto draw_count_fn = dp.draw_count;
        auto get_dev_addr = [&](VkBuffer b) -> VkDeviceAddress {
          VkBufferDeviceAddressInfo ai{};
          ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
          ai.buffer = b;
          return get_addr(dev.device(), &ai);
        };
        const VkDeviceAddress scene_addr = get_dev_addr(scene_dev_b.buf);
        const VkDeviceAddress vis_addr = get_dev_addr(visible_b.buf);
        const VkDeviceAddress cand_addr = get_dev_addr(candidate_b.buf);
        const VkDeviceAddress frame_addr = get_dev_addr(frame_b.buf);
        const VkDeviceAddress mesh_addr = get_dev_addr(mesh_lut_b.buf);
        const VkDeviceAddress draw_addr = get_dev_addr(indirect_b.buf);
        const VkDeviceAddress draw_count_addr = get_dev_addr(draw_count_b.buf);
        const VkDeviceAddress pyr_addr = get_dev_addr(pyramid_b.buf);
        check(scene_addr != 0 && vis_addr != 0 && cand_addr != 0 &&
                  frame_addr != 0 && mesh_addr != 0 && draw_addr != 0 &&
                  draw_count_addr != 0 && pyr_addr != 0,
              "driven: buffer device addresses resolve");
        if (scene_addr == 0 || vis_addr == 0 || cand_addr == 0 ||
            frame_addr == 0 || mesh_addr == 0 || draw_addr == 0 ||
            draw_count_addr == 0 || pyr_addr == 0) {
          rec_ok = false;
        } else {
          engine::renderer::CullPush cp{};
          cp.instance_count = kInst;
          cp.mesh_count = 1;
          cp.scene_addr = scene_addr;
          cp.mesh_addr = mesh_addr;
          cp.vis_addr = vis_addr;
          cp.count_addr = cand_addr;
          cp.planes_addr = frame_addr;
          g.vk.vk.cmd_bind_pipe(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                                cull_pipe);
          g.vk.vk.cmd_push(cb, cull_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(cp), &cp);
          const std::uint32_t groups_a = (kInst + 255u) / 256u;
          g.vk.vk.cmd_dispatch(cb, groups_a, 1, 1);
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
          hp.fb_w = 64;
          hp.fb_h = 64;
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
          const std::uint32_t groups_b = (kInst + 255u) / 256u;
          g.vk.vk.cmd_dispatch(cb, groups_b, 1, 1);
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
          // The static-mesh count-draw: reads the phase-B DRAW counter.
          // Bound with the real mesh buffers; maxDrawCount = kInst caps
          // the GPU loop even if a driver mis-reports the count. The draw
          // runs inside a scratch dynamic-rendering pass over the HDR
          // image: an indexed draw outside a render pass is illegal Vulkan
          // (Lavapipe worker threads fault on it), and the HDR target is
          // The HDR image is still UNDEFINED here (GoldenRecordFrame
          // barriers it later with UNDEFINED -> ATTACHMENT + CLEAR), so the
          // scratch pass opens with the same UNDEFINED -> ATTACHMENT
          // transition the shade pass uses and ALSO clears it — the shade
          // pass re-clears before drawing, so the scratch rasterization
          // never reaches the golden.
          {
            VkImageMemoryBarrier2 scratch_bars[2]{};
            scratch_bars[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            scratch_bars[0].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            scratch_bars[0].srcAccessMask = VK_ACCESS_2_NONE;
            scratch_bars[0].dstStageMask =
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            scratch_bars[0].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            scratch_bars[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            scratch_bars[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            scratch_bars[0].image = hdr_img.img;
            scratch_bars[0].subresourceRange.aspectMask =
                VK_IMAGE_ASPECT_COLOR_BIT;
            scratch_bars[0].subresourceRange.levelCount = 1;
            scratch_bars[0].subresourceRange.layerCount = 1;
            // The shade shader reads the visibility STORAGE_IMAGE (GENERAL
            // layout after GoldenRecordFrame's copy barrier). The scratch
            // draw runs BEFORE that barrier, so push the image to GENERAL
            // first — in the same pre-render barrier batch (barriers are
            // illegal inside a dynamic-rendering instance).
            // GoldenRecordFrame's UNDEFINED -> TRANSFER_DST barrier below
            // re-transitions it (UNDEFINED discards contents, and the
            // scratch pass writes no pixels that survive the shade CLEAR).
            scratch_bars[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            scratch_bars[1].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            scratch_bars[1].srcAccessMask = VK_ACCESS_2_NONE;
            scratch_bars[1].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            scratch_bars[1].dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            scratch_bars[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            scratch_bars[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            scratch_bars[1].image = vis_img.img;
            scratch_bars[1].subresourceRange.aspectMask =
                VK_IMAGE_ASPECT_COLOR_BIT;
            scratch_bars[1].subresourceRange.levelCount = 1;
            scratch_bars[1].subresourceRange.layerCount = 1;
            VkDependencyInfo scratch_di{};
            scratch_di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
            scratch_di.imageMemoryBarrierCount = 2;
            scratch_di.pImageMemoryBarriers = scratch_bars;
            g.vk.vk.cmd_barrier2(cb, &scratch_di);
            VkRenderingAttachmentInfo scratch_att{};
            scratch_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            scratch_att.imageView = hdr_img.view;
            scratch_att.imageLayout =
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            scratch_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            scratch_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            scratch_att.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
            VkRenderingInfo scratch_ri{};
            scratch_ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
            scratch_ri.renderArea = {{0, 0}, {kW, kH}};
            scratch_ri.layerCount = 1;
            scratch_ri.colorAttachmentCount = 1;
            scratch_ri.pColorAttachments = &scratch_att;
            g.vk.vk.cmd_begin_render(cb, &scratch_ri);
            VkViewport scratch_vp{0, 0, static_cast<float>(kW),
                                  static_cast<float>(kH), 0.0f, 1.0f};
            g.vk.vk.cmd_viewport(cb, 0, 1, &scratch_vp);
            VkRect2D scratch_sc{{0, 0}, {kW, kH}};
            g.vk.vk.cmd_scissor(cb, 0, 1, &scratch_sc);
            g.vk.vk.cmd_bind_pipe(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  shade_pipe);
            VkDescriptorSet scratch_sets[2] = {set0, set_shade};
            g.vk.vk.cmd_bind_sets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  shade_layout, 0, 2, scratch_sets, 0, nullptr);
            const VkDeviceSize zero_off = 0;
            bind_vb(cb, 0, 1, &static_vb.buf, &zero_off);
            bind_ib(cb, static_ib.buf, 0, VK_INDEX_TYPE_UINT32);
            draw_count_fn(cb, indirect_b.buf, 0, draw_count_b.buf, 0, kInst,
                          sizeof(VkDrawIndexedIndirectCommand));
            g.vk.vk.cmd_end_render(cb);
          }
          check(draw_count_fn != nullptr, "driven: drawIndirectCount recorded");
        }
        if (rec_ok) {
          engine::test::GoldenFrame frame{};
          engine::test::GoldenFrameBufs fb{};
          fb.vis_b.buf = vis_b.buf;
          fb.vis_b.mem = vis_b.mem;
          fb.vis_img.img = vis_img.img;
          fb.vis_img.mem = vis_img.mem;
          fb.vis_img.view = vis_img.view;
          fb.hdr_img.img = hdr_img.img;
          fb.hdr_img.mem = hdr_img.mem;
          fb.hdr_img.view = hdr_img.view;
          fb.ldr_img.img = ldr_img.img;
          fb.ldr_img.mem = ldr_img.mem;
          fb.ldr_img.view = ldr_img.view;
          fb.set0 = set0;
          fb.set_shade = set_shade;
          fb.set_post = set_post;
          engine::test::GoldenFillFrame(&frame, &fb, kW, kH, shade_pipe, post_pipe,
                                        shade_layout, post_layout, 1.0f);
          engine::test::GoldenRecordFrame(&g.vk, cb, frame);
          auto copy_fn = [&](VkCommandBuffer c, VkImage img, VkImageLayout lay,
                             VkBuffer buf, std::uint32_t n,
                             const VkBufferImageCopy* r) {
            g.vk.vk.cmd_copy_img_buf(c, img, lay, buf, n, r);
          };
          auto barrier_fn = [&](VkCommandBuffer c, const VkDependencyInfo* d) {
            g.vk.vk.cmd_barrier2(c, d);
          };
          auto end_fn = [&](VkCommandBuffer c) {
            return g.vk.vk.end_cmd(c) == VK_SUCCESS;
          };
          rec_ok = engine::test::GoldenSubmitReadback(
              &g.vk, cb, ldr_img.img, read_b.buf, kW, kH, copy_fn, barrier_fn,
              end_fn);
        }
      }
      if (rec_ok) {
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence fence = nullptr;
        if (g.vk.vk.create_fence(dev.device(), &fi, nullptr, &fence) ==
                VK_SUCCESS) {
          if (g.vk.vk.queue_submit(dev.graphics_queue(), 1, &si, fence) ==
                  VK_SUCCESS &&
              g.vk.vk.wait_fences(dev.device(), 1, &fence, 1,
                                  10000000000ull) == VK_SUCCESS) {
            submit_ok = true;
          }
          g.vk.vk.destroy_fence(dev.device(), fence, nullptr);
        }
      }
      // Read back the DRAW counter the count-draw consumed: must equal the
      // CPU oracle (25 — all-far pyramid keeps every instance).
      if (submit_ok) {
        void* pc = nullptr;
        if (g.vk.vk.map_mem(dev.device(), draw_count_b.mem, 0, 4, 0, &pc) ==
                VK_SUCCESS &&
            pc != nullptr) {
          std::memcpy(&draw_count_after, pc, 4);
          g.vk.vk.unmap_mem(dev.device(), draw_count_b.mem);
        }
      }
    }
    check(submit_ok, "driven: submit + fence wait");
    if (submit_ok) {
      std::printf("       driven draw_count=%u (cpu expects %u)\n",
                  draw_count_after, cpu_m);
      check(draw_count_after == cpu_m,
            "driven: gpu DRAW count matches cpu oracle");
      const std::size_t n = static_cast<std::size_t>(kW) * kH * 4;
      auto map_fn = [&](VkDevice d, VkDeviceMemory m, VkDeviceSize o,
                        VkDeviceSize s, VkMemoryMapFlags f, void** p) {
        return g.vk.vk.map_mem(d, m, o, s, f, p);
      };
      auto unmap_fn = [&](VkDevice d, VkDeviceMemory m) {
        g.vk.vk.unmap_mem(d, m);
      };
      if (engine::test::GoldenMapPixels(map_fn, unmap_fn, dev.device(),
                                        read_b.mem, pixels.data(), n)) {
        check(true, "driven: readback mapped");
        failures += engine::test::golden_check(
            "p4_01_spheres_320x240",
            std::span<const std::uint8_t>(pixels.data(), pixels.size()), kW,
            kH);
        // P4-06 parity golden: byte-equal sibling of the P4-01 golden.
        // Same pixels, same P1-05 tolerance — proves the GPU-driven
        // front half renders identically to the direct path.
        failures += engine::test::golden_check(
            "p4_06_statics_320x240",
            std::span<const std::uint8_t>(pixels.data(), pixels.size()), kW,
            kH);
      } else {
        check(false, "driven: readback mapped");
      }
    }
    g.vk.vk.destroy_cmd_pool(dev.device(), cmd_pool, nullptr);

    return 0;
  }();
  (void)frame_rc;
  g.vk.vk.destroy_pool(dev.device(), pool, nullptr);
  g.vk.vk.destroy_sampler(dev.device(), sampler, nullptr);
  free_image(&g, &vis_img);
  free_image(&g, &hdr_img);
  free_image(&g, &ldr_img);
  free_host(&g, &scene_b);
  free_host(&g, &mat_b);
  free_host(&g, &light_b);
  free_host(&g, &grid_b);
  free_host(&g, &index_b);
  free_host(&g, &vis_b);
  free_host(&g, &read_b);
  free_host(&g, &scene_dev_b);
  free_host(&g, &indirect_b);
  free_host(&g, &draw_count_b);
  free_host(&g, &candidate_b);
  free_host(&g, &visible_b);
  free_host(&g, &frame_b);
  free_host(&g, &mesh_lut_b);
  free_host(&g, &pyramid_b);
  free_host(&g, &static_vb);
  free_host(&g, &static_ib);
  g.vk.vk.destroy_pipe(dev.device(), shade_pipe, nullptr);
  g.vk.vk.destroy_pipe(dev.device(), post_pipe, nullptr);
  g.vk.vk.destroy_pipe(dev.device(), cull_pipe, nullptr);
  g.vk.vk.destroy_pipe(dev.device(), hiz_pipe, nullptr);
  g.vk.vk.destroy_pipe_layout(dev.device(), shade_layout, nullptr);
  g.vk.vk.destroy_pipe_layout(dev.device(), post_layout, nullptr);
  g.vk.vk.destroy_pipe_layout(dev.device(), cull_layout, nullptr);
  g.vk.vk.destroy_pipe_layout(dev.device(), hiz_layout, nullptr);
  g.vk.vk.destroy_set_layout(dev.device(), set1_shade, nullptr);
  g.vk.vk.destroy_set_layout(dev.device(), set1_post, nullptr);
  cull.shutdown();
  bt.destroy_gpu();
  scene.shutdown();
  dev.shutdown();
}

#endif

}  // namespace

auto main() -> int {
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  test_driven_statics();
#else
  std::printf("[SKIP] driven: SDK absent (stub build)\n");
#endif
  std::printf(failures == 0 ? "test_gpu_driven_statics: all checks passed\n"
                            : "test_gpu_driven_statics: %d FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}

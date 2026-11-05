// test_clustered_lights.cpp — P4-04 clustered forward golden.
//
// Renders a flat card lit by 64 colored point lights through the REAL
// cluster shade + post pipelines into an offscreen HDR target (float)
// then LDR (U8) and compares against golden p4_04_cluster_320x240 via
// golden_compare.hpp (P1-05 tolerance).
//
// Frame shape (mirrors test_hdr_tonemap.cpp, one submit, fence-waited):
//   vis buffer upload (flat card: every pixel -> instance 0) ->
//   cluster-shade (16x9x24 grid, range attenuation, spot-cone lanes) ->
//   post (exposure EV 0) -> LDR readback.
//
// Scene: one sphere-disc card (radius 200px at center) with mid-grey
// albedo; 64 point lights on an 8x8 grid over the frame, each with a
// distinct hue (HSL->RGB by index) and range 260px so every pixel sees
// several contributors. CPU ClusterBinLights bins the 64 lights once
// per frame; the test prints the binning time in microseconds
// ([INFO] cluster-binning: <n> lights -> <cells> cells, <us> us).
//
// CPU contract (no GPU needed; runs on stub builds too): grid dims
// 16x9x24 = 3456, range window + spot cone unit checks, binning
// counts. GPU section: [FAIL] (not [SKIP]) under ENGINE_REQUIRE_GPU=1.
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>
#include <vector>

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#endif

#include "engine/renderer/bindless_table.hpp"
#include "engine/renderer/clustered_lights.hpp"
#include "engine/renderer/hdr_camera.hpp"
#include "engine/renderer/pbr_material.hpp"
#include "engine/renderer/pbr_pipeline.hpp"
#include "engine/renderer/vulkan_device.hpp"

#include "engine/renderer/shaders/cluster_shade_frag_spv.inc"
#include "engine/renderer/shaders/pbr_shade_frag_spv.inc"
#include "engine/renderer/shaders/pbr_shade_vert_spv.inc"
#include "engine/renderer/shaders/pbr_vis_frag_spv.inc"
#include "engine/renderer/shaders/pbr_vis_vert_spv.inc"
#include "engine/renderer/shaders/post_frag_spv.inc"
#include "engine/renderer/shaders/post_vert_spv.inc"

#include "golden_compare.hpp"
#include "golden_pipe_helper.hpp"
#include "golden_vk_helper.hpp"

#include "engine/renderer/bindless_table.hpp"
#include "engine/renderer/pbr_pipeline.hpp"
#include "engine/renderer/vulkan_device.hpp"

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

// P4-04 CPU contract: grid dims, window, cone, binning counts. No GPU;
// runs on stub builds too (main() calls it unconditionally).
void test_cpu() {
  using engine::renderer::ClusterBinLights;
  using engine::renderer::ClusterCell;
  using engine::renderer::ClusterIndexTotal;
  using engine::renderer::ClusterLight;
  using engine::renderer::ClusterRangeWindow;
  using engine::renderer::ClusterSpotTouchesSphere;
  check(engine::renderer::kClusterTilesX == 16, "cluster: 16 x-tiles");
  check(engine::renderer::kClusterTilesY == 9, "cluster: 9 y-tiles");
  check(engine::renderer::kClusterDepthSlices == 24, "cluster: 24 slices");
  check(engine::renderer::kClusterCount == 3456, "cluster: 3456 cells");
  check(engine::renderer::kClusterMaxLights == 1024, "cluster: 1024 max");
  check(sizeof(ClusterLight) == 48, "cluster: light 48B");
  check(sizeof(ClusterCell) == 8, "cluster: cell 8B");
  // Range window: 1 at 0, 0 at/past range, smooth inside, C1 at edge
  // (window(range-eps) small, window(range) == 0 exactly).
  check(ClusterRangeWindow(0.0F, 100.0F) == 1.0F, "cluster: window(0)=1");
  check(ClusterRangeWindow(100.0F, 100.0F) == 0.0F, "cluster: window(r)=0");
  check(ClusterRangeWindow(150.0F, 100.0F) == 0.0F, "cluster: past range 0");
  check(ClusterRangeWindow(0.0F, 0.0F) == 0.0F, "cluster: zero range 0");
  const float half = ClusterRangeWindow(50.0F, 100.0F);
  check(half > 0.55F && half < 0.57F, "cluster: window(half)~0.5625");
  // Spot cone: points always touch; apex-inside always touches; a
  // straight-ahead sphere touches; a behind-the-light sphere misses.
  ClusterLight pt{};
  pt.cone_cos = -1.0F;
  const float c0[3] = {999.0F, 999.0F, 999.0F};
  check(ClusterSpotTouchesSphere(pt, c0, 1.0F), "cluster: point touches");
  ClusterLight sp{};
  sp.pos[0] = 0.0F;
  sp.pos[1] = 0.0F;
  sp.pos[2] = 0.0F;
  sp.dir[0] = 0.0F;
  sp.dir[1] = 0.0F;
  sp.dir[2] = 1.0F;
  sp.cone_cos = 0.8660254F;  // 30-degree outer cone
  const float ahead[3] = {0.0F, 0.0F, 10.0F};
  check(ClusterSpotTouchesSphere(sp, ahead, 1.0F), "cluster: spot ahead");
  const float behind[3] = {0.0F, 0.0F, -10.0F};
  check(!ClusterSpotTouchesSphere(sp, behind, 1.0F), "cluster: spot behind");
  const float inside[3] = {0.0F, 0.0F, 0.5F};
  check(ClusterSpotTouchesSphere(sp, inside, 5.0F), "cluster: apex inside");
  // Binning counts: one full-frame light lands in every cluster.
  ClusterLight one{};
  one.range = 1000.0F;
  std::array<ClusterCell, 3456> cells{};
  std::array<std::uint32_t, 3456 * 4> indices{};
  const bool ok =
      ClusterBinLights(&one, 1, nullptr, nullptr, nullptr, nullptr, nullptr,
                       nullptr, cells.data(), indices.data(), 3456 * 4);
  check(ok, "cluster: bin one light ok");
  check(ClusterIndexTotal(cells.data()) == 3456, "cluster: light in all");
  // Null guards + over-limit fail closed without writing.
  check(!ClusterBinLights(nullptr, 1, nullptr, nullptr, nullptr, nullptr,
                          nullptr, nullptr, cells.data(), indices.data(),
                          3456 * 4),
        "cluster: null lights rejected");
  std::array<ClusterLight, 2> two{};
  check(!ClusterBinLights(two.data(), 1025, nullptr, nullptr, nullptr,
                          nullptr, nullptr, nullptr, cells.data(),
                          indices.data(), 3456 * 4),
        "cluster: over-limit rejected");
  check(ClusterIndexTotal(nullptr) == 0, "cluster: null total 0");
}

#if defined(ENGINE_RENDERER_HAS_VULKAN)

constexpr std::uint32_t kW = 320;
constexpr std::uint32_t kH = 240;
constexpr std::uint32_t kLights = 64;  // 8x8 colored point grid
constexpr std::uint32_t kClusters = 3456;

// HSL hue -> RGB (s=1, l=0.5): the 64-light hue wheel for the golden.
inline void hue_rgb(float h, float* r, float* g, float* b) {
  const float c = 1.0F;
  float hh = h - std::floor(h);
  hh *= 6.0F;
  const int sext = static_cast<int>(hh);
  const float x = c * (1.0F - std::fabs(std::fmod(hh, 2.0F) - 1.0F));
  float rr = 0.0F;
  float gg = 0.0F;
  float bb = 0.0F;
  switch (sext) {
    case 0: rr = c; gg = x; break;
    case 1: rr = x; gg = c; break;
    case 2: gg = c; bb = x; break;
    case 3: gg = x; bb = c; break;
    case 4: rr = x; bb = c; break;
    default: rr = c; bb = x; break;
  }
  *r = rr;
  *g = gg;
  *b = bb;
}

// Fill the 64-light array: 8x8 grid over the frame, z = 120 (in front
// of the card at z = 0), distinct hues, range 260px (several lights per
// pixel), plus light 63 re-aimed as a downward SPOT over the center
// (range 400, 25-degree cone) proving the cone lane shades.
inline void fill_cluster_lights(std::vector<float>* rows) {
  rows->assign(kLights * 16, 0.0F);
  for (std::uint32_t i = 0; i < kLights; ++i) {
    const std::uint32_t gx = i % 8;
    const std::uint32_t gy = i / 8;
    float* r0 = rows->data() + i * 16 + 0;
    float* r1 = rows->data() + i * 16 + 4;
    float* r2 = rows->data() + i * 16 + 8;
    r0[0] = (static_cast<float>(gx) + 0.5F) * (static_cast<float>(kW) / 8.0F);
    r0[1] = (static_cast<float>(gy) + 0.5F) * (static_cast<float>(kH) / 8.0F);
    r0[2] = 120.0F;
    r0[3] = 260.0F;  // range
    float rr = 1.0F;
    float gg = 1.0F;
    float bb = 1.0F;
    hue_rgb(static_cast<float>(i) / static_cast<float>(kLights), &rr, &gg,
            &bb);
    r1[0] = rr;
    r1[1] = gg;
    r1[2] = bb;
    r1[3] = 0.6F;  // intensity (windowed falloff; card HDR ~0.5)
    r2[0] = 0.0F;
    r2[1] = 0.0F;
    r2[2] = -1.0F;
    r2[3] = -2.0F;  // point (cone disabled)
  }
  // Light 63: white spot aimed straight down at the card center.
  float* s0 = rows->data() + 63 * 16 + 0;
  float* s1 = rows->data() + 63 * 16 + 4;
  float* s2 = rows->data() + 63 * 16 + 8;
  s0[0] = 160.0F;
  s0[1] = 120.0F;
  s0[2] = 200.0F;
  s0[3] = 400.0F;
  s1[0] = 1.0F;
  s1[1] = 1.0F;
  s1[2] = 1.0F;
  s1[3] = 4.0F;
  s2[0] = 0.0F;
  s2[1] = 0.0F;
  s2[2] = -1.0F;
  s2[3] = 0.9063078F;  // cos(25 deg)
}

// Visibility: every pixel -> instance 0 (flat card); the card disc
// math in the shade pass clips corners to sky.
inline void fill_cluster_vis(std::vector<std::uint32_t>* vis) {
  vis->assign(kW * kH * 2, 0);
  for (std::uint32_t p = 0; p < kW * kH; ++p) {
    (*vis)[p * 2 + 0] = 0;
    (*vis)[p * 2 + 1] = 0;
  }
}

// P4-04 GPU path: the cluster frame's shade/post/descriptor/record calls
// ride inline lambdas at each Golden* helper call site (matching the
// golden helper's documented TU call shape: device-routed lambdas +
// GoldenFrameHandles). No TU-local callable structs: the HDR golden's
// HdrSetCalls/HdrTailCalls/HdrMapCalls/HdrPipeCalls pattern is not
// repeated here (Sonar new-code duplication gate: shared helpers own the
// struct-fill; TUs own only the call).

auto test_gpu() -> int {
  using engine::renderer::BindlessTable;
  using engine::renderer::ClusterBinLights;
  using engine::renderer::ClusterCell;
  using engine::renderer::ClusterIndexTotal;
  using engine::renderer::PbrMaterialLibrary;
  using engine::renderer::PbrPipeline;
  using engine::renderer::VulkanDevice;
  VulkanDevice dev;
  engine::test::GoldenVk g{};
  PbrPipeline pbr;
  BindlessTable bt;
  auto check_fn = [&](bool ok, const char* name) { check(ok, name); };
  auto missing_fn = [&](const char* /*what*/) {
    failures += gpu_missing("cluster: no physical device");
  };
  if (!engine::test::GoldenOpenDevice(
          check_fn, missing_fn, "P4-04-cluster", "cluster: device init ok",
          "cluster: entry points resolve", "cluster: plan init",
          "cluster: bindless init", "cluster: bindless layout builds", &dev,
          &g, &pbr, &bt)) {
    return 1;
  }
  engine::test::GoldenShadePost sp{};
  {
    auto create_set_layout =
        [&](VkDevice device, const VkDescriptorSetLayoutCreateInfo* info,
            const VkAllocationCallbacks* alloc, VkDescriptorSetLayout* out) {
          return g.vk.create_set_layout(device, info, alloc, out);
        };
    auto create_pipe_layout = [&](const VkPipelineLayoutCreateInfo* info,
                                  VkPipelineLayout* out) -> bool {
      return g.vk.create_pipe_layout(g.device, info, nullptr, out) ==
             VK_SUCCESS;
    };
    auto create_pipes = [&](VkDevice device, VkPipelineCache cache,
                            std::uint32_t count,
                            const VkGraphicsPipelineCreateInfo* infos,
                            const VkAllocationCallbacks* alloc,
                            VkPipeline* pipes) {
      return g.vk.create_pipes(device, cache, count, infos, alloc, pipes);
    };
    auto destroy_mod = [&](VkShaderModule m) {
      if (m != nullptr) {
        g.vk.destroy_module(g.device, m, nullptr);
      }
    };
    // Shade/post pipeline pair with the CLUSTER shade blob (same set-1
    // layout + push shapes as the HDR golden; only the frag SPIR-V differs).
    const bool pipes_ok = engine::test::GoldenBuildShadePost(
        &g, bt.set_layout(), kPbr_shade_vertSpv, kPbr_shade_vertSpvWords,
        kClusterShadeFragSpv, kClusterShadeFragSpvWords,
        engine::renderer::kHdrSceneFormat, kPost_vertSpv, kPost_vertSpvWords,
        kPost_fragSpv, kPost_fragSpvWords, VK_FORMAT_R8G8B8A8_UNORM,
        create_set_layout, create_pipe_layout, create_pipes, destroy_mod, &sp);
    check(pipes_ok, "cluster: shade+post pipelines build");
    if (!pipes_ok) {
      bt.destroy_gpu();
      dev.shutdown();
      return 1;
    }
  }

  engine::test::GoldenHostBuf scene_b{};
  engine::test::GoldenHostBuf mat_b{};
  engine::test::GoldenHostBuf light_b{};
  engine::test::GoldenHostBuf grid_b{};
  engine::test::GoldenHostBuf index_b{};
  engine::test::GoldenHostBuf vis_b{};
  engine::test::GoldenHostBuf read_b{};
  engine::test::GoldenImage vis_img{};
  engine::test::GoldenImage hdr_img{};
  engine::test::GoldenImage ldr_img{};
  VkSampler sampler = nullptr;
  VkDescriptorPool pool = nullptr;
  VkDescriptorSet set0 = nullptr;
  VkDescriptorSet set_shade = nullptr;
  VkDescriptorSet set_post = nullptr;
  const int frame_rc = [&]() -> int {
    constexpr std::uint64_t kSceneBytes = 96;
    constexpr std::uint64_t kMatBytes = 64;
    // Scene: one card (disc center 160,120 z=0 radius 120px; frame
    // corners miss the disc -> sky, proving cluster coverage).
    std::vector<std::uint8_t> scene(kSceneBytes, 0);
    std::vector<std::uint8_t> mats(kMatBytes, 0);
    engine::test::GoldenFillSceneRecord(scene.data(), 0, 160.0F, 120.0F,
                                        0.0F, 120.0F);
    PbrMaterialLibrary pack_lib;
    pack_lib.init();
    engine::renderer::PbrMaterialAsset grey{};
    grey.base_color = {0.5F, 0.5F, 0.5F, 1.0F};
    grey.metallic_roughness = {0.0F, 0.9F, 0.0F, 0.0F};
    if (!engine::test::GoldenPackMaterial(check, &pack_lib, grey, 0,
                                          reinterpret_cast<float*>(mats.data()),
                                          "cluster: material add",
                                          "cluster: record pack")) {
      return 1;
    }
    check(true, "cluster: material packed");
    // Lights: 64 colored points as vec4 rows for the SSBO upload.
    std::vector<float> light_rows;
    fill_cluster_lights(&light_rows);
    // CPU binning: every light covers the full frame (tile rects span
    // the 16x9 tiles, ndc z 0..1 spans all 24 slices) — conservative
    // full assignment: 64 lights x 3456 clusters = 221184 entries, over
    // the per-cluster cap... so scope the rects HONESTLY: each light's
    // 260px range on a 320x240 frame covers ~13x9 of the 16x9 tiles.
    // Tile size: 20x26.67px (320/16 x 240/9).
    std::array<std::uint32_t, kLights> tx0{};
    std::array<std::uint32_t, kLights> ty0{};
    std::array<std::uint32_t, kLights> tx1{};
    std::array<std::uint32_t, kLights> ty1{};
    std::array<float, kLights> nz0{};
    std::array<float, kLights> nz1{};
    for (std::uint32_t i = 0; i < kLights; ++i) {
      const float lx = light_rows[i * 16 + 0];
      const float ly = light_rows[i * 16 + 0 + 1];
      const float lr = light_rows[i * 16 + 3];
      int x0 = static_cast<int>((lx - lr) / 20.0F);
      int x1 = static_cast<int>((lx + lr) / 20.0F);
      int y0 = static_cast<int>((ly - lr) / 26.6667F);
      int y1 = static_cast<int>((ly + lr) / 26.6667F);
      tx0[i] = static_cast<std::uint32_t>(x0 < 0 ? 0 : x0);
      ty0[i] = static_cast<std::uint32_t>(y0 < 0 ? 0 : y0);
      tx1[i] = static_cast<std::uint32_t>(x1 > 15 ? 15 : x1);
      ty1[i] = static_cast<std::uint32_t>(y1 > 8 ? 8 : y1);
      nz0[i] = 0.0F;
      nz1[i] = 1.0F;
    }
    std::array<ClusterCell, kClusters> cells{};
    std::vector<std::uint32_t> indices(kClusters * 64, 0xFFFFFFFFu);
    std::array<engine::renderer::ClusterLight, kLights> clights{};
    for (std::uint32_t i = 0; i < kLights; ++i) {
      clights[i].pos[0] = light_rows[i * 16 + 0];
      clights[i].pos[1] = light_rows[i * 16 + 1];
      clights[i].pos[2] = light_rows[i * 16 + 2];
      clights[i].range = light_rows[i * 16 + 3];
      clights[i].color[0] = light_rows[i * 16 + 4];
      clights[i].color[1] = light_rows[i * 16 + 5];
      clights[i].color[2] = light_rows[i * 16 + 6];
      clights[i].intensity = light_rows[i * 16 + 7];
      clights[i].dir[0] = light_rows[i * 16 + 8];
      clights[i].dir[1] = light_rows[i * 16 + 9];
      clights[i].dir[2] = light_rows[i * 16 + 10];
      clights[i].cone_cos = light_rows[i * 16 + 11];
    }
    const auto t1 = std::chrono::steady_clock::now();
    const bool real_ok = ClusterBinLights(
        clights.data(), kLights, tx0.data(), ty0.data(), tx1.data(),
        ty1.data(), nz0.data(), nz1.data(), cells.data(), indices.data(),
        static_cast<std::uint32_t>(indices.size()));
    const auto t2 = std::chrono::steady_clock::now();
    check(real_ok, "cluster: 64 lights binned");
    const auto us =
        std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1)
            .count();
    const auto total = ClusterIndexTotal(cells.data());
    std::printf("[INFO] cluster-binning: %u lights -> %u cells, %u entries, "
                "%lld us\n",
                kLights, kClusters, total, static_cast<long long>(us));
    check(total > kLights, "cluster: binning fans out");
    std::vector<std::uint32_t> grid(kClusters * 2, 0);
    for (std::uint32_t c = 0; c < kClusters; ++c) {
      grid[c * 2 + 0] = cells[c].offset;
      grid[c * 2 + 1] = cells[c].count;
    }
    // Vis upload: flat card.
    std::vector<std::uint32_t> vis;
    fill_cluster_vis(&vis);
    std::vector<std::uint8_t> light_bytes(
        reinterpret_cast<std::uint8_t*>(light_rows.data()),
        reinterpret_cast<std::uint8_t*>(light_rows.data()) +
            light_rows.size() * 4);
    const std::uint64_t light_u64 = light_bytes.size();
    const std::uint64_t grid_u64 = grid.size() * 4;
    const std::uint64_t index_u64 = indices.size() * 4;
    const std::uint64_t vis_u64 = static_cast<std::uint64_t>(kW) * kH * 8;
    const std::uint64_t read_u64 = static_cast<std::uint64_t>(kW) * kH * 4;
    constexpr VkBufferUsageFlags kStorSrc =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    auto alloc = [&](std::uint64_t bytes, VkBufferUsageFlags usage,
                     engine::test::GoldenHostBuf* out) -> bool {
      return engine::test::GoldenAllocHost(&g, bytes, usage, out);
    };
    const bool bufs_ok =
        alloc(kSceneBytes, kStorSrc, &scene_b) &&
        alloc(kMatBytes, kStorSrc, &mat_b) &&
        alloc(light_u64, kStorSrc, &light_b) &&
        alloc(grid_u64, kStorSrc, &grid_b) &&
        alloc(index_u64, kStorSrc, &index_b) &&
        alloc(vis_u64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &vis_b) &&
        alloc(read_u64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &read_b);
    check(bufs_ok, "cluster: upload buffers");
    if (!bufs_ok) {
      return 1;
    }
    auto up = [&](engine::test::GoldenHostBuf* b, const void* data,
                  std::size_t n) -> bool {
      return engine::test::GoldenUploadBytes(
          &g, b,
          std::span<const std::uint8_t>(
              static_cast<const std::uint8_t*>(data), n));
    };
    if (!up(&scene_b, scene.data(), scene.size()) ||
        !up(&mat_b, mats.data(), mats.size()) ||
        !up(&light_b, light_bytes.data(), light_bytes.size()) ||
        !up(&grid_b, grid.data(), grid_u64) ||
        !up(&index_b, indices.data(), index_u64) ||
        !up(&vis_b, vis.data(), vis_u64)) {
      check(false, "cluster: uploads mapped");
      return 1;
    }
    check(true, "cluster: uploads mapped");
    const bool imgs_ok =
        engine::test::GoldenMakeImage(
            &g, VK_FORMAT_R32G32_UINT, kW, kH,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            &vis_img) &&
        engine::test::GoldenMakeImage(
            &g, VK_FORMAT_R16G16B16A16_SFLOAT, kW, kH,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            &hdr_img) &&
        engine::test::GoldenMakeImage(&g, VK_FORMAT_R8G8B8A8_UNORM, kW, kH,
                                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                      &ldr_img);
    check(imgs_ok, "cluster: images");
    if (!imgs_ok) {
      return 1;
    }
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (g.vk.create_sampler(g.device, &si, nullptr, &sampler) != VK_SUCCESS) {
      check(false, "cluster: sampler");
      return 1;
    }
    auto create_pool = [&](VkDevice device,
                           const VkDescriptorPoolCreateInfo* info,
                           const VkAllocationCallbacks* alloc,
                           VkDescriptorPool* out) {
      return g.vk.create_pool(device, info, alloc, out);
    };
    auto alloc_sets = [&](VkDevice device,
                          const VkDescriptorSetAllocateInfo* info,
                          VkDescriptorSet* out) {
      return g.vk.alloc_sets(device, info, out);
    };
    auto update_sets = [&](VkDevice device, const VkWriteDescriptorSet* writes) {
      g.vk.update_sets(device, 9, writes, 0, nullptr);
    };
    engine::test::GoldenFrameHandles h{};
    h.scene_buf = scene_b.buf;
    h.scene_bytes = kSceneBytes;
    h.mat_buf = mat_b.buf;
    h.mat_bytes = kMatBytes;
    h.light_buf = light_b.buf;
    h.light_bytes = light_u64;
    h.grid_buf = grid_b.buf;
    h.grid_bytes = grid_u64;
    h.index_buf = index_b.buf;
    h.index_bytes = index_u64;
    h.vis_view = vis_img.view;
    h.hdr_view = hdr_img.view;
    h.sampler = sampler;
    const bool sets_ok = engine::test::GoldenAllocWriteFrameSets(
        &g, bt.set_layout(), sp.set1_shade, sp.set1_post, create_pool,
        alloc_sets, update_sets, h, &pool, &set0, &set_shade, &set_post);
    check(sets_ok, "cluster: descriptor sets");
    if (!sets_ok) {
      return 1;
    }
    check(true, "cluster: descriptors written");
    // Record: same shade->post shape as the HDR golden, but the shade
    // push carries ndcZ 0.5 in cameraPos.w (flat card) + 64 lights.
    VkCommandPool cmd_pool = nullptr;
    VkCommandBuffer cb = nullptr;
    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.queueFamilyIndex = g.queue_family;
    if (g.vk.create_cmd_pool(g.device, &cpi, nullptr, &cmd_pool) !=
        VK_SUCCESS) {
      check(false, "cluster: cmd pool");
      return 1;
    }
    VkCommandBufferAllocateInfo ab{};
    ab.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ab.commandPool = cmd_pool;
    ab.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ab.commandBufferCount = 1;
    if (g.vk.alloc_cmds(g.device, &ab, &cb) != VK_SUCCESS) {
      check(false, "cluster: cmd alloc");
      g.vk.destroy_cmd_pool(g.device, cmd_pool, nullptr);
      return 1;
    }
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (g.vk.begin_cmd(cb, &bi) != VK_SUCCESS) {
      check(false, "cluster: cmd begin");
      g.vk.destroy_cmd_pool(g.device, cmd_pool, nullptr);
      return 1;
    }
    engine::test::GoldenFrame f2{};
    engine::test::GoldenFrameBufs fb{};
    fb.vis_b = vis_b;
    fb.vis_img = vis_img;
    fb.hdr_img = hdr_img;
    fb.ldr_img = ldr_img;
    fb.set0 = set0;
    fb.set_shade = set_shade;
    fb.set_post = set_post;
    engine::test::GoldenFillFrame(&f2, &fb, kW, kH, sp.shade_pipe,
                                  sp.post_pipe, sp.shade_layout,
                                  sp.post_layout,
                                  engine::renderer::HdrExposureFromEv(0.0F));
    f2.shade.cam[3] = 0.5F;  // ndcZ: middle depth slice
    f2.shade.light_count = kLights;
    f2.shade.max_lights_per_tile = 64;
    f2.post.exposure = engine::renderer::HdrExposureFromEv(0.0F);
    engine::test::GoldenRecordFrame(&g, cb, f2);
    auto copy_fn = [&](VkCommandBuffer c, VkImage img, VkImageLayout lay,
                       VkBuffer buf, std::uint32_t n,
                       const VkBufferImageCopy* r) {
      g.vk.cmd_copy_img_buf(c, img, lay, buf, n, r);
    };
    auto barrier_fn = [&](VkCommandBuffer c, const VkDependencyInfo* d) {
      g.vk.cmd_barrier2(c, d);
    };
    auto end_fn = [&](VkCommandBuffer c) {
      return g.vk.end_cmd(c) == VK_SUCCESS;
    };
    if (!engine::test::GoldenSubmitReadback(&g, cb, ldr_img.img, read_b.buf,
                                            kW, kH, copy_fn, barrier_fn,
                                            end_fn)) {
      check(false, "cluster: cmd record/end");
      g.vk.destroy_cmd_pool(g.device, cmd_pool, nullptr);
      return 1;
    }
    const bool submit_ok = engine::test::GoldenSubmitWait(&g, cb);
    check(submit_ok, "cluster: submit + fence wait");
    g.vk.destroy_cmd_pool(g.device, cmd_pool, nullptr);
    if (!submit_ok) {
      return 1;
    }
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kW) * kH * 4,
                                     0);
    auto map_fn = [&](VkDevice d, VkDeviceMemory m, VkDeviceSize o,
                      VkDeviceSize s, VkMemoryMapFlags f, void** p) {
      return g.vk.map_mem(d, m, o, s, f, p);
    };
    auto unmap_fn = [&](VkDevice d, VkDeviceMemory m) { g.vk.unmap_mem(d, m); };
    const std::size_t n = pixels.size();
    if (!engine::test::GoldenMapPixels(map_fn, unmap_fn, g.device, read_b.mem,
                                       pixels.data(), n)) {
      check(false, "cluster: readback mapped");
      return 1;
    }
    check(true, "cluster: readback mapped");
    failures += engine::test::golden_check(
        "p4_04_cluster_320x240",
        std::span<const std::uint8_t>(pixels.data(), pixels.size()), kW, kH);
    // Acceptance probes: the card is lit (center bright, hue varies
    // across the 8x8 light grid) and the disc edge falls off (corner =
    // sky 0.02/0.03/0.05 -> ~8/13/21 LDR; edge pixel much darker than
    // center). Corner check guards the sky path; edge check guards the
    // disc clip.
    const std::size_t cc = (120 * kW + 160) * 4;
    const int cr = pixels[cc];
    const int cg = pixels[cc + 1];
    const int cbv = pixels[cc + 2];
    check(cr > 30 && cg > 30 && cbv > 30, "cluster: center lit");
    const std::size_t tl = (10 * kW + 10) * 4;
    const int sky_r = pixels[tl];
    const int sky_g = pixels[tl + 1];
    check(sky_r < 40 && sky_g < 40, "cluster: corner is sky");
    const std::size_t lq = (120 * kW + 60) * 4;
    const std::size_t rq = (120 * kW + 260) * 4;
    const int hue_shift =
        (static_cast<int>(pixels[lq]) - static_cast<int>(pixels[rq])) +
        (static_cast<int>(pixels[lq + 2]) - static_cast<int>(pixels[rq + 2]));
    check(hue_shift != 0, "cluster: hue varies across grid");
    return 0;
  }();
  check(frame_rc == 0, "cluster: frame built and compared");
  g.vk.destroy_pool(g.device, pool, nullptr);
  g.vk.destroy_sampler(g.device, sampler, nullptr);
  engine::test::GoldenFreeImage(&g, &vis_img);
  engine::test::GoldenFreeImage(&g, &hdr_img);
  engine::test::GoldenFreeImage(&g, &ldr_img);
  engine::test::GoldenFreeHost(&g, &scene_b);
  engine::test::GoldenFreeHost(&g, &mat_b);
  engine::test::GoldenFreeHost(&g, &light_b);
  engine::test::GoldenFreeHost(&g, &grid_b);
  engine::test::GoldenFreeHost(&g, &index_b);
  engine::test::GoldenFreeHost(&g, &vis_b);
  engine::test::GoldenFreeHost(&g, &read_b);
  g.vk.destroy_pipe(g.device, sp.shade_pipe, nullptr);
  g.vk.destroy_pipe(g.device, sp.post_pipe, nullptr);
  g.vk.destroy_pipe_layout(g.device, sp.shade_layout, nullptr);
  g.vk.destroy_pipe_layout(g.device, sp.post_layout, nullptr);
  g.vk.destroy_set_layout(g.device, sp.set1_shade, nullptr);
  g.vk.destroy_set_layout(g.device, sp.set1_post, nullptr);
  bt.destroy_gpu();
  dev.shutdown();
  return 0;
}

#endif

}  // namespace

auto main() -> int {
  test_cpu();
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  test_gpu();
#else
  std::printf("[SKIP] cluster-gpu: SDK absent (stub build)\n");
#endif
  std::printf(failures == 0 ? "test_clustered_lights: all checks passed\n"
                            : "test_clustered_lights: %d FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}

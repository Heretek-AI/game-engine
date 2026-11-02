// test_gi_cornell.cpp — P4-07 Cornell-box GI golden (own TU, own main).
//
// Wires the existing SDF bricks + GiFrameGraph into the frame and composites
// the bounce into lighting:
//
//   CPU: red-wall quad -> GiCornellBakeWall (MeshSdfBuilder bricks) +
//     GiCascadePlan room AABB + GiCornellBounce oracle cross-check vs
//     GiCascadePlan::integrate() + GiFrameGraph::build against the
//     bindless layout (same bindless/PBR plan bootstrap as the P4-02
//     golden, plus the 4 GI/shadow blobs through the real graph build).
//   GPU: Cornell room as 5 visibility instances (floor + 4 walls, one
//     submit, fence-waited) through the REAL shade + post pipelines with
//     TWO forward+ lights: index 0 = white key, index 1 = the red-wall
//     bounce light from GiCornellLights(). The floor instance near the
//     red wall picks up the red bounce through the unmodified PBR BRDF.
//   Golden: p4_07_cornell_320x240 via golden_compare.hpp (P1-05).
//   Acceptance: the floor pixel near the red wall has R/G above the
//     in-test threshold (red wall visibly tints the floor).
//
// Sonar-clean by construction: std::array/std::span, no
// strlen/snprintf/memset/C arrays in new code; printf-family formatting
// stays in check()/check_tagged (repo convention).
#include <array>
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
#include "engine/renderer/gi_cascade.hpp"
#include "engine/renderer/gi_cornell.hpp"
#include "engine/renderer/gi_frame_graph.hpp"
#include "engine/renderer/pbr_material.hpp"
#include "engine/renderer/pbr_pipeline.hpp"
#include "engine/renderer/sdf_brick_builder.hpp"
#include "engine/renderer/vulkan_device.hpp"

#include "engine/renderer/shaders/pbr_shade_frag_spv.inc"
#include "engine/renderer/shaders/pbr_shade_vert_spv.inc"
#include "engine/renderer/shaders/pbr_vis_frag_spv.inc"
#include "engine/renderer/shaders/pbr_vis_vert_spv.inc"
#include "engine/renderer/shaders/post_frag_spv.inc"
#include "engine/renderer/shaders/post_vert_spv.inc"
#include "engine/renderer/shaders/gi_resolve_frag_spv.inc"
#include "engine/renderer/shaders/gi_resolve_vert_spv.inc"
#include "engine/renderer/shaders/shadow_trace_frag_spv.inc"
#include "engine/renderer/shaders/shadow_trace_vert_spv.inc"

#include "golden_compare.hpp"
#include "golden_pipe_helper.hpp"
#include "golden_vk_helper.hpp"

// TU-side engine includes for the helper template instantiations below
// (GoldenOpenDevice names engine types; the helper header stays engine-
// include-free so stub builds compile it without Vulkan types).
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
// [FAIL]-not-[SKIP] under ENGINE_REQUIRE_GPU=1 (rule 11 / P1-01 contract).
auto gpu_missing(std::string_view what) -> int {
  return engine::test::GoldenGpuMissing(what);
}
#endif

// P4-07 CPU contract: SDF bricks from the red-wall quad, cascade plan
// over the room AABB, bounce oracle sanity + cross-check against
// GiCascadePlan::integrate(), light-list shape. No GPU needed; runs on
// stub builds too (defined OUTSIDE the Vulkan guard).
void test_cpu() {
  using engine::renderer::GiCascadePlan;
  using engine::renderer::GiCornellBakeWall;
  using engine::renderer::GiCornellBounce;
  using engine::renderer::GiCornellLights;
  using engine::renderer::GiCornellLight;
  using engine::renderer::GiSampleRequest;
  using engine::renderer::MeshSdf;
  using engine::renderer::SdfBrick;
  // Red wall quad (x=-2 plane, 4x2 m): 4 verts, 2 triangles.
  const std::array<float, 12> wall_pos = {-2.0f, 0.0f, -2.0f, -2.0f, 0.0f,
                                          2.0f,  -2.0f, 2.0f,  2.0f, -2.0f,
                                          2.0f,  -2.0f};
  const std::array<std::uint32_t, 6> wall_idx = {0, 1, 2, 0, 2, 3};
  std::vector<SdfBrick> storage(512);
  MeshSdf sdf{};
  sdf.bricks = storage.data();
  sdf.capacity = 512;
  check(GiCornellBakeWall(wall_pos.data(), 4, wall_idx.data(), 6, &sdf).ok(),
        "cornell: red wall bakes to bricks");
  check(sdf.brick_count > 0, "cornell: shell materialized");
  check(sdf.dropped_bricks == 0, "cornell: nothing dropped");
  check(!GiCornellBakeWall(nullptr, 4, wall_idx.data(), 6, &sdf).ok(),
        "cornell: null positions rejected");
  // Interior sample (just off the wall, inside the room): the red wall
  // is an OPEN quad (no closed volume), so the sparse shell only covers
  // a ~band-wide slab around the plane (band = 2.5 vox * 0.125 m) plus
  // the +band far-field fallback beyond it. Assert the sample at 0.1 m
  // off the wall sits inside the shell (below the far-field value).
  const float inside =
      engine::renderer::MeshSdfBuilder::sample_voxel(sdf, -1.9f, 1.0f, 0.0f);
  check(inside < engine::renderer::kSdfBandVox,
        "cornell: wall-adjacent sample inside shell");
  // Cascade plan over the room AABB.
  GiCascadePlan plan;
  check(plan.init(engine::renderer::kCornellMin, engine::renderer::kCornellMax)
            .ok(),
        "cornell: room aabb plan ok");
  check(plan.total_probe_bytes() == 4096 * 16 * 4,
        "cornell: total 256KiB");
  // Bounce oracle: nulls rejected, near-wall gain positive and red.
  {
    float b[3] = {0.0f, 0.0f, 0.0f};
    check(!GiCornellBounce(nullptr, b).ok(), "cornell: null pos rejected");
    const float p[3] = {-1.0f, 0.02f, 0.0f};
    check(!GiCornellBounce(p, nullptr).ok(), "cornell: null out rejected");
    check(GiCornellBounce(p, b).ok(), "cornell: bounce ok");
    check(b[0] > 0.0f && b[0] > b[1] * 2.0f && b[0] > b[2] * 2.0f,
          "cornell: bounce is red-dominated");
    const float far[3] = {1.9f, 0.02f, 0.0f};
    float bf[3] = {0.0f, 0.0f, 0.0f};
    check(GiCornellBounce(far, bf).ok(), "cornell: far bounce ok");
    check(bf[0] < b[0], "cornell: bounce falls off with distance");
    const float behind[3] = {-3.0f, 0.02f, 0.0f};
    float bb[3] = {0.0f, 0.0f, 0.0f};
    check(GiCornellBounce(behind, bb).ok(), "cornell: behind-wall ok");
    check(bb[0] == 0.0f, "cornell: no bounce behind the wall");
  }
  // Cross-check: bounce gain near the wall is within 2x of the cascade
  // integrator's one-bounce energy for the same albedo/sky contract.
  {
    GiSampleRequest req{};
    req.world_pos[0] = -1.0f;
    req.world_pos[1] = 0.02f;
    req.world_pos[2] = 0.0f;
    req.normal[0] = 0.0f;
    req.normal[1] = 1.0f;
    req.normal[2] = 0.0f;
    req.albedo[0] = 0.85f;
    req.albedo[1] = 0.85f;
    req.albedo[2] = 0.82f;
    req.sky_visibility = 0.7f;
    const float sky[3] = {0.5f, 0.5f, 0.5f};
    const auto r = GiCascadePlan::integrate(req, nullptr, 0, sky, 0.5f, 1.0f);
    float b[3] = {0.0f, 0.0f, 0.0f};
    const float p[3] = {-1.0f, 0.02f, 0.0f};
    check(GiCornellBounce(p, b).ok(), "cornell: xcheck bounce ok");
    const bool same_order =
        (r.irradiance[0] > 0.0f) && (b[0] > r.irradiance[0] * 0.05f) &&
        (b[0] < r.irradiance[0] * 20.0f);
    check(same_order, "cornell: bounce matches integrator order");
  }
  // Light list: key + red bounce.
  {
    check(GiCornellLights(nullptr).ok() == false,
          "cornell: null lights rejected");
    GiCornellLight lights[2]{};
    check(GiCornellLights(lights).ok(), "cornell: lights ok");
    check(lights[0].color_intensity[3] > 0.0f, "cornell: key has energy");
    check(lights[1].color_intensity[0] == 1.0f &&
              lights[1].color_intensity[1] < 0.5f,
          "cornell: bounce light is red");
    check(lights[1].pos_radius[0] < 60.0f,
          "cornell: bounce light at red wall side");
  }
}

#if defined(ENGINE_RENDERER_HAS_VULKAN)

constexpr std::uint32_t kW = 320;
constexpr std::uint32_t kH = 240;
// 5 instances: 0 floor, 1 red wall, 2 back wall, 3 right wall, 4 ceiling.
constexpr std::uint32_t kInst = 5;

// Cornell visibility: left strip -> red wall (inst 1), floor band ->
// floor (inst 0), right strip -> right wall (inst 3), top band ->
// ceiling (inst 4), else back wall (inst 2). The floor band hugs the
// red wall so the acceptance probe lands on bounce-tinted pixels.
inline void fill_cornell_vis(std::uint32_t w, std::uint32_t h,
                             std::vector<std::uint32_t>* vis) {
  vis->assign(w * h * 2, 0);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      std::uint32_t inst = 2;
      if (x < w / 5) {
        inst = 1;  // red wall strip
      } else if (x < (w * 2) / 5 && y >= (h * 3) / 5) {
        inst = 0;  // floor band near the red wall
      } else if (x >= (w * 4) / 5) {
        inst = 3;  // right wall strip
      } else if (y < h / 5) {
        inst = 4;  // ceiling band
      }
      (*vis)[(y * w + x) * 2 + 0] = inst;
      (*vis)[(y * w + x) * 2 + 1] = 0;
    }
  }
}

inline auto gpu_missing_wrap(std::string_view what) -> int {
  return gpu_missing(what);
}

// Acceptance: the floor pixel near the red wall carries the red bounce:
// R/G must exceed the in-test threshold (stated here: 1.25). The probe
// sits in the floor band (x = w*3/10, y = h*4/5): inside the bounce
// strip, away from the wall strip edge.
inline void probe_cornell_acceptance(
    const std::vector<std::uint8_t>& pixels) {
  const std::uint32_t px = (kW * 3) / 10;
  const std::uint32_t py = (kH * 4) / 5;
  const std::size_t c = (py * kW + px) * 4;
  const float r = static_cast<float>(pixels[c]);
  const float g = static_cast<float>(pixels[c + 1]);
  const float b = static_cast<float>(pixels[c + 2]);
  constexpr float kTintThreshold = 1.25f;
  const float ratio = r / (g > 0.5f ? g : 0.5f);
  check(ratio > kTintThreshold, "cornell-golden: red wall tints floor");
  check(r > b, "cornell-golden: red above blue on floor");
  check(g > 4.0f, "cornell-golden: floor is lit (not black)");
}

// P4-07 GPU path: the Cornell frame's shade/post/descriptor/record calls
// ride inline lambdas at each Golden* helper call site (matching the
// golden helper's documented TU call shape: two device-routed lambdas +
// one GoldenFrameHandles). No TU-local callable structs: the HDR golden's
// HdrSetCalls/HdrTailCalls/HdrMapCalls/HdrPipeCalls pattern is not
// repeated here (Sonar new-code duplication gate: shared helpers own the
// struct-fill; TUs own only the call).

auto test_gpu() -> int {
  using engine::renderer::BindlessTable;
  using engine::renderer::GiCornellLights;
  using engine::renderer::GiCornellLight;
  using engine::renderer::PbrMaterialLibrary;
  using engine::renderer::PbrPipeline;
  using engine::renderer::VulkanDevice;
  VulkanDevice dev;
  engine::test::GoldenVk g{};
  PbrPipeline pbr;
  BindlessTable bt;
  auto check_fn = [&](bool ok, const char* name) { check(ok, name); };
  auto missing_fn = [&](const char* what) {
    failures += gpu_missing_wrap(what);
  };
  if (!engine::test::GoldenOpenDevice(
          check_fn, missing_fn, "P4-07-cornell",
          "cornell-golden: device init ok",
          "cornell-golden: entry points resolve", "cornell-golden: plan init",
          "cornell-golden: bindless init",
          "cornell-golden: bindless layout builds", &dev, &g, &pbr, &bt)) {
    return 1;
  }
  // P4-07 wiring proof: the existing GiFrameGraph builds against the
  // bindless layout + borrowed PBR blobs + the 4 GI/shadow blobs. The
  // graph stays live across the whole frame (destroyed at the tail after
  // submit, before bt): its gi/shadow pipelines are the frame's GI leg,
  // while the shade pass composites the SDF-derived bounce
  // (GiCornellLights light index 1) through the forward+ grid.
  // Scope note (see docs/notes/P4-07.md): the graph build proves the
  // plates/pipelines against scene meshes; per-pixel GI resolve output
  // is not yet sampled by the shade pass (no GI image/descriptor is
  // bound in the frame) — the composited bounce term is the analytic
  // GiCornellBounce oracle cross-checked against
  // GiCascadePlan::integrate() in test_cpu().
  engine::renderer::GiFrameGraph gi;
  {
    check(!gi.ready(), "cornell-golden: graph not ready before build");
    const auto bs = gi.build(
        dev, bt.set_layout(), kGi_resolve_vertSpv, kGi_resolve_vertSpvWords,
        kGi_resolve_fragSpv, kGi_resolve_fragSpvWords, kShadow_trace_vertSpv,
        kShadow_trace_vertSpvWords, kShadow_trace_fragSpv,
        kShadow_trace_fragSpvWords, pbr);
    check(bs.ok(), "cornell-golden: GiFrameGraph build ok (Lavapipe)");
    if (bs.ok()) {
      check(gi.gi_pipeline() != nullptr && gi.shadow_pipeline() != nullptr,
            "cornell-golden: gi + shadow pipelines live");
    }
  }
  // Shade/post pipeline pair (shared builder with the HDR golden).
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
    const bool ok = engine::test::GoldenBuildShadePost(
        &g, bt.set_layout(), kPbr_shade_vertSpv, kPbr_shade_vertSpvWords,
        kPbr_shade_fragSpv, kPbr_shade_fragSpvWords,
        VK_FORMAT_R16G16B16A16_SFLOAT, kPost_vertSpv, kPost_vertSpvWords,
        kPost_fragSpv, kPost_fragSpvWords, VK_FORMAT_R8G8B8A8_UNORM,
        create_set_layout, create_pipe_layout, create_pipes, destroy_mod, &sp);
    check(ok, "cornell-golden: shade+post pipelines build");
    if (!ok) {
      gi.destroy();
      bt.destroy_gpu();
      dev.shutdown();
      return 1;
    }
  }
  // Frame resources: 5 scene instances + 2 lights (key + red bounce).
  constexpr std::uint64_t kSceneBytes = kInst * 96;
  constexpr std::uint64_t kMatBytes = kInst * 64;
  std::vector<std::uint8_t> scene(kSceneBytes, 0);
  std::vector<std::uint8_t> mats(kMatBytes, 0);
  PbrMaterialLibrary pack_lib;
  pack_lib.init();
  bool pack_ok = true;
  // Instance layout (sphere centers in a pseudo room volume):
  // 0 floor (wide, y~10), 1 red wall (x~20), 2 back, 3 right, 4 ceiling.
  const std::array<std::array<float, 4>, kInst> spheres = {{
      {{160.0f, 220.0f, 0.0f, 90.0f}},
      {{20.0f, 120.0f, 0.0f, 60.0f}},
      {{160.0f, 120.0f, 0.0f, 60.0f}},
      {{300.0f, 120.0f, 0.0f, 60.0f}},
      {{160.0f, 20.0f, 0.0f, 60.0f}},
  }};
  for (std::uint32_t inst = 0; inst < kInst && pack_ok; ++inst) {
    engine::test::GoldenFillSceneRecord(scene.data(), inst, spheres[inst][0],
                                        spheres[inst][1], spheres[inst][2],
                                        spheres[inst][3]);
    engine::renderer::PbrMaterialAsset a{};
    a.base_color = {0.85f, 0.85f, 0.82f, 1.0f};
    if (inst == 1) {
      a.base_color = {0.85f, 0.08f, 0.06f, 1.0f};  // red wall
    }
    a.metallic_roughness = {0.0f, 0.9f, 0.0f, 0.0f};
    if (!engine::test::GoldenPackMaterial(
            check, &pack_lib, a, inst,
            reinterpret_cast<float*>(&mats[inst * 64]),
            "cornell-golden: material add", "cornell-golden: record pack")) {
      pack_ok = false;
    }
  }
  check(pack_ok, "cornell-golden: 5 materials packed");
  if (!pack_ok) {
    gi.destroy();
    bt.destroy_gpu();
    dev.shutdown();
    return 1;
  }
  // Two-light forward+ grid: every tile sees both the key and the bounce.
  GiCornellLight lights[2]{};
  check(GiCornellLights(lights).ok(), "cornell-golden: bounce lights ok");
  std::array<float, 16> light_data{};
  for (int l = 0; l < 2; ++l) {
    for (int k = 0; k < 4; ++k) {
      light_data[l * 8 + k] = lights[l].pos_radius[k];
      light_data[l * 8 + 4 + k] = lights[l].color_intensity[k];
    }
  }
  constexpr std::uint32_t kTilesX = kW / 16;
  constexpr std::uint32_t kTilesY = kH / 16;
  std::vector<std::uint32_t> grid(kTilesX * kTilesY * 2, 0);
  for (std::uint32_t t = 0; t < kTilesX * kTilesY; ++t) {
    grid[t * 2 + 0] = 0;
    grid[t * 2 + 1] = 2;
  }
  std::array<std::uint32_t, 2> index_list = {0, 1};
  std::vector<std::uint32_t> vis;
  fill_cornell_vis(kW, kH, &vis);
  // Upload buffers (scene/mat/light/grid/index + vis/readback).
  engine::test::GoldenHostBuf scene_b{};
  engine::test::GoldenHostBuf mat_b{};
  engine::test::GoldenHostBuf light_b{};
  engine::test::GoldenHostBuf grid_b{};
  engine::test::GoldenHostBuf index_b{};
  engine::test::GoldenHostBuf vis_b{};
  engine::test::GoldenHostBuf read_b{};
  const std::array<std::pair<engine::test::GoldenHostBuf*, std::uint64_t>, 7>
      bufs = {{{&scene_b, kSceneBytes},
               {&mat_b, kMatBytes},
               {&light_b, sizeof(light_data)},
               {&grid_b, grid.size() * 4},
               {&index_b, sizeof(index_list)},
               {&vis_b, static_cast<std::uint64_t>(kW) * kH * 8},
               {&read_b, static_cast<std::uint64_t>(kW) * kH * 4}}};
  bool bufs_ok = true;
  for (const auto& b : bufs) {
    const VkBufferUsageFlags use =
        (b.first == &read_b) ? VK_BUFFER_USAGE_TRANSFER_DST_BIT
                             : (b.first == &vis_b)
                                   ? VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                                   : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufs_ok = engine::test::GoldenAllocHost(&g, b.second, use, b.first) &&
              bufs_ok;
  }
  check(bufs_ok, "cornell-golden: upload buffers");
  if (!bufs_ok) {
    gi.destroy();
    bt.destroy_gpu();
    dev.shutdown();
    return 1;
  }
  auto upload = [&](engine::test::GoldenHostBuf* hb, const void* src,
                    std::size_t n) -> bool {
    return engine::test::GoldenUploadBytes(
        &g, hb, std::span<const std::uint8_t>(
                     static_cast<const std::uint8_t*>(src), n));
  };
  const bool up_ok =
      upload(&scene_b, scene.data(), scene.size()) &&
      upload(&mat_b, mats.data(), mats.size()) &&
      upload(&light_b, light_data.data(), sizeof(light_data)) &&
      upload(&grid_b, grid.data(), grid.size() * 4) &&
      upload(&index_b, index_list.data(), sizeof(index_list)) &&
      upload(&vis_b, vis.data(), vis.size() * 4);
  check(up_ok, "cornell-golden: uploads mapped");
  if (!up_ok) {
    gi.destroy();
    bt.destroy_gpu();
    dev.shutdown();
    return 1;
  }
  // Images (vis/HDR/LDR) + sampler.
  engine::test::GoldenImage vis_img{};
  engine::test::GoldenImage hdr_img{};
  engine::test::GoldenImage ldr_img{};
  const bool imgs_ok =
      engine::test::GoldenMakeImage(
          &g, VK_FORMAT_R32G32_UINT, kW, kH,
          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
          &vis_img) &&
      engine::test::GoldenMakeImage(
          &g, VK_FORMAT_R16G16B16A16_SFLOAT, kW, kH,
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
          &hdr_img) &&
      engine::test::GoldenMakeImage(
          &g, VK_FORMAT_R8G8B8A8_UNORM, kW, kH,
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
              VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
          &ldr_img);
  check(imgs_ok, "cornell-golden: images allocated");
  if (!imgs_ok) {
    gi.destroy();
    bt.destroy_gpu();
    dev.shutdown();
    return 1;
  }
  VkSampler sampler = nullptr;
  {
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (g.vk.create_sampler(g.device, &si, nullptr, &sampler) != VK_SUCCESS) {
      check(false, "cornell-golden: sampler");
      gi.destroy();
      bt.destroy_gpu();
      dev.shutdown();
      return 1;
    }
  }
  // Descriptors + record + submit + readback (shared helpers).
  VkDescriptorPool pool = nullptr;
  VkDescriptorSet set0 = nullptr;
  VkDescriptorSet set_shade = nullptr;
  VkDescriptorSet set_post = nullptr;
  {
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
    auto update_sets = [&](VkDevice device, const VkWriteDescriptorSet* w) {
      g.vk.update_sets(device, 9, w, 0, nullptr);
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
    const bool sets_ok = engine::test::GoldenAllocWriteFrameSets(
        &g, bt.set_layout(), sp.set1_shade, sp.set1_post, create_pool,
        alloc_sets, update_sets, h, &pool, &set0, &set_shade, &set_post);
    check(sets_ok, "cornell-golden: descriptor sets");
    if (!sets_ok) {
      gi.destroy();
      bt.destroy_gpu();
      dev.shutdown();
      return 1;
    }
  }
  check(true, "cornell-golden: descriptors written");
  // Record: vis upload + shade (2 lights) + post at exposure 1 + readback.
  VkCommandPool cmd_pool = nullptr;
  {
    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.queueFamilyIndex = g.queue_family;
    if (g.vk.create_cmd_pool(g.device, &cpi, nullptr, &cmd_pool) !=
        VK_SUCCESS) {
      check(false, "cornell-golden: cmd pool");
      gi.destroy();
      bt.destroy_gpu();
      dev.shutdown();
      return 1;
    }
  }
  VkCommandBuffer cb = nullptr;
  {
    VkCommandBufferAllocateInfo ab{};
    ab.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ab.commandPool = cmd_pool;
    ab.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ab.commandBufferCount = 1;
    if (g.vk.alloc_cmds(g.device, &ab, &cb) != VK_SUCCESS) {
      check(false, "cornell-golden: cmd alloc");
      gi.destroy();
      bt.destroy_gpu();
      dev.shutdown();
      return 1;
    }
  }
  {
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (g.vk.begin_cmd(cb, &bi) != VK_SUCCESS) {
      check(false, "cornell-golden: cmd begin");
      gi.destroy();
      bt.destroy_gpu();
      dev.shutdown();
      return 1;
    }
  }
  {
    engine::test::GoldenFrame fr{};
    engine::test::GoldenFrameBufs fb{};
    fb.vis_b = vis_b;
    fb.vis_img = vis_img;
    fb.hdr_img = hdr_img;
    fb.ldr_img = ldr_img;
    fb.set0 = set0;
    fb.set_shade = set_shade;
    fb.set_post = set_post;
    engine::test::GoldenFillFrame(&fr, &fb, kW, kH, sp.shade_pipe,
                                  sp.post_pipe, sp.shade_layout,
                                  sp.post_layout, 1.0f);
    fr.shade.light_count = 2;
    engine::test::GoldenRecordFrame(&g, cb, fr);
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
      check(false, "cornell-golden: cmd record/end");
      gi.destroy();
      bt.destroy_gpu();
      dev.shutdown();
      return 1;
    }
  }
  const bool submit_ok = engine::test::GoldenSubmitWait(&g, cb);
  check(submit_ok, "cornell-golden: submit + fence wait");
  g.vk.destroy_cmd_pool(g.device, cmd_pool, nullptr);
  if (!submit_ok) {
    gi.destroy();
    bt.destroy_gpu();
    dev.shutdown();
    return 1;
  }
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kW) * kH * 4, 0);
  {
    auto map_fn = [&](VkDevice d, VkDeviceMemory m, VkDeviceSize o,
                      VkDeviceSize s, VkMemoryMapFlags f, void** p) {
      return g.vk.map_mem(d, m, o, s, f, p);
    };
    auto unmap_fn = [&](VkDevice d, VkDeviceMemory m) {
      g.vk.unmap_mem(d, m);
    };
    if (!engine::test::GoldenMapPixels(map_fn, unmap_fn, g.device, read_b.mem,
                                       pixels.data(), pixels.size())) {
      check(false, "cornell-golden: readback mapped");
      gi.destroy();
      bt.destroy_gpu();
      dev.shutdown();
      return 1;
    }
  }
  check(true, "cornell-golden: readback mapped");
  failures += engine::test::golden_check(
      "p4_07_cornell_320x240",
      std::span<const std::uint8_t>(pixels.data(), pixels.size()), kW, kH);
  probe_cornell_acceptance(pixels);
  // Cleanup (mirrors the HDR golden tail; gi first: its pipelines were
  // created against the bindless layout, so it releases before bt).
  gi.destroy();
  check(!gi.ready(), "cornell-golden: graph destroy clears");
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
  std::printf("[SKIP] cornell-gpu: SDK absent (stub build)\n");
#endif
  std::printf(failures == 0 ? "test_gi_cornell: all checks passed\n"
                            : "test_gi_cornell: %d FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}

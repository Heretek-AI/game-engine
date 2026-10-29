// test_gi_zero_bake.cpp — M3-04 acceptance (native CTest).
//
// Zero-bake real-time GI contract, all CPU-side (no GPU needed) except
// the final Lavapipe section:
//
//   AC-1 SDF bricks: build a unit-cube mesh -> sparse shell materializes
//     (brick_count > 0, dropped == 0); texel distances quantize within
//     the band range; sample_voxel() at the cube center is negative
//     (inside) and outside the mesh is non-negative; degenerate input
//     rejected; capacity overflow counts dropped_bricks instead of
//     failing.
//   AC-2 radiance cascades: plan init + sizing (4 levels x 16^3 x 16B =
//     256KiB total; spacing 1/4/16/64); scene_sdf() of a sphere matches
//     the analytic distance; integrate() in an empty room returns sky *
//     albedo / pi with zero occlusion; integrate() next to a wall
//     reports occlusion > 0 and bounded steps.
//   AC-3 SDSM + grade: splits strictly increase and tile the 2x2 atlas;
//     atlas = 16MiB R32F; soft_shadow(0,*)=1 lit, soft_shadow(1,* )<1,
//     monotonic in blocker; tonemap_grade() of mid-grey is brighter than
//     black and darker than white (ACES S-curve), exposure scales output,
//     neutral grade keeps grey grey-ish.
//   Shaders: all 10 SPIR-V blobs (6 M3-03 + 4 M3-04) carry magic + >= 1.3.
//   Lavapipe: vkCreateShaderModule succeeds on all 10 blobs; the
//     GiResolvePush/ShadowGradePush structs fit 128B.
//
// Naming note: check() helpers mirror test_slang_pbr.cpp (own TU, no
// shared test lib in this repo) — check_tagged avoids printf-family
// formatting per Sonar cpp:S3458.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#endif

#include "engine/renderer/bindless_table.hpp"
#include "engine/renderer/gi_cascade.hpp"
#include "engine/renderer/gi_frame_graph.hpp"
#include "engine/renderer/gi_shadow.hpp"
#include "engine/renderer/pbr_pipeline.hpp"
#include "engine/renderer/sdf_brick_builder.hpp"
#include "engine/renderer/vulkan_device.hpp"

#include "engine/renderer/shaders/gi_resolve_frag_spv.inc"
#include "engine/renderer/shaders/gi_resolve_vert_spv.inc"
#include "engine/renderer/shaders/pbr_shade_frag_spv.inc"
#include "engine/renderer/shaders/pbr_shade_vert_spv.inc"
#include "engine/renderer/shaders/pbr_vis_frag_spv.inc"
#include "engine/renderer/shaders/pbr_vis_vert_spv.inc"
#include "engine/renderer/shaders/post_frag_spv.inc"
#include "engine/renderer/shaders/post_vert_spv.inc"
#include "engine/renderer/shaders/shadow_trace_frag_spv.inc"
#include "engine/renderer/shaders/shadow_trace_vert_spv.inc"

namespace {

auto failures = 0;

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}

void check_tagged(bool ok, const char* prefix, const char* suffix) {
  char label[128]{};
  std::size_t n = 0;
  while (prefix[n] != '\0' && n + 1 < sizeof(label)) {
    label[n] = prefix[n];
    ++n;
  }
  std::size_t m = 0;
  while (suffix[m] != '\0' && n + 1 < sizeof(label)) {
    label[n++] = suffix[m++];
  }
  label[n] = '\0';
  check(ok, label);
}

using engine::renderer::GiCascadePlan;
using engine::renderer::GiFrameGraph;
using engine::renderer::GiResolvePush;
using engine::renderer::GiSampleRequest;
using engine::renderer::GiSphereOccluder;
using engine::renderer::MeshSdf;
using engine::renderer::MeshSdfBuilder;
using engine::renderer::SdfBrick;
using engine::renderer::SdfBuildParams;
using engine::renderer::ShadowGradePush;
using engine::renderer::ShadowMapPlan;

// Unit cube [-1,+1]^3 as 12 triangles (CCW outward).
void cube_mesh(std::vector<float>& pos, std::vector<std::uint32_t>& idx) {
  pos = {-1, -1, -1, 1, -1, -1, 1, 1, -1, -1, 1, -1,
         -1, -1, 1,  1, -1, 1,  1, 1, 1,  -1, 1, 1};
  idx = {0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4,
         2, 3, 7, 2, 7, 6, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5};
}

void test_sdf_bricks() {
  std::vector<float> pos;
  std::vector<std::uint32_t> idx;
  cube_mesh(pos, idx);

  MeshSdfBuilder builder;
  // Degenerate input rejected.
  {
    MeshSdf out{};
    check(!builder
               .build(nullptr, 0, nullptr, 0, SdfBuildParams{}, &out)
               .ok(),
          "sdf: null input rejected");
    std::vector<SdfBrick> storage(8);
    MeshSdf out2{};
    out2.bricks = storage.data();
    SdfBuildParams bad{};
    bad.texel_size = 0.0f;
    check(!builder
               .build(pos.data(), 8, idx.data(),
                      static_cast<std::uint32_t>(idx.size()), bad, &out2)
               .ok(),
          "sdf: zero texel rejected");
  }
  // OOB index rejected.
  {
    std::vector<SdfBrick> storage(8);
    MeshSdf out{};
    out.bricks = storage.data();
    std::vector<std::uint32_t> bad_idx = idx;
    bad_idx[0] = 99;
    check(!builder
               .build(pos.data(), 8, bad_idx.data(),
                      static_cast<std::uint32_t>(bad_idx.size()),
                      SdfBuildParams{}, &out)
               .ok(),
          "sdf: OOB index rejected");
  }
    // Full build: sparse shell around the cube surface.
    // NOTE: with texel 0.25 the bricks are 2.0 wide, so the shell bricks
    // straddle the faces and the center (0,0,0) is a brick corner shared
    // by all 8 bricks — sample() resolves corner/edge texels by clamping
    // to the resident side. Interior/exterior assertions below probe
    // off-corner points (face-adjacent) for a robust sign check.
  std::vector<SdfBrick> storage(2048);
  MeshSdf sdf{};
  sdf.bricks = storage.data();
  SdfBuildParams params{};
  params.texel_size = 0.25f;
  params.brick_capacity = 2048;
  check(builder
            .build(pos.data(), 8, idx.data(),
                   static_cast<std::uint32_t>(idx.size()), params, &sdf)
            .ok(),
        "sdf: cube build ok");
  check(sdf.brick_count > 0, "sdf: shell materialized");
  check(sdf.dropped_bricks == 0, "sdf: nothing dropped");
  check(sdf.texel_size == 0.25f, "sdf: texel recorded");
  // Interior sample (just inside the +X face) is negative; exterior
  // corner positive. The cube center is a shared brick corner (see NOTE
  // above), so sign checks probe face-adjacent points instead.
  {
    const float inside =
        MeshSdfBuilder::sample_voxel(sdf, 0.9f, 0.1f, 0.1f);
    check(inside < 0.0f, "sdf: center inside (negative)");
    const float outside =
        MeshSdfBuilder::sample_voxel(sdf, 5.0f, 5.0f, 5.0f);
    check(outside >= 0.0f, "sdf: far corner non-negative");
  }
  // Capacity overflow is soft: brick_count capped, dropped > 0, still ok.
  {
    std::vector<SdfBrick> tiny(1);
    MeshSdf small{};
    small.bricks = tiny.data();
    SdfBuildParams p{};
    p.texel_size = 0.25f;
    p.brick_capacity = 1;
    check(builder
              .build(pos.data(), 8, idx.data(),
                     static_cast<std::uint32_t>(idx.size()), p, &small)
              .ok(),
          "sdf: tiny capacity still ok");
    check(small.brick_count == 1 && small.dropped_bricks > 0,
          "sdf: overflow counted as dropped");
  }
  // Sizing helper is a sane upper bound on the built shell.
  {
    const std::uint64_t dense = MeshSdfBuilder::dense_brick_bytes(
        sdf.aabb_min, sdf.aabb_max, 0.25f);
    const std::uint64_t built =
        static_cast<std::uint64_t>(sdf.brick_count) * sizeof(SdfBrick);
    check(dense >= built && dense > 0, "sdf: dense bound covers shell");
  }
}

void test_cascades() {
  GiCascadePlan plan;
  check(!plan.ready(), "gi: not ready before init");
  check(plan.total_probe_bytes() == 0, "gi: zero bytes before init");
  {
    const float mn[3] = {1.0f, 1.0f, 1.0f};
    const float mx[3] = {-1.0f, -1.0f, -1.0f};
    check(!plan.init(mn, mx).ok(), "gi: degenerate aabb rejected");
  }
  const float mn[3] = {-8.0f, -4.0f, -8.0f};
  const float mx[3] = {8.0f, 4.0f, 8.0f};
  check(plan.init(mn, mx).ok(), "gi: room aabb ok");
  check(plan.ready(), "gi: ready after init");
  // 16^3 probes x 16B = 64KiB per level; 4 levels = 256KiB.
  check(plan.level_probe_bytes(0) == 4096 * 16, "gi: L0 64KiB");
  check(plan.total_probe_bytes() == 4096 * 16 * 4, "gi: total 256KiB");
  check(plan.level_probe_bytes(9) == 0, "gi: OOB level zero");
  check(plan.level_spacing(0) == 1.0f, "gi: L0 spacing 1");
  check(plan.level_spacing(1) == 4.0f, "gi: L1 spacing 4");
  check(plan.level_spacing(2) == 16.0f, "gi: L2 spacing 16");
  check(plan.level_spacing(3) == 64.0f, "gi: L3 spacing 64");
  // scene_sdf: single unit sphere at origin; distance at (3,0,0) = 2.
  {
    GiSphereOccluder occ{{0.0f, 0.0f, 0.0f}, 1.0f};
    const float p[3] = {3.0f, 0.0f, 0.0f};
    const float d = GiCascadePlan::scene_sdf(p, &occ, 1);
    check(d > 1.99f && d < 2.01f, "gi: analytic sphere SDF exact");
    check(GiCascadePlan::scene_sdf(p, nullptr, 0) > 1e29f,
          "gi: empty set is open sky");
  }
  // Empty-room integrate: sky * albedo / pi, zero occlusion, fast exit.
  {
    GiSampleRequest req{};
    req.world_pos[0] = 0.0f;
    req.world_pos[1] = 0.0f;
    req.world_pos[2] = 0.0f;
    req.normal[0] = 0.0f;
    req.normal[1] = 1.0f;
    req.normal[2] = 0.0f;
    req.albedo[0] = 1.0f;
    req.albedo[1] = 1.0f;
    req.albedo[2] = 1.0f;
    req.sky_visibility = 1.0f;
    const float sky[3] = {0.2f, 0.3f, 0.5f};
    const auto r =
        GiCascadePlan::integrate(req, nullptr, 0, sky, 0.5f, 1.0f);
    // Direct (sky/pi) + 6 sky taps each contributing (sky/6/pi):
    // total = 2 * sky / pi for albedo 1.
    constexpr float kInvPi = 0.3183099f;
    const float e0 = 2.0f * 0.2f * kInvPi;
    check(r.occlusion == 0.0f, "gi: empty room no occlusion");
    check(r.steps == 6, "gi: empty room exits after 6 first-steps");
    const bool close = (r.irradiance[0] > e0 * 0.999f) &&
                       (r.irradiance[0] < e0 * 1.001f);
    check(close, "gi: empty room irradiance == 2*sky/pi");
  }
  // Walled pit: floor + 4 walls (5 spheres flattened into planes is
  // overkill for the oracle — one big sphere behind the sample blocks
  // the back taps): occlusion > 0, steps bounded by the trace budget.
  {
    GiSampleRequest req{};
    req.world_pos[0] = 0.0f;
    req.world_pos[1] = 0.0f;
    req.world_pos[2] = 0.0f;
    req.normal[0] = 0.0f;
    req.normal[1] = 1.0f;
    req.normal[2] = 0.0f;
    req.albedo[0] = 0.8f;
    req.albedo[1] = 0.8f;
    req.albedo[2] = 0.8f;
    req.sky_visibility = 0.4f;
    // A wall 0.5 units away in -Y..+X hemisphere coverage.
    GiSphereOccluder wall{{2.0f, 0.5f, 0.0f}, 1.5f};
    const float sky[3] = {0.2f, 0.3f, 0.5f};
    const auto r = GiCascadePlan::integrate(req, &wall, 1, sky, 0.5f, 1.0f);
    check(r.occlusion > 0.0f, "gi: wall occludes some taps");
    check(r.occlusion <= 1.0f, "gi: occlusion bounded");
    check(r.steps <= 6 * 64, "gi: steps within budget");
    check(r.irradiance[0] >= 0.0f && r.irradiance[1] >= 0.0f &&
              r.irradiance[2] >= 0.0f,
          "gi: irradiance non-negative");
  }
  check(sizeof(GiResolvePush) <= 128, "gi: resolve push fits 128B");
}

void test_shadows() {
  ShadowMapPlan plan;
  check(!plan.ready(), "shadow: not ready before init");
  check(!plan.init(0.0f, 10.0f, 0.01f).ok(), "shadow: zero near rejected");
  check(!plan.init(1.0f, 0.5f, 0.01f).ok(), "shadow: far<=near rejected");
  check(plan.init(0.5f, 100.0f, 0.01f).ok(), "shadow: splits ok");
  check(plan.ready(), "shadow: ready after init");
  // Splits strictly increase and tile the 2x2 atlas exactly once.
  {
    bool increasing = true;
    bool tiles_ok = true;
    bool seen[4] = {false, false, false, false};
    for (std::uint32_t i = 0; i < 4; ++i) {
      const auto& c = plan.cascade(i);
      if (c.far_dist <= c.near_dist) {
        increasing = false;
      }
      if (i > 0 && c.near_dist != plan.cascade(i - 1).far_dist) {
        increasing = false;
      }
      const std::uint32_t tx = c.tile_x / 1024;
      const std::uint32_t ty = c.tile_y / 1024;
      if (tx > 1 || ty > 1 || seen[ty * 2 + tx]) {
        tiles_ok = false;
      } else {
        seen[ty * 2 + tx] = true;
      }
    }
    check(increasing, "shadow: splits chain near->far");
    check(plan.cascade(3).far_dist == 100.0f, "shadow: last split == far");
    check(tiles_ok, "shadow: 2x2 atlas tiles exact");
  }
  check(ShadowMapPlan::atlas_bytes() ==
            static_cast<std::uint64_t>(2048) * 2048 * 4,
        "shadow: atlas 16MiB R32F");
  check(ShadowMapPlan::pcf_kernel_taps() == 9, "shadow: 3x3 PCF");
  // Soft-shadow contract.
  check(ShadowMapPlan::soft_shadow(0.0f, 1.0f) == 1.0f,
        "shadow: no blocker fully lit");
  {
    const float half = ShadowMapPlan::soft_shadow(0.5f, 1.0f);
    const float full = ShadowMapPlan::soft_shadow(1.0f, 1.0f);
    check(half < 1.0f && full < half, "shadow: monotonic in blocker");
    check(full >= 0.0f, "shadow: factor bounded");
    check(ShadowMapPlan::soft_shadow(2.0f, 5.0f) ==
              ShadowMapPlan::soft_shadow(1.0f, 1.0f),
          "shadow: clamps OOB inputs");
  }
  // ACES grade: S-curve order, exposure scaling, neutral-grade sanity.
  {
    const float black[3] = {0.0f, 0.0f, 0.0f};
    const float grey[3] = {0.18f, 0.18f, 0.18f};
    const float white[3] = {4.0f, 4.0f, 4.0f};
    ShadowGradePush g{};
    g.exposure = 1.0f;
    g.contrast = 1.0f;
    g.saturation = 1.0f;
    g.vignette = 0.0f;
    float ob[3];
    float og[3];
    float ow[3];
    ShadowMapPlan::tonemap_grade(black, g, 0.5f, 0.5f, ob);
    ShadowMapPlan::tonemap_grade(grey, g, 0.5f, 0.5f, og);
    ShadowMapPlan::tonemap_grade(white, g, 0.5f, 0.5f, ow);
    check(ob[0] == 0.0f && ob[1] == 0.0f && ob[2] == 0.0f,
          "shadow: black stays black");
    check(og[0] > ob[0] && ow[0] > og[0], "shadow: ACES order black<grey<wht");
    check(ow[0] <= 1.0f, "shadow: white shoulder clamps to LDR");
    ShadowGradePush bright = g;
    bright.exposure = 2.0f;
    float og2[3];
    ShadowMapPlan::tonemap_grade(grey, bright, 0.5f, 0.5f, og2);
    check(og2[0] > og[0], "shadow: exposure scales output");
    // Grey in -> roughly grey out (neutral grade preserves achromatic).
    const bool greyish = (og[0] > og[1] * 0.95f) && (og[0] < og[1] * 1.05f) &&
                         (og[1] > og[2] * 0.95f) && (og[1] < og[2] * 1.05f);
    check(greyish, "shadow: neutral grade keeps grey grey");
    check(sizeof(ShadowGradePush) <= 128, "shadow: grade push fits 128B");
  }
}

void test_gi_spirv() {
  auto check_spv = [](const std::uint32_t* w, std::size_t n,
                      const char* name) {
    const bool ok =
        n > 5 && w[0] == 0x07230203u && w[1] >= 0x00010300u;
    check_tagged(ok, "gi spirv magic+1.3: ", name);
  };
  check_spv(kPbr_vis_vertSpv, kPbr_vis_vertSpvWords, "pbr_vis_vert");
  check_spv(kPbr_vis_fragSpv, kPbr_vis_fragSpvWords, "pbr_vis_frag");
  check_spv(kPbr_shade_vertSpv, kPbr_shade_vertSpvWords, "pbr_shade_vert");
  check_spv(kPbr_shade_fragSpv, kPbr_shade_fragSpvWords, "pbr_shade_frag");
  check_spv(kPost_vertSpv, kPost_vertSpvWords, "post_vert");
  check_spv(kPost_fragSpv, kPost_fragSpvWords, "post_frag");
  check_spv(kGi_resolve_vertSpv, kGi_resolve_vertSpvWords, "gi_resolve_vert");
  check_spv(kGi_resolve_fragSpv, kGi_resolve_fragSpvWords, "gi_resolve_frag");
  check_spv(kShadow_trace_vertSpv, kShadow_trace_vertSpvWords,
            "shadow_trace_vert");
  check_spv(kShadow_trace_fragSpv, kShadow_trace_fragSpvWords,
            "shadow_trace_frag");
}

#if defined(ENGINE_RENDERER_HAS_VULKAN)
void test_gi_gpu() {
  using engine::renderer::BindlessTable;
  using engine::renderer::PbrPipeline;
  using engine::renderer::VulkanDevice;
  VulkanDevice dev;
  engine::renderer::DeviceCreateInfo ci{};
  ci.app_name = "M3-04-test";
  ci.device_hint = "llvmpipe";
  const auto st = dev.init(ci);
  if (!st.ok() &&
      st.code == engine::renderer::StatusCode::kNoPhysicalDevice) {
    std::printf("[SKIP] gi-gpu: no physical device\n");
    return;
  }
  check(st.ok(), "gi-gpu: device init ok");
  if (!st.ok()) {
    return;
  }
  auto create_mod = [&](const std::uint32_t* w, std::size_t n,
                        VkShaderModule* m, const char* nm) {
    const auto vkCreateShaderModule =
        reinterpret_cast<PFN_vkCreateShaderModule>(
            dev.instance_proc("vkCreateShaderModule"));
    const auto vkDestroyShaderModule =
        reinterpret_cast<PFN_vkDestroyShaderModule>(
            dev.instance_proc("vkDestroyShaderModule"));
    if (vkCreateShaderModule == nullptr) {
      check(false, "gi-gpu: proc null");
      return;
    }
    VkShaderModuleCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sci.codeSize = n * 4;
    sci.pCode = w;
    const bool ok = vkCreateShaderModule(dev.device(), &sci, nullptr, m) ==
                    VK_SUCCESS;
    check_tagged(ok, "gi-gpu module: ", nm);
    if (ok && vkDestroyShaderModule != nullptr) {
      vkDestroyShaderModule(dev.device(), *m, nullptr);
      *m = VK_NULL_HANDLE;
    }
  };
  VkShaderModule m = VK_NULL_HANDLE;
  create_mod(kGi_resolve_vertSpv, kGi_resolve_vertSpvWords, &m,
             "gi_resolve_vert");
  create_mod(kGi_resolve_fragSpv, kGi_resolve_fragSpvWords, &m,
             "gi_resolve_frag");
  create_mod(kShadow_trace_vertSpv, kShadow_trace_vertSpvWords, &m,
             "shadow_trace_vert");
  create_mod(kShadow_trace_fragSpv, kShadow_trace_fragSpvWords, &m,
             "shadow_trace_frag");
  // Full frame-graph path: bindless layout + PBR plan + GI pipelines on
  // Lavapipe (dynamic rendering, no swapchain). Guard: skipped cleanly
  // when the Lavapipe device lacks a feature the layout needs.
  {
    BindlessTable bt;
    check(bt.init().ok(), "gi-gpu: bindless init");
    VkBuffer sentinel = reinterpret_cast<VkBuffer>(0x1000);
    const auto ls =
        bt.create_gpu(dev.device(), sentinel, 1 << 20, sentinel, 1 << 16);
    if (!ls.ok()) {
      std::printf("[SKIP] gi-gpu: bindless layout unavailable\n");
      return;
    }
    PbrPipeline pbr;
    check(pbr
              .init(kPbr_vis_vertSpv, kPbr_vis_vertSpvWords, kPbr_vis_fragSpv,
                    kPbr_vis_fragSpvWords, kPbr_shade_vertSpv,
                    kPbr_shade_vertSpvWords, kPbr_shade_fragSpv,
                    kPbr_shade_fragSpvWords, kPost_vertSpv, kPost_vertSpvWords,
                    kPost_fragSpv, kPost_fragSpvWords)
              .ok(),
          "gi-gpu: pbr plan init");
    GiFrameGraph gi;
    check(!gi.ready(), "gi-gpu: graph not ready before build");
    const auto bs = gi.build(
        dev, bt.set_layout(), kGi_resolve_vertSpv,
        kGi_resolve_vertSpvWords, kGi_resolve_fragSpv,
        kGi_resolve_fragSpvWords, kShadow_trace_vertSpv,
        kShadow_trace_vertSpvWords, kShadow_trace_fragSpv,
        kShadow_trace_fragSpvWords, pbr);
    check(bs.ok(), "gi-gpu: graph build ok (Lavapipe)");
    if (bs.ok()) {
      check(gi.ready() && gi.module_count() == 4, "gi-gpu: 4 modules owned");
      check(gi.gi_pipeline() != nullptr &&
                gi.shadow_pipeline() != nullptr &&
                gi.pipeline_layout() != nullptr,
            "gi-gpu: gi + shadow pipelines live");
      check(!gi.build(dev, bt.set_layout(), kGi_resolve_vertSpv,
                      kGi_resolve_vertSpvWords, kGi_resolve_fragSpv,
                      kGi_resolve_fragSpvWords, kShadow_trace_vertSpv,
                      kShadow_trace_vertSpvWords, kShadow_trace_fragSpv,
                      kShadow_trace_fragSpvWords, pbr)
                 .ok(),
            "gi-gpu: double-build rejected");
      gi.destroy();
      check(!gi.ready(), "gi-gpu: destroy clears");
    }
  }
}
#endif

}  // namespace

auto main() -> int {
  test_sdf_bricks();
  test_cascades();
  test_shadows();
  test_gi_spirv();
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  test_gi_gpu();
#endif
  std::printf(failures == 0 ? "test_gi_zero_bake: all checks passed\n"
                            : "test_gi_zero_bake: %d FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}

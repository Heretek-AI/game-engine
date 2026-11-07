// test_ssao.cpp — P4-09 SSAO blur/composite + toggle proof (task t_2ba3e16a).
//
// CPU contract (no GPU; also runs on stub builds): SsaoComponent 16B,
// SsaoValidate (null/non-finite reject, radius/sigma clamp),
// SsaoBlurHalfSize (160x120 for 320x240, odd ceil-halve, null/zero
// reject), SsaoBlurWeight (same-depth 1.0, far-field decays to 0,
// degenerate sigma delta-only, never NaN), SsaoBlurTap (weight-then-FMA
// accumulate), SsaoBlurAxisCpu (flat field passes through, center weight
// guarantees sum_w>0), SsaoApplyCpu (null reject, enabled multiplies
// then tonemaps, OFF skips the multiply, out clamped [0,1]).
//
// GPU frame (one submit, fence-waited): P4-05 IBL shade (chrome/rough
// split over the synthetic HDR sky, compute pre-pass: irradiance +
// prefilter + 16px CPU-reference LUT upload) into full-res HDR lit ->
// synthetic half-res depth-step + noisy-occlusion upload -> blur H ->
// blur V (ping/pong R32F) -> composite ON into LDR + composite OFF into
// LDR (second record, same lit, enabled=0, no occlusion sampled).
//
// Acceptance: composite-ON vs new golden p4_09_ssao_320x240, composite-OFF
// vs p4_05_ibl_chrome_rough_320x240 (OFF skips the multiply, so the OFF
// frame is the P4-05 shade->tonemap path), plus analytic probes:
// blurred occlusion smooths the single-texel noise dip and keeps the
// depth-step silhouette darker on the far side; the ON frame is darker
// than OFF inside the occluded disc area.
//
// [FAIL] (not [SKIP]) under ENGINE_REQUIRE_GPU=1.
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#endif

#include "engine/renderer/bindless_table.hpp"
#include "engine/renderer/brdf_lut.hpp"
#include "engine/renderer/hdr_camera.hpp"
#include "engine/renderer/pbr_pipeline.hpp"
#include "engine/renderer/skybox_image_based_lighting.hpp"
#include "engine/renderer/ssao.hpp"
#include "engine/renderer/vulkan_device.hpp"

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#include "engine/renderer/shaders/ibl_shade_frag_spv.inc"
#include "engine/renderer/shaders/ibl_shade_vert_spv.inc"
#include "engine/renderer/shaders/irradiance_spv.inc"
#include "engine/renderer/shaders/pbr_shade_frag_spv.inc"
#include "engine/renderer/shaders/pbr_shade_vert_spv.inc"
#include "engine/renderer/shaders/pbr_vis_frag_spv.inc"
#include "engine/renderer/shaders/pbr_vis_vert_spv.inc"
#include "engine/renderer/shaders/post_frag_spv.inc"
#include "engine/renderer/shaders/post_vert_spv.inc"
#include "engine/renderer/shaders/prefilter_spv.inc"
#include "engine/renderer/shaders/skybox_frag_spv.inc"
#include "engine/renderer/shaders/skybox_vert_spv.inc"
#include "engine/renderer/shaders/ssao_blur_frag_spv.inc"
#include "engine/renderer/shaders/ssao_composite_frag_spv.inc"
#include "engine/renderer/shaders/ssao_full_vert_spv.inc"
#include "golden_compare.hpp"
#include "golden_pipe_helper.hpp"
#include "golden_vk_helper.hpp"
#endif

// TU-side engine includes for the helper template instantiations below
// (GoldenOpenDevice names engine types; the helper header stays engine-
// include-free so stub builds compile it without Vulkan types).
#include "engine/renderer/bindless_table.hpp"
#include "engine/renderer/pbr_pipeline.hpp"
#include "engine/renderer/vulkan_device.hpp"

// Shared check scaffold (failures counter + [PASS]/[FAIL] printer lives
// in golden_vk_helper.hpp; Sonar's new-duplication gate counts the
// cross-file verbatim mirror). Thin aliases keep call sites untouched.
namespace {

engine::test::GoldenCheck gold{};

auto check(bool ok, const char* name) -> void { gold.check(ok, name); }

auto& failures = gold.failures;

// --- CPU contract ---------------------------------------------------------

#if defined(ENGINE_RENDERER_HAS_VULKAN)

// [FAIL]-not-[SKIP] under ENGINE_REQUIRE_GPU=1 (rule 11 / P1-01 contract).
// Shared helper (golden_vk_helper.hpp) so GPU TUs don't each carry the
// 16-line getenv fallback (Sonar duplication gate).
auto gpu_missing(std::string_view what) -> int {
  return engine::test::GoldenGpuMissing(what);
}

#endif  // defined(ENGINE_RENDERER_HAS_VULKAN)

void test_cpu_component() {
  using engine::renderer::SsaoBlurHalfSize;
  using engine::renderer::SsaoComponent;
  using engine::renderer::SsaoValidate;
  check(sizeof(SsaoComponent) == 16, "ssao: component 16B");
  check(alignof(SsaoComponent) == 4, "ssao: component 4B aligned");
  const SsaoComponent dflt{};
  check(dflt.enabled == 1, "ssao: default enabled");
  check(dflt.sigma > 0.019f && dflt.sigma < 0.021f, "ssao: default sigma 0.02");
  check(!SsaoValidate(nullptr).ok(), "ssao: null rejected");
  SsaoComponent bad{};
  bad.sigma = std::numeric_limits<float>::quiet_NaN();  // deliberate NaN
  check(!SsaoValidate(&bad).ok(), "ssao: NaN rejected");
  SsaoComponent low{};
  low.radius = -3.0f;
  low.sigma = 0.0f;
  check(SsaoValidate(&low).ok(), "ssao: clamp ok");
  check(low.radius == 0.5f, "ssao: radius floored 0.5");
  check(low.sigma > 0.019f && low.sigma < 0.021f, "ssao: sigma reset default");
  SsaoComponent high{};
  high.radius = 99.0f;
  check(SsaoValidate(&high).ok(), "ssao: high radius ok");
  check(high.radius == 4.0f, "ssao: radius capped 4.0");
  // Half sizes for the 320x240 golden frame: 160x120.
  std::uint32_t hw = 0;
  std::uint32_t hh = 0;
  check(SsaoBlurHalfSize(320, 240, &hw, &hh), "ssao: half size ok");
  check(hw == 160 && hh == 120, "ssao: half is 160x120");
  check(!SsaoBlurHalfSize(320, 240, nullptr, &hh), "ssao: null w rejected");
  check(!SsaoBlurHalfSize(0, 240, &hw, &hh), "ssao: zero w rejected");
  std::uint32_t o1 = 0;
  std::uint32_t o2 = 0;
  check(SsaoBlurHalfSize(1, 1, &o1, &o2) && o1 == 1 && o2 == 1,
        "ssao: 1x1 never 0");
  check(SsaoBlurHalfSize(321, 241, &o1, &o2) && o1 == 161 && o2 == 121,
        "ssao: odd ceil-halves");
}

void test_cpu_blur() {
  using engine::renderer::kSsaoBlurTaps;
  using engine::renderer::SsaoBlurAxisCpu;
  using engine::renderer::SsaoBlurTap;
  using engine::renderer::SsaoBlurWeight;
  check(kSsaoBlurTaps == 5, "ssao: 5 blur taps");
  // Same depth -> full weight.
  check(SsaoBlurWeight(0.5f, 0.5f, 0.02f) == 1.0f, "ssao: same depth w=1");
  // Far field (6 sigma) -> ~0.
  const float far = SsaoBlurWeight(0.2f, 0.8f, 0.02f);
  check(far == 0.0f, "ssao: far depth w=0");
  // One sigma -> exp(-1).
  const float one = SsaoBlurWeight(0.5f, 0.52f, 0.02f);
  check(std::fabs(one - 0.36787944f) < 1e-6f, "ssao: 1-sigma w=exp(-1)");
  // Degenerate sigma: delta-only, never NaN.
  check(SsaoBlurWeight(0.5f, 0.5f, 0.0f) == 1.0f, "ssao: sigma0 equal w=1");
  check(SsaoBlurWeight(0.5f, 0.6f, 0.0f) == 0.0f, "ssao: sigma0 diff w=0");
  const float nan_w = SsaoBlurWeight(std::numeric_limits<float>::quiet_NaN(),
                                     0.5f, 0.02f);
  check(nan_w >= 0.0f && nan_w <= 1.0f, "ssao: NaN depth bounded");
  // Tap: null accumulators are a safe no-op.
  SsaoBlurTap(0.5f, 0.5f, 0.5f, 0.02f, nullptr, nullptr);
  check(true, "ssao: null tap safe");
  float sum_w = 0.0f;
  float sum_o = 0.0f;
  SsaoBlurTap(0.75f, 0.5f, 0.5f, 0.02f, &sum_w, &sum_o);
  check(sum_w == 1.0f && sum_o == 0.75f, "ssao: tap accumulates w*o");
  // Axis: flat field passes through exactly.
  const float flat_occ[5] = {0.6f, 0.6f, 0.6f, 0.6f, 0.6f};
  const float flat_dpt[5] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
  check(SsaoBlurAxisCpu(flat_occ, flat_dpt, 0.02f) == 0.6f,
        "ssao: flat blur passthrough");
  // Axis: single dark tap is smoothed (result between tap and field).
  const float dip_occ[5] = {0.6f, 0.6f, 0.1f, 0.6f, 0.6f};
  const float dip = SsaoBlurAxisCpu(dip_occ, flat_dpt, 0.02f);
  check(dip > 0.1f && dip < 0.6f, "ssao: blur smooths single dip");
  // Axis: depth edge isolates (far taps get ~0 weight, center dominates).
  const float edge_dpt[5] = {0.8f, 0.8f, 0.2f, 0.8f, 0.8f};
  const float edge_occ[5] = {0.0f, 0.0f, 0.9f, 0.0f, 0.0f};
  check(std::fabs(SsaoBlurAxisCpu(edge_occ, edge_dpt, 0.02f) - 0.9f) < 1e-5f,
        "ssao: depth edge isolates center");
}

void test_cpu_apply() {
  using engine::renderer::SsaoApplyCpu;
  using engine::renderer::SsaoBlurPush;
  using engine::renderer::SsaoCompositePush;
  check(sizeof(SsaoBlurPush) == 16, "ssao: blur push 16B");
  check(sizeof(SsaoCompositePush) == 16, "ssao: composite push 16B");
  check(!SsaoApplyCpu(nullptr, 1.0f, 1, 1.0f, nullptr), "ssao: null rejected");
  // ON: occlusion multiplies before tonemap.
  const float lit[3] = {2.0f, 2.0f, 2.0f};
  float on[3] = {0.0f, 0.0f, 0.0f};
  float half_ref[3] = {0.0f, 0.0f, 0.0f};
  check(SsaoApplyCpu(lit, 0.5f, 1, 1.0f, on), "ssao: apply ON ok");
  const float half_lit[3] = {1.0f, 1.0f, 1.0f};
  check(engine::renderer::HdrTonemapCpu(half_lit, 1.0f, half_ref),
        "ssao: tonemap ref ok");
  check(std::fabs(on[0] - half_ref[0]) < 1e-5f, "ssao: ON == tonemap(lit*occ)");
  // OFF: identical to the plain tonemap path (multiply skipped).
  float off[3] = {0.0f, 0.0f, 0.0f};
  float plain[3] = {0.0f, 0.0f, 0.0f};
  check(SsaoApplyCpu(lit, 0.25f, 0, 1.0f, off), "ssao: apply OFF ok");
  check(engine::renderer::HdrTonemapCpu(lit, 1.0f, plain),
        "ssao: plain tonemap ok");
  check(std::fabs(off[0] - plain[0]) < 1e-6f &&
            std::fabs(off[1] - plain[1]) < 1e-6f,
        "ssao: OFF matches plain tonemap");
  // Occlusion clamps [0,1]; out clamps [0,1].
  float neg[3] = {0.0f, 0.0f, 0.0f};
  float over[3] = {0.0f, 0.0f, 0.0f};
  check(SsaoApplyCpu(lit, -5.0f, 1, 1.0f, neg), "ssao: neg occ ok");
  check(neg[0] == 0.0f, "ssao: neg occ clamps black");
  check(SsaoApplyCpu(lit, 99.0f, 1, 1.0f, over), "ssao: over occ ok");
  check(over[0] >= 0.0f && over[0] <= 1.0f, "ssao: out clamped [0,1]");
}

void test_cpu() {
  test_cpu_component();
  test_cpu_blur();
  test_cpu_apply();
}

#if defined(ENGINE_RENDERER_HAS_VULKAN)

constexpr std::uint32_t kW = 320;
constexpr std::uint32_t kH = 240;
constexpr std::uint32_t kHw = 160;  // ceil-halve of 320x240
constexpr std::uint32_t kHh = 120;

// Same synthetic HDR sky as the P4-05 IBL TU (analytic: bright zenith
// band + warm horizon, red up to ~4). Shared by the GPU upload and the
// shade path, so comparisons are exact by construction.
inline void ssao_sky(float dy, float* rgb) {
  const float up = dy * 0.5f + 0.5f;
  rgb[0] = 0.5f + 3.5f * up * up;
  rgb[1] = 0.4f + 1.2f * up;
  rgb[2] = 0.3f + 0.6f * (1.0f - up);
}

// Compute entry points beyond the shared GoldenVkProcs table: alias the
// shared GoldenComputeProcs (same four leftovers the P4-05 family loads).
using SsaoComputeVk = engine::test::GoldenComputeProcs;

template <typename Loader>
void load_ssao_compute_procs(Loader loader, SsaoComputeVk* p) {
  engine::test::GoldenLoadComputeProcs(loader, p);
}

// Test-owned set-1 layout for the ssao shaders: 3 bindings
// (0 SAMPLED_IMAGE, 1 SAMPLER, 2 SAMPLED_IMAGE), fragment stage.
// Matches ssao_blur.frag + ssao_composite.frag (set 1); the TU binds it
// at firstSet 1 with an empty set 0 so the compiled set indices link.
inline auto make_ssao_set1(engine::test::GoldenVk* g,
                           VkDescriptorSetLayout* out) -> bool {
  VkDescriptorSetLayoutBinding b[3]{};
  b[0].binding = 0;
  b[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  b[0].descriptorCount = 1;
  b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  b[1].binding = 1;
  b[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  b[1].descriptorCount = 1;
  b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  b[2].binding = 2;
  b[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  b[2].descriptorCount = 1;
  b[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo li{};
  li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  li.bindingCount = 3;
  li.pBindings = b;
  return g->vk.create_set_layout(g->device, &li, nullptr, out) == VK_SUCCESS;
}

// Empty set-0 layout (no bindings): the ssao shaders declare set 1, so
// the pipeline layout needs a set-0 placeholder at index 0.
inline auto make_empty_set0(engine::test::GoldenVk* g,
                            VkDescriptorSetLayout* out) -> bool {
  VkDescriptorSetLayoutCreateInfo li{};
  li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  li.bindingCount = 0;
  return g->vk.create_set_layout(g->device, &li, nullptr, out) == VK_SUCCESS;
}

inline auto make_per_mip_view(engine::test::GoldenVk* g, VkImage img,
                              std::uint32_t mip, VkImageView* out) -> bool {
  return engine::test::GoldenMakeMipView(g, img, mip, out);
}

auto test_gpu() -> int {
  using engine::renderer::VulkanDevice;
  constexpr std::uint32_t kSrc = 64;     // source cube edge
  constexpr std::uint32_t kIrr = 32;     // irradiance edge
  constexpr std::uint32_t kPref = 32;    // prefilter base edge (6 mips)
  constexpr std::uint32_t kPrefMips = 6;  // 32->16->8->4->2->1
  constexpr float kSigma = 0.02f;
  constexpr float kExposure = 1.0f;

  VulkanDevice dev;
  engine::test::GoldenVk g{};
  // Failures counter hook for the missing-device path (fail-closed under
  // ENGINE_REQUIRE_GPU=1 via the shared GoldenGpuMissing helper).
  auto missing_fn = [&](const char* what) {
    failures += gpu_missing(what);
  };
  auto check_fn = [&](bool ok, const char* name) { check(ok, name); };
  const bool vk_ok = engine::test::GoldenOpenComputeDevice(
      check_fn, missing_fn, "P4-09-ssao", "ssao-gpu: no physical device",
      "ssao-gpu: device init ok", "ssao-gpu: entry points resolve", &dev, &g);
  if (!vk_ok) {
    return 1;
  }
  auto loader = [&](const char* name) -> void* {
    void* p = dev.proc(name);
    return (p != nullptr) ? p : dev.instance_proc(name);
  };
  SsaoComputeVk cv{};
  load_ssao_compute_procs(loader, &cv);
  const bool comp_ok =
      g.vk.create_compute != nullptr && cv.destroy_pipe != nullptr &&
      cv.cmd_bind_pipe != nullptr && g.vk.cmd_dispatch != nullptr &&
      cv.cmd_bind_sets != nullptr && cv.cmd_push != nullptr;
  check(comp_ok, "ssao-gpu: compute procs resolve");
  if (!comp_ok) {
    dev.shutdown();
    return 1;
  }

  check(kIrradianceSpv[0] == 0x07230203u, "ssao-gpu: irradiance spv magic");
  check(kPrefilterSpv[0] == 0x07230203u, "ssao-gpu: prefilter spv magic");
  check(kIbl_shade_vertSpv[0] == 0x07230203u, "ssao-gpu: ibl vert magic");
  check(kIbl_shade_fragSpv[0] == 0x07230203u, "ssao-gpu: ibl frag magic");
  check(kSsao_blur_fragSpv[0] == 0x07230203u, "ssao-gpu: blur frag magic");
  check(kSsao_composite_fragSpv[0] == 0x07230203u,
        "ssao-gpu: composite frag magic");
  check(kSsao_full_vertSpv[0] == 0x07230203u, "ssao-gpu: full vert magic");

  // Analytic source cube (same math as the P4-05 TU).
  std::vector<float> src_px;
  src_px.resize(static_cast<std::size_t>(kSrc) * kSrc * 4 * 6);
  for (std::uint32_t face = 0; face < 6; ++face) {
    for (std::uint32_t y = 0; y < kSrc; ++y) {
      for (std::uint32_t x = 0; x < kSrc; ++x) {
        float dir[3] = {0.0f, 0.0f, 0.0f};
        if (!engine::renderer::SkyboxFaceDir(face, x, y, kSrc, dir)) {
          continue;
        }
        float rgb[3] = {0.0f, 0.0f, 0.0f};
        ssao_sky(dir[1], rgb);
        const std::size_t i =
            (static_cast<std::size_t>(face) * kSrc * kSrc +
             static_cast<std::size_t>(y) * kSrc + x) *
            4;
        src_px[i + 0] = rgb[0];
        src_px[i + 1] = rgb[1];
        src_px[i + 2] = rgb[2];
        src_px[i + 3] = 1.0f;
      }
    }
  }

  // Synthetic half-res depth step (near left 0.2, far right 0.8) +
  // noisy occlusion (0.55 base, single-texel 0.1 dip at center): the
  // blur must smooth the dip while the depth edge keeps halves apart.
  std::vector<float> depth_px(static_cast<std::size_t>(kHw) * kHh, 0.0f);
  std::vector<float> occ_px(static_cast<std::size_t>(kHw) * kHh, 0.0f);
  const std::uint32_t dip_x = kHw / 2;
  const std::uint32_t dip_y = kHh / 2;
  for (std::uint32_t y = 0; y < kHh; ++y) {
    for (std::uint32_t x = 0; x < kHw; ++x) {
      const std::size_t i = static_cast<std::size_t>(y) * kHw + x;
      depth_px[i] = (x < kHw / 2) ? 0.2f : 0.8f;
      occ_px[i] = (x == dip_x && y == dip_y) ? 0.1f : 0.55f;
    }
  }

  engine::test::GoldenHostBuf up_b{};
  engine::test::GoldenHostBuf read_irr_b{};
  engine::test::GoldenHostBuf read_pref_b{};
  engine::test::GoldenHostBuf read_on_b{};
  engine::test::GoldenHostBuf read_off_b{};
  engine::test::GoldenHostBuf depth_up_b{};
  engine::test::GoldenHostBuf occ_up_b{};
  engine::test::GoldenHostBuf read_blur_b{};
  engine::test::GoldenImage src_img{};
  engine::test::GoldenImage irr_img{};
  engine::test::GoldenImage pref_img{};
  engine::test::GoldenImage lut_img{};
  engine::test::GoldenImage lit_img{};
  engine::test::GoldenImage depth_img{};
  engine::test::GoldenImage occ_img{};
  engine::test::GoldenImage blur_a_img{};
  engine::test::GoldenImage blur_b_img{};
  engine::test::GoldenImage ldr_on_img{};
  engine::test::GoldenImage ldr_off_img{};
  VkSampler cube_sampler = nullptr;
  VkSampler lut_sampler = nullptr;
  VkSampler lin_sampler = nullptr;
  // Nearest sampler for the blur taps: exact texelFetch-equivalent taps
  // so the GPU pass matches the CPU 5-tap reference bit-near.
  VkSampler near_sampler = nullptr;

  const std::uint64_t src_bytes =
      static_cast<std::uint64_t>(kSrc) * kSrc * 16 * 6;
  const std::uint64_t irr_bytes =
      static_cast<std::uint64_t>(kIrr) * kIrr * 16 * 6;
  const std::uint64_t pref_bytes =
      static_cast<std::uint64_t>(kPref) * kPref * 16 * 6;
  constexpr std::uint32_t kLutEdge = 16;
  const std::uint64_t lut_bytes =
      static_cast<std::uint64_t>(kLutEdge) * kLutEdge * 8;
  const std::uint64_t up_bytes = src_bytes + lut_bytes;
  const std::uint64_t half_bytes =
      static_cast<std::uint64_t>(kHw) * kHh * 4;
  const std::uint64_t ldr_bytes = static_cast<std::uint64_t>(kW) * kH * 4;

  bool ok = engine::test::GoldenAllocHost(
                  &g, up_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &up_b) &&
            engine::test::GoldenAllocHost(
                &g, irr_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &read_irr_b) &&
            engine::test::GoldenAllocHost(
                &g, pref_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                &read_pref_b) &&
            engine::test::GoldenAllocHost(
                &g, ldr_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &read_on_b) &&
            engine::test::GoldenAllocHost(
                &g, ldr_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &read_off_b) &&
            engine::test::GoldenAllocHost(
                &g, half_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                &depth_up_b) &&
            engine::test::GoldenAllocHost(
                &g, half_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &occ_up_b) &&
            engine::test::GoldenAllocHost(
                &g, half_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                &read_blur_b);
  check(ok, "ssao-gpu: staging buffers");
  if (ok) {
    ok = engine::test::GoldenUploadBytes(
        &g, &up_b,
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(src_px.data()),
            src_px.size() * sizeof(float)));
    check(ok, "ssao-gpu: source uploaded to staging");
  }
  if (ok) {
    ok = engine::test::GoldenUploadBytes(
        &g, &depth_up_b,
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(depth_px.data()),
            depth_px.size() * sizeof(float)));
    check(ok, "ssao-gpu: depth uploaded to staging");
  }
  if (ok) {
    ok = engine::test::GoldenUploadBytes(
        &g, &occ_up_b,
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(occ_px.data()),
            occ_px.size() * sizeof(float)));
    check(ok, "ssao-gpu: occlusion uploaded to staging");
  }
  // IBL cube chain (compute pre-pass fills irradiance + all prefilter
  // mips, shade samples the whole-chain CUBE views): one builder for all
  // three, one sample view each — mirrors make_mip_cube in the P4-05 TU.
  auto make_ibl_cube = [&](std::uint32_t edge, std::uint32_t mips,
                           engine::test::GoldenImage* out) -> bool {
    return engine::test::GoldenMakeCube(
        &g, edge, mips,
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        out);
  };
  if (ok) {
    ok = make_ibl_cube(kSrc, 1, &src_img) && make_ibl_cube(kIrr, 1, &irr_img) &&
         make_ibl_cube(kPref, kPrefMips, &pref_img);
    check(ok, "ssao-gpu: cube images");
  }
  VkImageView src_sample = nullptr;
  VkImageView irr_sample = nullptr;
  VkImageView pref_sample = nullptr;
  if (ok) {
    // One builder for the three IBL cube sample views (whole-chain CUBE;
    // per-mip storage uses GoldenMakeMipView above). The edge/mips table
    // keeps the three calls to one update site.
    const VkImage cubes[3] = {src_img.img, irr_img.img, pref_img.img};
    const std::uint32_t cube_mips[3] = {1, 1, kPrefMips};
    VkImageView* cube_views[3] = {&src_sample, &irr_sample, &pref_sample};
    for (int k = 0; k < 3 && ok; ++k) {
      ok = engine::test::GoldenMakeCubeSampleView(&g, cubes[k], cube_mips[k],
                                                  cube_views[k]);
    }
    check(ok, "ssao-gpu: cube sample views");
  }
  if (ok) {
    ok = engine::test::GoldenMakeLutImage(&g, kLutEdge, &lut_img);
    check(ok, "ssao-gpu: LUT image");
  }
  if (ok) {
    ok = engine::test::GoldenMakeImage(
             &g, VK_FORMAT_R32G32B32A32_SFLOAT, kW, kH,
             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
             &lit_img) &&
         engine::test::GoldenMakeImage(
             &g, VK_FORMAT_R32_SFLOAT, kHw, kHh,
             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
             &depth_img) &&
         engine::test::GoldenMakeImage(
             &g, VK_FORMAT_R32_SFLOAT, kHw, kHh,
             VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
             &occ_img) &&
         engine::test::GoldenMakeImage(
             &g, VK_FORMAT_R32_SFLOAT, kHw, kHh,
             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
             &blur_a_img) &&
         engine::test::GoldenMakeImage(
             &g, VK_FORMAT_R32_SFLOAT, kHw, kHh,
             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
             &blur_b_img) &&
         engine::test::GoldenMakeImage(&g, VK_FORMAT_R8G8B8A8_UNORM, kW, kH,
                                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                       &ldr_on_img) &&
         engine::test::GoldenMakeImage(&g, VK_FORMAT_R8G8B8A8_UNORM, kW, kH,
                                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                       &ldr_off_img);
    check(ok, "ssao-gpu: frame images");
  }
  if (ok) {
    // Samplers: cube (linear, LOD-capable) + LUT (linear clamp) + lin
    // (bilinear upscale at composite) — all linear-clamp siblings — plus
    // near (exact taps for the blur passes). One lambda covers the three
    // linear-clamp creations; cube/near keep their own settings.
    auto make_linear_clamp = [&](VkSampler* out) -> bool {
      VkSamplerCreateInfo ci{};
      ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
      ci.magFilter = VK_FILTER_LINEAR;
      ci.minFilter = VK_FILTER_LINEAR;
      ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      return g.vk.create_sampler(g.device, &ci, nullptr, out) == VK_SUCCESS;
    };
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.minLod = 0.0f;
    si.maxLod = 12.0f;
    ok = g.vk.create_sampler(g.device, &si, nullptr, &cube_sampler) ==
         VK_SUCCESS;
    if (ok) {
      ok = make_linear_clamp(&lut_sampler) && make_linear_clamp(&lin_sampler);
    }
    if (ok) {
      // near_sampler = exact-tap sampling for the blur passes.
      VkSamplerCreateInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
      bi.magFilter = VK_FILTER_NEAREST;
      bi.minFilter = VK_FILTER_NEAREST;
      bi.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      bi.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      bi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      ok = g.vk.create_sampler(g.device, &bi, nullptr, &near_sampler) ==
           VK_SUCCESS;
    }
    check(ok, "ssao-gpu: samplers");
  }

  // Modules: compute (irradiance/prefilter) + shade pair + ssao triple.
  VkShaderModule irr_mod = nullptr;
  VkShaderModule pref_mod = nullptr;
  VkShaderModule shade_vert_mod = nullptr;
  VkShaderModule shade_frag_mod = nullptr;
  VkShaderModule ssao_vert_mod = nullptr;
  VkShaderModule blur_frag_mod = nullptr;
  VkShaderModule comp_frag_mod = nullptr;
  VkDescriptorSetLayout comp_layout = nullptr;
  VkPipelineLayout irr_pipe_layout = nullptr;
  VkPipelineLayout pref_pipe_layout = nullptr;
  VkPipeline irr_pipe = nullptr;
  VkPipeline pref_pipe = nullptr;
  VkDescriptorPool comp_pool = nullptr;
  VkDescriptorSet irr_set = nullptr;
  std::array<VkDescriptorSet, 6> pref_sets{};
  VkImageView irr_mip0_view = nullptr;
  std::array<VkImageView, 6> pref_mip_views{};
  // Shade set layout + pipe (set 0: 4 combined cubemap/LUT samplers).
  VkDescriptorSetLayout shade_layout = nullptr;
  VkPipelineLayout shade_pipe_layout = nullptr;
  VkPipeline shade_pipe = nullptr;
  VkDescriptorPool shade_pool = nullptr;
  VkDescriptorSet shade_set = nullptr;
  // SSAO set-1 layouts (blur + composite share the 3-binding shape) +
  // empty set-0 placeholder + pipes + pool/sets.
  VkDescriptorSetLayout ssao_set1 = nullptr;
  VkDescriptorSetLayout ssao_empty0 = nullptr;
  VkPipelineLayout blur_pipe_layout = nullptr;
  VkPipelineLayout comp_pipe_layout = nullptr;
  VkPipeline blur_pipe = nullptr;
  VkPipeline comp_pipe = nullptr;
  VkDescriptorPool ssao_pool = nullptr;
  VkDescriptorSet blur_h_set = nullptr;
  VkDescriptorSet blur_v_set = nullptr;
  VkDescriptorSet comp_on_set = nullptr;
  VkDescriptorSet comp_off_set = nullptr;

  if (ok) {
    ok = engine::test::GoldenMakeComputeSetLayout(&g, &comp_layout);
    check(ok, "ssao-gpu: compute set layout");
  }
  // Compute pipe layouts (irradiance push 8B, prefilter push 16B) + the
  // two compute pipes + descriptor sets for the irradiance set and one
  // set per prefilter mip (sets bound mid-record must not be rewritten —
  // each mip gets its own, as in the P4-05 TU).
  auto make_compute_pipe_layout = [&](std::uint32_t push_bytes,
                                      VkPipelineLayout* out,
                                      const char* label) -> bool {
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pc.size = push_bytes;
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &comp_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    ok = g.vk.create_pipe_layout(g.device, &pli, nullptr, out) == VK_SUCCESS;
    check(ok, label);
    return ok;
  };
  if (ok) {
    ok = make_compute_pipe_layout(8, &irr_pipe_layout,
                                 "ssao-gpu: irradiance pipeline layout") &&
         make_compute_pipe_layout(16, &pref_pipe_layout,
                                  "ssao-gpu: prefilter pipeline layout");
  }
  if (ok) {
    ok = engine::test::GoldenMakeModule(&g, kIrradianceSpv,
                                        kIrradianceSpvWords, &irr_mod) &&
         engine::test::GoldenMakeModule(&g, kPrefilterSpv,
                                        kPrefilterSpvWords, &pref_mod) &&
         engine::test::GoldenMakeModule(&g, kIbl_shade_vertSpv,
                                        kIbl_shade_vertSpvWords,
                                        &shade_vert_mod) &&
         engine::test::GoldenMakeModule(&g, kIbl_shade_fragSpv,
                                        kIbl_shade_fragSpvWords,
                                        &shade_frag_mod) &&
         engine::test::GoldenMakeModule(&g, kSsao_full_vertSpv,
                                        kSsao_full_vertSpvWords,
                                        &ssao_vert_mod) &&
         engine::test::GoldenMakeModule(&g, kSsao_blur_fragSpv,
                                        kSsao_blur_fragSpvWords,
                                        &blur_frag_mod) &&
         engine::test::GoldenMakeModule(&g, kSsao_composite_fragSpv,
                                        kSsao_composite_fragSpvWords,
                                        &comp_frag_mod);
    check(ok, "ssao-gpu: shader modules");
  }
  if (ok) {
    auto make_compute_pipe = [&](VkShaderModule mod, VkPipelineLayout layout,
                                 VkPipeline* out) -> bool {
      VkComputePipelineCreateInfo cpi{};
      cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
      cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      cpi.stage.module = mod;
      cpi.stage.pName = "main";
      cpi.layout = layout;
      return g.vk.create_compute(g.device, nullptr, 1, &cpi, nullptr, out) ==
             VK_SUCCESS;
    };
    ok = make_compute_pipe(irr_mod, irr_pipe_layout, &irr_pipe) &&
         make_compute_pipe(pref_mod, pref_pipe_layout, &pref_pipe);
    check(ok, "ssao-gpu: compute pipelines");
  }
  // Compute pre-pass resources: one pool holding the irradiance set + one
  // set per prefilter mip (sets bound mid-record must not be rewritten —
  // each mip gets its own, as in the P4-05 TU), then per-mip storage
  // views, then the descriptor writes wiring source sampler + storage.
  auto make_one_plus_mips_pool = [&](VkDescriptorType type0,
                                     std::uint32_t count0,
                                     VkDescriptorType type1,
                                     std::uint32_t count1,
                                     std::uint32_t max_sets,
                                     VkDescriptorSetLayout layout,
                                     VkDescriptorPool* pool_out,
                                     const char* label) -> bool {
    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type = type0;
    sizes[0].descriptorCount = count0;
    sizes[1].type = type1;
    sizes[1].descriptorCount = count1;
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = max_sets;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    if (g.vk.create_pool(g.device, &pi, nullptr, pool_out) != VK_SUCCESS) {
      check(false, label);
      return false;
    }
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = *pool_out;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &layout;
    bool sets_ok = g.vk.alloc_sets(g.device, &ai, &irr_set) == VK_SUCCESS;
    for (std::uint32_t m = 0; m < kPrefMips && sets_ok; ++m) {
      sets_ok = g.vk.alloc_sets(g.device, &ai, &pref_sets[m]) == VK_SUCCESS;
    }
    check(sets_ok, label);
    return sets_ok;
  };
  if (ok) {
    ok = make_one_plus_mips_pool(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                 1 + 6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 + 6,
                                 1 + 6, comp_layout, &comp_pool,
                                 "ssao-gpu: compute descriptor sets");
  }
  if (ok) {
    ok = make_per_mip_view(&g, irr_img.img, 0, &irr_mip0_view);
    for (std::uint32_t m = 0; m < kPrefMips && ok; ++m) {
      ok = make_per_mip_view(&g, pref_img.img, m, &pref_mip_views[m]);
    }
    check(ok, "ssao-gpu: prefilter per-mip views");
  }
  if (ok) {
    // Irradiance + mip-0 prefilter writes share one descriptor template
    // (source sampler at 0, GENERAL storage at 1); each mip set gets its
    // own copy (sets bound mid-record must not be rewritten).
    auto write_compute_set = [&](VkDescriptorSet dst, VkImageView stor_view) {
      VkDescriptorImageInfo src_info{};
      src_info.sampler = cube_sampler;
      src_info.imageView = src_sample;
      src_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      VkDescriptorImageInfo stor_info{};
      stor_info.imageView = stor_view;
      stor_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      VkWriteDescriptorSet w[2]{};
      w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w[0].dstSet = dst;
      w[0].dstBinding = 0;
      w[0].descriptorCount = 1;
      w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      w[0].pImageInfo = &src_info;
      w[1] = w[0];
      w[1].dstBinding = 1;
      w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      w[1].pImageInfo = &stor_info;
      g.vk.update_sets(g.device, 2, w, 0, nullptr);
    };
    write_compute_set(irr_set, irr_mip0_view);
    for (std::uint32_t m = 0; m < kPrefMips; ++m) {
      write_compute_set(pref_sets[m], pref_mip_views[m]);
    }
    check(true, "ssao-gpu: compute descriptors written");
  }
  // Shade layout + pipeline (mirrors the P4-05 TU: 4 combined samplers
  // at set 0, 40B inv-view push, fullscreen-triangle pipe).
  if (ok) {
    VkDescriptorSetLayoutBinding binds[4]{};
    for (int k = 0; k < 4; ++k) {
      binds[k].binding = static_cast<std::uint32_t>(k);
      binds[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      binds[k].descriptorCount = 1;
      binds[k].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 4;
    li.pBindings = binds;
    ok = g.vk.create_set_layout(g.device, &li, nullptr, &shade_layout) ==
         VK_SUCCESS;
    check(ok, "ssao-gpu: shade set layout");
  }
  if (ok) {
    VkPushConstantRange pc{};
    pc.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.size = 40;
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &shade_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    ok = g.vk.create_pipe_layout(g.device, &pli, nullptr, &shade_pipe_layout) ==
         VK_SUCCESS;
    check(ok, "ssao-gpu: shade pipeline layout");
  }
  if (ok) {
    ok = engine::test::GoldenBuildPipe(g.vk.create_pipes, g.device,
                                       shade_vert_mod, shade_frag_mod,
                                       VK_FORMAT_R32G32B32A32_SFLOAT,
                                       shade_pipe_layout, &shade_pipe);
    check(ok, "ssao-gpu: shade pipeline");
  }
  if (ok) {
    // Shade set pool: one set of 4 combined samplers (mirrors the P4-05
    // TU pool shape; single-set alloc folded into the create branch).
    auto make_single_pool = [&](VkDescriptorType type, std::uint32_t count,
                                VkDescriptorSetLayout layout,
                                VkDescriptorPool* pool_out,
                                VkDescriptorSet* set_out,
                                const char* label) -> bool {
      VkDescriptorPoolSize size{};
      size.type = type;
      size.descriptorCount = count;
      VkDescriptorPoolCreateInfo pi{};
      pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
      pi.maxSets = 1;
      pi.poolSizeCount = 1;
      pi.pPoolSizes = &size;
      if (g.vk.create_pool(g.device, &pi, nullptr, pool_out) != VK_SUCCESS) {
        check(false, label);
        return false;
      }
      VkDescriptorSetAllocateInfo ai{};
      ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      ai.descriptorPool = *pool_out;
      ai.descriptorSetCount = 1;
      ai.pSetLayouts = &layout;
      const bool sets_ok =
          g.vk.alloc_sets(g.device, &ai, set_out) == VK_SUCCESS;
      check(sets_ok, label);
      return sets_ok;
    };
    ok = make_single_pool(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4,
                          shade_layout, &shade_pool, &shade_set,
                          "ssao-gpu: shade descriptor set");
  }
  if (ok) {
    // Shade set: three cube views share the cube sampler, LUT has its own.
    // (Same 4-combined-sampler shape the P4-05 TU writes field-by-field;
    // one loop over the four image infos keeps this TU's copy short.)
    auto write_shade_set = [&]() {
      VkDescriptorImageInfo cube{};
      cube.sampler = cube_sampler;
      cube.imageView = src_sample;
      cube.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      VkDescriptorImageInfo irr = cube;
      irr.imageView = irr_sample;
      VkDescriptorImageInfo pref = cube;
      pref.imageView = pref_sample;
      VkDescriptorImageInfo lut{};
      lut.sampler = lut_sampler;
      lut.imageView = lut_img.view;
      lut.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      VkWriteDescriptorSet writes[4]{};
      const VkDescriptorImageInfo* infos[4] = {&cube, &irr, &pref, &lut};
      for (int k = 0; k < 4; ++k) {
        writes[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[k].dstSet = shade_set;
        writes[k].dstBinding = static_cast<std::uint32_t>(k);
        writes[k].descriptorCount = 1;
        writes[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[k].pImageInfo = infos[k];
      }
      g.vk.update_sets(g.device, 4, writes, 0, nullptr);
    };
    write_shade_set();
    check(true, "ssao-gpu: shade descriptors written");
  }
  // SSAO layouts + pipes: one set-1 shape shared by blur/composite;
  // pipeline layouts are [empty set0, ssao set1] with 16B fragment
  // pushes (SsaoBlurPush / SsaoCompositePush are both 16B).
  if (ok) {
    ok = make_ssao_set1(&g, &ssao_set1) && make_empty_set0(&g, &ssao_empty0);
    check(ok, "ssao-gpu: ssao set layouts");
  }
  if (ok) {
    const VkDescriptorSetLayout sets[2] = {ssao_empty0, ssao_set1};
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.offset = 0;
    pc.size = 16;
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 2;
    pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    ok = g.vk.create_pipe_layout(g.device, &pli, nullptr, &blur_pipe_layout) ==
             VK_SUCCESS &&
         g.vk.create_pipe_layout(g.device, &pli, nullptr, &comp_pipe_layout) ==
             VK_SUCCESS;
    check(ok, "ssao-gpu: ssao pipeline layouts");
  }
  if (ok) {
    ok = engine::test::GoldenBuildPipe(g.vk.create_pipes, g.device,
                                       ssao_vert_mod, blur_frag_mod,
                                       VK_FORMAT_R32_SFLOAT, blur_pipe_layout,
                                       &blur_pipe) &&
         engine::test::GoldenBuildPipe(g.vk.create_pipes, g.device,
                                       ssao_vert_mod, comp_frag_mod,
                                       VK_FORMAT_R8G8B8A8_UNORM,
                                       comp_pipe_layout, &comp_pipe);
    check(ok, "ssao-gpu: ssao pipelines build");
  }
  if (ok) {
    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    sizes[0].descriptorCount = 8;  // 4 sets x 2 images
    sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    sizes[1].descriptorCount = 4;  // 4 sets x 1 sampler
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 4;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    if (g.vk.create_pool(g.device, &pi, nullptr, &ssao_pool) != VK_SUCCESS) {
      ok = false;
    } else {
      VkDescriptorSetLayout layouts[4] = {ssao_set1, ssao_set1, ssao_set1,
                                          ssao_set1};
      VkDescriptorSetAllocateInfo ai{};
      ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      ai.descriptorPool = ssao_pool;
      ai.descriptorSetCount = 4;
      ai.pSetLayouts = layouts;
      VkDescriptorSet sets[4]{nullptr, nullptr, nullptr, nullptr};
      ok = g.vk.alloc_sets(g.device, &ai, sets) == VK_SUCCESS;
      if (ok) {
        blur_h_set = sets[0];
        blur_v_set = sets[1];
        comp_on_set = sets[2];
        comp_off_set = sets[3];
      }
    }
    check(ok, "ssao-gpu: ssao descriptor sets");
  }
  if (ok) {
    // blur H: occ + near sampler + depth; blur V: blur_a + sampler +
    // depth; composite ON: lit + sampler + blur_b; composite OFF: lit +
    // sampler + blur_b (valid binding; the OFF branch never samples it).
    // One view-info lambda keeps the four set writes to a table loop
    // instead of five named image infos.
    auto view_info = [](VkImageView view) {
      VkDescriptorImageInfo info{};
      info.imageView = view;
      info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      return info;
    };
    const VkImageView first_plane[4] = {occ_img.view, blur_a_img.view,
                                        lit_img.view, lit_img.view};
    const VkImageView second_plane[4] = {depth_img.view, depth_img.view,
                                         blur_b_img.view, blur_b_img.view};
    const VkSampler samps[4] = {near_sampler, near_sampler, lin_sampler,
                                lin_sampler};
    const VkDescriptorSet dst[4] = {blur_h_set, blur_v_set, comp_on_set,
                                    comp_off_set};
    for (int s = 0; s < 4; ++s) {
      VkDescriptorImageInfo planes[2] = {view_info(first_plane[s]),
                                         view_info(second_plane[s])};
      VkDescriptorImageInfo sampler_info{};
      sampler_info.sampler = samps[s];
      VkWriteDescriptorSet w[3]{};
      const VkDescriptorImageInfo* infos[3] = {&planes[0], &sampler_info,
                                               &planes[1]};
      for (int k = 0; k < 3; ++k) {
        w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[k].dstSet = dst[s];
        w[k].dstBinding = static_cast<std::uint32_t>(k);
        w[k].descriptorCount = 1;
        w[k].pImageInfo = infos[k];
      }
      w[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
      w[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      g.vk.update_sets(g.device, 3, w, 0, nullptr);
    }
    check(true, "ssao-gpu: ssao descriptors written");
  }

  // Record: upload src + LUT + depth/occ, compute pre-pass, shade,
  // blur H/V, composite ON + OFF, read back LDR pair + blurred floats.
  // The barrier helper wraps GoldenBarrier (single-image subresource);
  // cube uploads pass layers=6 (all faces) and the prefilter barrier
  // passes mips=kPrefMips (every level the shade view samples).
  // Barrier + record helpers shared by the upload/prepass passes below.
  auto barrier_img = [&](VkCommandBuffer cb, VkImage img, VkImageLayout old_l,
                         VkImageLayout new_l, VkPipelineStageFlags2 src_s,
                         VkAccessFlags2 src_a, VkPipelineStageFlags2 dst_s,
                         VkAccessFlags2 dst_a, std::uint32_t layers,
                         std::uint32_t mips) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = src_s;
    b.srcAccessMask = src_a;
    b.dstStageMask = dst_s;
    b.dstAccessMask = dst_a;
    b.oldLayout = old_l;
    b.newLayout = new_l;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, layers};
    VkDependencyInfo d{};
    d.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    g.vk.cmd_barrier2(cb, &d);
  };
  // Upload one image from a staging buffer (TRANSFER_DST entry +
  // per-layer copies for cubes, single copy for 2D targets).
  auto upload_img = [&](VkCommandBuffer cb, VkBuffer staging, VkImage img,
                        std::uint32_t layers, std::uint32_t edge,
                        VkDeviceSize stride) {
    barrier_img(cb, img, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                layers, 1);
    for (std::uint32_t layer = 0; layer < layers; ++layer) {
      VkBufferImageCopy rc{};
      rc.bufferOffset = static_cast<VkDeviceSize>(layer) * stride;
      rc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
      rc.imageExtent = {edge, edge, 1};
      g.vk.cmd_copy_buf_img(cb, staging, img,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rc);
    }
  };
  // Fill the 16px LUT staging tail from the CPU BrdfLutReference, then
  // copy it into the LUT image (RG32F: 2 floats per texel).
  auto upload_lut = [&](VkCommandBuffer cb) -> bool {
    barrier_img(cb, lut_img.img, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                1, 1);
    void* lp = nullptr;
    if (g.vk.map_mem(g.device, up_b.mem, src_bytes, lut_bytes, 0, &lp) !=
            VK_SUCCESS ||
        lp == nullptr) {
      return false;
    }
    auto* ld = static_cast<float*>(lp);
    for (std::uint32_t yy = 0; yy < kLutEdge; ++yy) {
      for (std::uint32_t xx = 0; xx < kLutEdge; ++xx) {
        float s = 0.0f;
        float b = 0.0f;
        engine::renderer::BrdfLutReference(xx, yy, kLutEdge, &s, &b);
        ld[(static_cast<std::size_t>(yy) * kLutEdge + xx) * 2 + 0] = s;
        ld[(static_cast<std::size_t>(yy) * kLutEdge + xx) * 2 + 1] = b;
      }
    }
    g.vk.unmap_mem(g.device, up_b.mem);
    VkBufferImageCopy lc{};
    lc.bufferOffset = static_cast<VkDeviceSize>(src_bytes);
    lc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    lc.imageExtent = {kLutEdge, kLutEdge, 1};
    g.vk.cmd_copy_buf_img(cb, up_b.buf, lut_img.img,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &lc);
    barrier_img(cb, lut_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1, 1);
    return true;
  };
  // Run the P4-05 compute pre-pass: irradiance dispatch, then one
  // prefilter dispatch per mip (each mip bound to its own set; ceil-div
  // groups so tail mips get coverage; memory barrier orders the GENERAL
  // writes between mips).
  auto run_compute_prepass = [&](VkCommandBuffer cb) {
    const std::uint32_t irr_groups = (kIrr + 15u) / 16u;
    cv.cmd_bind_pipe(cb, VK_PIPELINE_BIND_POINT_COMPUTE, irr_pipe);
    cv.cmd_bind_sets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, irr_pipe_layout, 0,
                     1, &irr_set, 0, nullptr);
    {
      std::array<std::uint32_t, 2> push = {kIrr, 512};
      cv.cmd_push(cb, irr_pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8,
                  push.data());
    }
    g.vk.cmd_dispatch(cb, irr_groups, irr_groups, 6);
    barrier_img(cb, pref_img.img, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_GENERAL, 0, 0,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, 6, kPrefMips);
    cv.cmd_bind_pipe(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pref_pipe);
    for (std::uint32_t m = 0; m < kPrefMips; ++m) {
      const std::uint32_t edge = kPref >> m;
      cv.cmd_bind_sets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pref_pipe_layout,
                       0, 1, &pref_sets[m], 0, nullptr);
      {
        std::array<std::uint32_t, 4> push = {edge, m, kPrefMips, 1024};
        cv.cmd_push(cb, pref_pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                    16, push.data());
      }
      const std::uint32_t groups = (edge + 15u) / 16u;
      g.vk.cmd_dispatch(cb, groups, groups, 6);
      VkMemoryBarrier2 mb{};
      mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
      mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
      mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
      mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
      mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                         VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
      VkDependencyInfo md{};
      md.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
      md.memoryBarrierCount = 1;
      md.pMemoryBarriers = &mb;
      g.vk.cmd_barrier2(cb, &md);
    }
  };
  // One fullscreen pass record (target enters COLOR_ATTACHMENT_OPTIMAL;
  // positive-height viewport: framebuffer row 0 samples source texel row
  // 0 (vUv (0,0) lands on row 0), so the output rows need no CPU flip
  // before the golden compare — the same row convention the P4-05 IBL TU
  // uses. NOTE: the shade pass right below keeps the P4-05 TU's
  // negative-height viewport verbatim (its golden was baked with it);
  // only the ssao passes — whose ssao_full.vert puts vUv (0,0) at NDC
  // (-1,-1) — use positive height so composite row y samples lit row y.
  // (Same dynamic-rendering + bind/push/draw tail the P4-05 TU repeats
  // per pass; one lambda covers all four SSAO passes here.)
  auto record_full = [&](VkCommandBuffer cb, VkImageView target,
                         std::uint32_t w, std::uint32_t h, VkPipeline pipe,
                         VkPipelineLayout layout, std::uint32_t first_set,
                         VkDescriptorSet set, const void* push,
                         std::uint32_t push_bytes,
                         VkShaderStageFlags stages) {
    VkRenderingAttachmentInfo att{};
    att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    att.imageView = target;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue.color.float32[0] = 0.0f;
    att.clearValue.color.float32[1] = 0.0f;
    att.clearValue.color.float32[2] = 0.0f;
    att.clearValue.color.float32[3] = 1.0f;
    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, {w, h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    g.vk.cmd_begin_render(cb, &ri);
    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);
    VkViewport vp{0, 0, fw, fh, 0.0f, 1.0f};
    g.vk.cmd_viewport(cb, 0, 1, &vp);
    VkRect2D sc{};
    sc.extent = {w, h};
    g.vk.cmd_scissor(cb, 0, 1, &sc);
    g.vk.cmd_bind_pipe(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    g.vk.cmd_bind_sets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, first_set,
                       1, &set, 0, nullptr);
    g.vk.cmd_push(cb, layout, stages, 0, push_bytes, push);
    g.vk.cmd_draw(cb, 3, 1, 0, 0);
    g.vk.cmd_end_render(cb);
  };
  if (ok) {
    VkCommandPool cmd_pool = nullptr;
    VkCommandBuffer cb = nullptr;
    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.queueFamilyIndex = g.queue_family;
    ok = g.vk.create_cmd_pool(g.device, &cpi, nullptr, &cmd_pool) == VK_SUCCESS;
    if (ok) {
      ok = engine::test::GoldenBeginCmd(&g, cmd_pool, &cb);
    }
    if (ok) {
      // Uploads: source cube (6 faces), CPU LUT tail, then half-res
      // depth + occlusion from their staging buffers.
      upload_img(cb, up_b.buf, src_img.img, 6, kSrc,
                 static_cast<VkDeviceSize>(kSrc) * kSrc * 16);
      ok = upload_lut(cb);
    }
    if (ok) {
      barrier_img(cb, depth_img.img, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  1, 1);
      barrier_img(cb, occ_img.img, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  1, 1);
      {
        VkBufferImageCopy dc{};
        dc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        dc.imageExtent = {kHw, kHh, 1};
        g.vk.cmd_copy_buf_img(cb, depth_up_b.buf, depth_img.img,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &dc);
        g.vk.cmd_copy_buf_img(cb, occ_up_b.buf, occ_img.img,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &dc);
      }
      barrier_img(cb, src_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 6, 1);
      barrier_img(cb, depth_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1, 1);
      barrier_img(cb, occ_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1, 1);
      // Irradiance + prefilter pre-pass (mirrors the P4-05 TU).
      run_compute_prepass(cb);
      barrier_img(cb, irr_img.img, VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 6, 1);
      barrier_img(cb, pref_img.img, VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 6, kPrefMips);
      // Shade into HDR lit (negative-height viewport keeps the P4-05
      // row convention the IBL TU baked its golden with).
      barrier_img(cb, lit_img.img, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 0,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 1, 1);
      {
        VkRenderingAttachmentInfo color_att{};
        color_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        color_att.imageView = lit_img.view;
        color_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color_att.clearValue.color.float32[0] = 0.0f;
        color_att.clearValue.color.float32[1] = 0.0f;
        color_att.clearValue.color.float32[2] = 0.0f;
        color_att.clearValue.color.float32[3] = 1.0f;
        VkRenderingInfo ri{};
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea = {{0, 0}, {kW, kH}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color_att;
        g.vk.cmd_begin_render(cb, &ri);
        VkViewport vp{};
        vp.y = static_cast<float>(kH);
        vp.width = static_cast<float>(kW);
        vp.height = -static_cast<float>(kH);
        vp.maxDepth = 1.0f;
        g.vk.cmd_viewport(cb, 0, 1, &vp);
        VkRect2D sc{};
        sc.extent = {kW, kH};
        g.vk.cmd_scissor(cb, 0, 1, &sc);
        g.vk.cmd_bind_pipe(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, shade_pipe);
        g.vk.cmd_bind_sets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                           shade_pipe_layout, 0, 1, &shade_set, 0, nullptr);
        float inv_view[10] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                              0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
        g.vk.cmd_push(cb, shade_pipe_layout,
                      VK_SHADER_STAGE_VERTEX_BIT |
                          VK_SHADER_STAGE_FRAGMENT_BIT,
                      0, 40, inv_view);
        g.vk.cmd_draw(cb, 3, 1, 0, 0);
        g.vk.cmd_end_render(cb);
      }
      // SSAO chain: blur H (occ/depth -> blur_a), blur V (blur_a/depth ->
      // blur_b), composite ON (lit * blur_b -> LDR, enabled 1), composite
      // OFF (same lit, enabled 0 — the P4-05 path; the OFF branch never
      // samples the occlusion binding). Blur/composite share the
      // record_full shape, so one lambda covers all four passes.
      barrier_img(cb, lit_img.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1, 1);
      auto run_ssao_pass = [&](VkImageView target, std::uint32_t w,
                               std::uint32_t h, VkPipeline pipe,
                               VkPipelineLayout layout, VkDescriptorSet set,
                               const void* push, std::uint32_t push_bytes) {
        barrier_img(cb, target == blur_a_img.view   ? blur_a_img.img
                     : target == blur_b_img.view   ? blur_b_img.img
                     : target == ldr_on_img.view   ? ldr_on_img.img
                                                   : ldr_off_img.img,
                    VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 0,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 1, 1);
        record_full(cb, target, w, h, pipe, layout, 1, set, push, push_bytes,
                    VK_SHADER_STAGE_FRAGMENT_BIT);
      };
      {
        engine::renderer::SsaoBlurPush push{};
        push.texel[0] = 1.0f / static_cast<float>(kHw);
        push.texel[1] = 1.0f / static_cast<float>(kHh);
        push.sigma = kSigma;
        push.horizontal = 1;
        run_ssao_pass(blur_a_img.view, kHw, kHh, blur_pipe, blur_pipe_layout,
                      blur_h_set, &push, sizeof(push));
      }
      barrier_img(cb, blur_a_img.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1, 1);
      {
        engine::renderer::SsaoBlurPush push{};
        push.texel[0] = 1.0f / static_cast<float>(kHw);
        push.texel[1] = 1.0f / static_cast<float>(kHh);
        push.sigma = kSigma;
        push.horizontal = 0;
        run_ssao_pass(blur_b_img.view, kHw, kHh, blur_pipe, blur_pipe_layout,
                      blur_v_set, &push, sizeof(push));
      }
      barrier_img(cb, blur_b_img.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1, 1);
      // Composite ON/OFF (same lit; ON multiplies blur_b, OFF skips it).
      auto run_composite = [&](VkImageView target, VkDescriptorSet set,
                               std::uint32_t enabled) {
        engine::renderer::SsaoCompositePush push{};
        push.exposure = kExposure;
        push.enabled = enabled;
        run_ssao_pass(target, kW, kH, comp_pipe, comp_pipe_layout, set, &push,
                      sizeof(push));
      };
      run_composite(ldr_on_img.view, comp_on_set, 1);
      run_composite(ldr_off_img.view, comp_off_set, 0);
      // Read back both LDR frames + the blurred occlusion floats: one
      // copy helper covers the three 2D targets (w/h vary), then the two
      // cube readbacks below iterate faces the same way.
      auto readback_2d = [&](VkImage img, VkBuffer dst, std::uint32_t w,
                             std::uint32_t h) {
        barrier_img(cb, img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                    1, 1);
        VkBufferImageCopy rl{};
        rl.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rl.imageExtent = {w, h, 1};
        g.vk.cmd_copy_img_buf(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              dst, 1, &rl);
      };
      readback_2d(ldr_on_img.img, read_on_b.buf, kW, kH);
      readback_2d(ldr_off_img.img, read_off_b.buf, kW, kH);
      barrier_img(cb, blur_b_img.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                  1, 1);
      {
        VkBufferImageCopy rl{};
        rl.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rl.imageExtent = {kHw, kHh, 1};
        g.vk.cmd_copy_img_buf(cb, blur_b_img.img,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              read_blur_b.buf, 1, &rl);
      }
      // Irradiance + prefilter readbacks (kept for parity with P4-05;
      // mapped but unchecked — the shade golden is the proof here).
      // One cube-copy loop covers both (edge/stride vary).
      auto readback_cube = [&](VkImage img, VkBuffer dst, std::uint32_t edge,
                               std::uint32_t mips, VkDeviceSize stride) {
        barrier_img(cb, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                    6, mips);
        for (std::uint32_t layer = 0; layer < 6; ++layer) {
          VkBufferImageCopy rl{};
          rl.bufferOffset = static_cast<VkDeviceSize>(layer) * stride;
          rl.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
          rl.imageExtent = {edge, edge, 1};
          g.vk.cmd_copy_img_buf(cb, img,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1,
                                &rl);
        }
      };
      readback_cube(irr_img.img, read_irr_b.buf, kIrr, 1,
                    static_cast<VkDeviceSize>(kIrr) * kIrr * 16);
      readback_cube(pref_img.img, read_pref_b.buf, kPref, kPrefMips,
                    static_cast<VkDeviceSize>(kPref) * kPref * 16);
      ok = engine::test::GoldenEndReadback(&g, cb);
    }
    check(ok, "ssao-gpu: frame recorded");
    if (ok) {
      ok = engine::test::GoldenSubmitWait(&g, cb);
      check(ok, "ssao-gpu: submit + fence wait");
    }
    if (cmd_pool != nullptr) {
      g.vk.destroy_cmd_pool(g.device, cmd_pool, nullptr);
    }
  }

  // Readback: blurred occlusion vs the CPU 5x5 separable reference.
  if (ok) {
    void* p = nullptr;
    ok = g.vk.map_mem(g.device, read_blur_b.mem, 0, half_bytes, 0, &p) ==
             VK_SUCCESS &&
         p != nullptr;
    check(ok, "ssao-gpu: blur readback mapped");
    if (ok) {
      const auto* f = static_cast<const float*>(p);
      // CPU reference: H pass then V pass over occ_px/depth_px with
      // clamp-to-edge taps (mirrors the shader's sampler addressing).
      std::vector<float> ref_h(static_cast<std::size_t>(kHw) * kHh, 0.0f);
      std::vector<float> ref(static_cast<std::size_t>(kHw) * kHh, 0.0f);
      auto occ_at = [&](int x, int y) -> float {
        const int cx = (x < 0) ? 0 : ((x >= static_cast<int>(kHw))
                                          ? static_cast<int>(kHw) - 1
                                          : x);
        const int cy = (y < 0) ? 0 : ((y >= static_cast<int>(kHh))
                                          ? static_cast<int>(kHh) - 1
                                          : y);
        return occ_px[(static_cast<std::size_t>(cy) * kHw +
                       static_cast<std::size_t>(cx))];
      };
      auto dpt_at = [&](int x, int y) -> float {
        const int cx = (x < 0) ? 0 : ((x >= static_cast<int>(kHw))
                                          ? static_cast<int>(kHw) - 1
                                          : x);
        const int cy = (y < 0) ? 0 : ((y >= static_cast<int>(kHh))
                                          ? static_cast<int>(kHh) - 1
                                          : y);
        return depth_px[(static_cast<std::size_t>(cy) * kHw +
                         static_cast<std::size_t>(cx))];
      };
      for (std::uint32_t y = 0; y < kHh; ++y) {
        for (std::uint32_t x = 0; x < kHw; ++x) {
          float o[5];
          float d[5];
          for (int i = -2; i <= 2; ++i) {
            o[i + 2] = occ_at(static_cast<int>(x) + i, static_cast<int>(y));
            d[i + 2] = dpt_at(static_cast<int>(x) + i, static_cast<int>(y));
          }
          ref_h[static_cast<std::size_t>(y) * kHw + x] =
              engine::renderer::SsaoBlurAxisCpu(o, d, kSigma);
        }
      }
      for (std::uint32_t y = 0; y < kHh; ++y) {
        for (std::uint32_t x = 0; x < kHw; ++x) {
          float o[5];
          float d[5];
          for (int i = -2; i <= 2; ++i) {
            const int yy = static_cast<int>(y) + i;
            const int cy = (yy < 0) ? 0
                           : ((yy >= static_cast<int>(kHh))
                                  ? static_cast<int>(kHh) - 1
                                  : yy);
            o[i + 2] =
                ref_h[static_cast<std::size_t>(cy) * kHw + x];
            d[i + 2] = dpt_at(static_cast<int>(x), yy);
          }
          ref[static_cast<std::size_t>(y) * kHw + x] =
              engine::renderer::SsaoBlurAxisCpu(o, d, kSigma);
        }
      }
      std::uint32_t bad = 0;
      float worst = 0.0f;
      std::uint32_t first_bx = 0;
      std::uint32_t first_by = 0;
      for (std::uint32_t y = 0; y < kHh; ++y) {
        for (std::uint32_t x = 0; x < kHw; ++x) {
          const std::size_t i = static_cast<std::size_t>(y) * kHw + x;
          const float got = f[i];
          const float dd = std::fabs(got - ref[i]);
          if (dd > worst) {
            worst = dd;
          }
          // R32F round-trip + nearest-tap bilinear-free path: 1e-3 covers
          // lavapipe exp() ULP drift across the 10 taps.
          if (dd > 1e-3f || !std::isfinite(got)) {
            if (bad == 0) {
              first_bx = x;
              first_by = y;
            }
            ++bad;
          }
        }
      }
      // The dip must survive smoothing but stay lifted (0.1 < dip < 0.55)
      // and flat far-field texels must hold the base value.
      const float dip_got =
          f[static_cast<std::size_t>(dip_y) * kHw + dip_x];
      const float flat_got =
          f[static_cast<std::size_t>(10) * kHw + 140];
      std::printf(
          "[INFO] ssao-gpu: blur worst %.6f bad=%u dip=%.4f flat=%.4f "
          "first=(%u,%u)\n",
          static_cast<double>(worst), bad, static_cast<double>(dip_got),
          static_cast<double>(flat_got), first_bx, first_by);
      check(bad == 0, "ssao-gpu: blur matches CPU reference");
      check(dip_got > 0.1f && dip_got < 0.55f, "ssao-gpu: dip smoothed");
      check(std::fabs(flat_got - 0.55f) < 1e-3f, "ssao-gpu: flat holds base");
      g.vk.unmap_mem(g.device, read_blur_b.mem);
    }
  }

  // Readback: ON vs golden + OFF vs P4-05 golden + ON-darker probe.
  if (ok) {
    std::vector<std::uint8_t> on_px(ldr_bytes, 0);
    std::vector<std::uint8_t> off_px(ldr_bytes, 0);
    const std::size_t n = on_px.size();
    ok = engine::test::GoldenMapPixels(
             [&](VkDevice d, VkDeviceMemory m, VkDeviceSize o, VkDeviceSize s,
                 VkMemoryMapFlags fl, void** pp) {
               return g.vk.map_mem(d, m, o, s, fl, pp);
             },
             [&](VkDevice d, VkDeviceMemory m) { g.vk.unmap_mem(d, m); },
             g.device, read_on_b.mem, on_px.data(), n) &&
         engine::test::GoldenMapPixels(
             [&](VkDevice d, VkDeviceMemory m, VkDeviceSize o, VkDeviceSize s,
                 VkMemoryMapFlags fl, void** pp) {
               return g.vk.map_mem(d, m, o, s, fl, pp);
             },
             [&](VkDevice d, VkDeviceMemory m) { g.vk.unmap_mem(d, m); },
             g.device, read_off_b.mem, off_px.data(), n);
    check(ok, "ssao-gpu: LDR readbacks mapped");
    if (ok) {
      failures += engine::test::golden_check(
          "p4_09_ssao_320x240",
          std::span<const std::uint8_t>(on_px.data(), on_px.size()), kW, kH);
      failures += engine::test::golden_check(
          "p4_05_ibl_chrome_rough_320x240",
          std::span<const std::uint8_t>(off_px.data(), off_px.size()), kW, kH);
      // Occluded-disc probe: with base occlusion 0.55 the ON frame is
      // strictly darker than OFF at the dip's full-res footprint.
      const std::size_t dip_o =
          (static_cast<std::size_t>(dip_y * 2) * kW + dip_x * 2) * 4;
      check(on_px[dip_o] < off_px[dip_o], "ssao-golden: ON darker than OFF");
      check(off_px[dip_o] > 0, "ssao-golden: OFF lit at probe");
    }
  }

  if (shade_pipe != nullptr) {
    g.vk.destroy_pipe(g.device, shade_pipe, nullptr);
  }
  if (blur_pipe != nullptr) {
    g.vk.destroy_pipe(g.device, blur_pipe, nullptr);
  }
  if (comp_pipe != nullptr) {
    g.vk.destroy_pipe(g.device, comp_pipe, nullptr);
  }
  if (irr_pipe != nullptr) {
    cv.destroy_pipe(g.device, irr_pipe, nullptr);
  }
  if (pref_pipe != nullptr) {
    cv.destroy_pipe(g.device, pref_pipe, nullptr);
  }
  for (auto* m :
       {&irr_mod, &pref_mod, &shade_vert_mod, &shade_frag_mod, &ssao_vert_mod,
        &blur_frag_mod, &comp_frag_mod}) {
    if (*m != nullptr) {
      g.vk.destroy_module(g.device, *m, nullptr);
    }
  }
  for (auto* l : {&comp_layout, &shade_layout, &ssao_set1, &ssao_empty0}) {
    if (*l != nullptr) {
      g.vk.destroy_set_layout(g.device, *l, nullptr);
    }
  }
  for (auto* l :
       {&irr_pipe_layout, &pref_pipe_layout, &shade_pipe_layout,
        &blur_pipe_layout, &comp_pipe_layout}) {
    if (*l != nullptr) {
      g.vk.destroy_pipe_layout(g.device, *l, nullptr);
    }
  }
  if (comp_pool != nullptr) {
    g.vk.destroy_pool(g.device, comp_pool, nullptr);
  }
  if (shade_pool != nullptr) {
    g.vk.destroy_pool(g.device, shade_pool, nullptr);
  }
  if (ssao_pool != nullptr) {
    g.vk.destroy_pool(g.device, ssao_pool, nullptr);
  }
  if (irr_mip0_view != nullptr) {
    g.vk.destroy_view(g.device, irr_mip0_view, nullptr);
  }
  for (auto v : pref_mip_views) {
    if (v != nullptr) {
      g.vk.destroy_view(g.device, v, nullptr);
    }
  }
  for (auto v : {src_sample, irr_sample, pref_sample}) {
    if (v != nullptr) {
      g.vk.destroy_view(g.device, v, nullptr);
    }
  }
  for (auto s : {cube_sampler, lut_sampler, lin_sampler, near_sampler}) {
    if (s != nullptr) {
      g.vk.destroy_sampler(g.device, s, nullptr);
    }
  }
  // Teardown: images + staging (bindless + device shutdown stay in the
  // TU, same as the P4-05 family; the per-handle free list is the only
  // TU-local part — the allocators above are all shared helpers).
  engine::test::GoldenFreeImage(&g, &src_img);
  engine::test::GoldenFreeImage(&g, &irr_img);
  engine::test::GoldenFreeImage(&g, &pref_img);
  engine::test::GoldenFreeImage(&g, &lut_img);
  engine::test::GoldenFreeImage(&g, &lit_img);
  engine::test::GoldenFreeImage(&g, &depth_img);
  engine::test::GoldenFreeImage(&g, &occ_img);
  engine::test::GoldenFreeImage(&g, &blur_a_img);
  engine::test::GoldenFreeImage(&g, &blur_b_img);
  engine::test::GoldenFreeImage(&g, &ldr_on_img);
  engine::test::GoldenFreeImage(&g, &ldr_off_img);
  engine::test::GoldenFreeHost(&g, &up_b);
  engine::test::GoldenFreeHost(&g, &read_irr_b);
  engine::test::GoldenFreeHost(&g, &read_pref_b);
  engine::test::GoldenFreeHost(&g, &read_on_b);
  engine::test::GoldenFreeHost(&g, &read_off_b);
  engine::test::GoldenFreeHost(&g, &depth_up_b);
  engine::test::GoldenFreeHost(&g, &occ_up_b);
  engine::test::GoldenFreeHost(&g, &read_blur_b);
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
  std::printf("[SKIP] ssao-gpu: SDK absent (stub build)\n");
#endif
  std::printf(failures == 0 ? "test_ssao: all checks passed\n"
                            : "test_ssao: %d FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}

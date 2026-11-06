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

namespace {

auto failures = 0;

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}

// --- CPU contract -----------------------------------------------------------

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

// Compute entry points beyond the shared GoldenVkProcs table (destroy +
// bind + push only; create_compute/dispatch live in GoldenVkProcs).
struct SsaoComputeVk {
  PFN_vkDestroyPipeline destroy_pipe{nullptr};
  PFN_vkCmdBindPipeline cmd_bind_pipe{nullptr};
  PFN_vkCmdBindDescriptorSets cmd_bind_sets{nullptr};
  PFN_vkCmdPushConstants cmd_push{nullptr};
};

template <typename Loader>
void load_ssao_compute_procs(Loader loader, SsaoComputeVk* p) {
  auto load = [&](const char* name) { return loader(name); };
  p->destroy_pipe =
      reinterpret_cast<PFN_vkDestroyPipeline>(load("vkDestroyPipeline"));
  p->cmd_bind_pipe =
      reinterpret_cast<PFN_vkCmdBindPipeline>(load("vkCmdBindPipeline"));
  p->cmd_bind_sets = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(
      load("vkCmdBindDescriptorSets"));
  p->cmd_push =
      reinterpret_cast<PFN_vkCmdPushConstants>(load("vkCmdPushConstants"));
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
  VkImageViewCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vi.image = img;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  vi.format = VK_FORMAT_R32G32B32A32_SFLOAT;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 6};
  return g->vk.create_view(g->device, &vi, nullptr, out) == VK_SUCCESS;
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
  auto missing_fn = [&](const char* what) {
    failures += engine::test::GoldenGpuMissing(what);
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
  const std::uint64_t lut_bytes = static_cast<std::uint64_t>(16) * 16 * 8;
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
  if (ok) {
    ok = engine::test::GoldenMakeCube(
             &g, kSrc, 1,
             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
             &src_img) &&
         engine::test::GoldenMakeCube(
             &g, kIrr, 1,
             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
             &irr_img) &&
         engine::test::GoldenMakeCube(
             &g, kPref, kPrefMips,
             VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
             &pref_img);
    check(ok, "ssao-gpu: cube images");
  }
  VkImageView src_sample = nullptr;
  VkImageView irr_sample = nullptr;
  VkImageView pref_sample = nullptr;
  if (ok) {
    ok = engine::test::GoldenMakeCubeSampleView(&g, src_img.img, 1,
                                                &src_sample) &&
         engine::test::GoldenMakeCubeSampleView(&g, irr_img.img, 1,
                                                &irr_sample) &&
         engine::test::GoldenMakeCubeSampleView(&g, pref_img.img, kPrefMips,
                                                &pref_sample);
    check(ok, "ssao-gpu: cube sample views");
  }
  if (ok) {
    constexpr std::uint32_t kLut = 16;
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R32G32_SFLOAT;
    ii.extent = {kLut, kLut, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ok = g.vk.create_image(g.device, &ii, nullptr, &lut_img.img) == VK_SUCCESS;
    if (ok) {
      VkMemoryRequirements req{};
      g.vk.get_image_reqs(g.device, lut_img.img, &req);
      VkPhysicalDeviceMemoryProperties mp{};
      g.vk.get_mem_props(g.physical, &mp);
      std::uint32_t mi = 0xFFFFFFFFu;
      for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) != 0u) {
          mi = i;
          break;
        }
      }
      ok = mi != 0xFFFFFFFFu;
      if (ok) {
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = mi;
        ok = g.vk.alloc_mem(g.device, &ai, nullptr, &lut_img.mem) == VK_SUCCESS;
      }
      if (ok) {
        ok = g.vk.bind_image_mem(g.device, lut_img.img, lut_img.mem, 0) ==
             VK_SUCCESS;
      }
      if (ok) {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = lut_img.img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R32G32_SFLOAT;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        ok = g.vk.create_view(g.device, &vi, nullptr, &lut_img.view) ==
             VK_SUCCESS;
      }
    }
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
    // Cube sampler (linear, LOD-capable) + LUT sampler (linear clamp) +
    // blur/composite linear-clamp sampler, mirroring the IBL TU.
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
      VkSamplerCreateInfo li{};
      li.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
      li.magFilter = VK_FILTER_LINEAR;
      li.minFilter = VK_FILTER_LINEAR;
      li.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      li.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      li.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      ok = g.vk.create_sampler(g.device, &li, nullptr, &lut_sampler) ==
           VK_SUCCESS;
    }
    if (ok) {
      // lin_sampler = bilinear upscale at composite (per P4-09 notes:
      // blur at half res, bilinear upscale at composite time);
      // near_sampler = exact-tap sampling for the blur passes.
      VkSamplerCreateInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
      bi.magFilter = VK_FILTER_LINEAR;
      bi.minFilter = VK_FILTER_LINEAR;
      bi.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      bi.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      bi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      ok = g.vk.create_sampler(g.device, &bi, nullptr, &lin_sampler) ==
           VK_SUCCESS;
      if (ok) {
        bi.magFilter = VK_FILTER_NEAREST;
        bi.minFilter = VK_FILTER_NEAREST;
        ok = g.vk.create_sampler(g.device, &bi, nullptr, &near_sampler) ==
             VK_SUCCESS;
      }
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
  if (ok) {
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pc.size = 8;
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &comp_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    ok = g.vk.create_pipe_layout(g.device, &pli, nullptr, &irr_pipe_layout) ==
         VK_SUCCESS;
    check(ok, "ssao-gpu: irradiance pipeline layout");
  }
  if (ok) {
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pc.size = 16;
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &comp_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    ok = g.vk.create_pipe_layout(g.device, &pli, nullptr, &pref_pipe_layout) ==
         VK_SUCCESS;
    check(ok, "ssao-gpu: prefilter pipeline layout");
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
    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = irr_mod;
    cpi.stage.pName = "main";
    cpi.layout = irr_pipe_layout;
    VkComputePipelineCreateInfo cpi2 = cpi;
    cpi2.stage.module = pref_mod;
    cpi2.layout = pref_pipe_layout;
    ok = g.vk.create_compute(g.device, nullptr, 1, &cpi, nullptr, &irr_pipe) ==
             VK_SUCCESS &&
         g.vk.create_compute(g.device, nullptr, 1, &cpi2, nullptr,
                             &pref_pipe) == VK_SUCCESS;
    check(ok, "ssao-gpu: compute pipelines");
  }
  if (ok) {
    VkDescriptorPoolSize size{};
    size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    size.descriptorCount = 1 + 6;
    VkDescriptorPoolSize size2{};
    size2.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    size2.descriptorCount = 1 + 6;
    VkDescriptorPoolSize sizes[2] = {size, size2};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 1 + 6;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    if (g.vk.create_pool(g.device, &pi, nullptr, &comp_pool) != VK_SUCCESS) {
      ok = false;
    } else {
      VkDescriptorSetAllocateInfo ai{};
      ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      ai.descriptorPool = comp_pool;
      ai.descriptorSetCount = 1;
      ai.pSetLayouts = &comp_layout;
      ok = g.vk.alloc_sets(g.device, &ai, &irr_set) == VK_SUCCESS;
      for (std::uint32_t m = 0; m < kPrefMips && ok; ++m) {
        ok = g.vk.alloc_sets(g.device, &ai, &pref_sets[m]) == VK_SUCCESS;
      }
    }
    check(ok, "ssao-gpu: compute descriptor sets");
  }
  if (ok) {
    ok = make_per_mip_view(&g, irr_img.img, 0, &irr_mip0_view);
    check(ok, "ssao-gpu: irradiance mip0 view");
  }
  if (ok) {
    for (std::uint32_t m = 0; m < kPrefMips; ++m) {
      ok = make_per_mip_view(&g, pref_img.img, m, &pref_mip_views[m]) && ok;
    }
    check(ok, "ssao-gpu: prefilter per-mip views");
  }
  if (ok) {
    VkDescriptorImageInfo src_info{};
    src_info.sampler = cube_sampler;
    src_info.imageView = src_sample;
    src_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo irr_stor{};
    irr_stor.imageView = irr_mip0_view;
    irr_stor.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet w0{};
    w0.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w0.dstSet = irr_set;
    w0.dstBinding = 0;
    w0.descriptorCount = 1;
    w0.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w0.pImageInfo = &src_info;
    VkWriteDescriptorSet w1 = w0;
    w1.dstBinding = 1;
    w1.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w1.pImageInfo = &irr_stor;
    VkDescriptorImageInfo pref_stor{};
    pref_stor.imageView = pref_mip_views[0];
    pref_stor.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet w2 = w0;
    w2.dstSet = pref_sets[0];
    VkWriteDescriptorSet w3 = w1;
    w3.dstSet = pref_sets[0];
    w3.pImageInfo = &pref_stor;
    VkWriteDescriptorSet writes[4] = {w0, w1, w2, w3};
    g.vk.update_sets(g.device, 4, writes, 0, nullptr);
    for (std::uint32_t m = 1; m < kPrefMips; ++m) {
      VkDescriptorImageInfo src_m{};
      src_m.sampler = cube_sampler;
      src_m.imageView = src_sample;
      src_m.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      VkDescriptorImageInfo stor_m{};
      stor_m.imageView = pref_mip_views[m];
      stor_m.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      VkWriteDescriptorSet wm0 = w0;
      wm0.dstSet = pref_sets[m];
      wm0.pImageInfo = &src_m;
      VkWriteDescriptorSet wm1 = w1;
      wm1.dstSet = pref_sets[m];
      wm1.pImageInfo = &stor_m;
      VkWriteDescriptorSet wm[2] = {wm0, wm1};
      g.vk.update_sets(g.device, 2, wm, 0, nullptr);
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
    VkDescriptorPoolSize size{};
    size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    size.descriptorCount = 4;
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    if (g.vk.create_pool(g.device, &pi, nullptr, &shade_pool) != VK_SUCCESS) {
      ok = false;
    } else {
      VkDescriptorSetAllocateInfo ai{};
      ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      ai.descriptorPool = shade_pool;
      ai.descriptorSetCount = 1;
      ai.pSetLayouts = &shade_layout;
      ok = g.vk.alloc_sets(g.device, &ai, &shade_set) == VK_SUCCESS;
    }
    check(ok, "ssao-gpu: shade descriptor set");
  }
  if (ok) {
    VkDescriptorImageInfo i0{};
    i0.sampler = cube_sampler;
    i0.imageView = src_sample;
    i0.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo i1 = i0;
    i1.imageView = irr_sample;
    VkDescriptorImageInfo i2 = i0;
    i2.imageView = pref_sample;
    VkDescriptorImageInfo i3{};
    i3.sampler = lut_sampler;
    i3.imageView = lut_img.view;
    i3.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet writes[4]{};
    const VkDescriptorImageInfo* infos[4] = {&i0, &i1, &i2, &i3};
    for (int k = 0; k < 4; ++k) {
      writes[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[k].dstSet = shade_set;
      writes[k].dstBinding = static_cast<std::uint32_t>(k);
      writes[k].descriptorCount = 1;
      writes[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[k].pImageInfo = infos[k];
    }
    g.vk.update_sets(g.device, 4, writes, 0, nullptr);
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
    VkDescriptorImageInfo occ_i{};
    occ_i.imageView = occ_img.view;
    occ_i.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo depth_i{};
    depth_i.imageView = depth_img.view;
    depth_i.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo blura_i{};
    blura_i.imageView = blur_a_img.view;
    blura_i.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo blurb_i{};
    blurb_i.imageView = blur_b_img.view;
    blurb_i.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo lit_i{};
    lit_i.imageView = lit_img.view;
    lit_i.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo near_i{};
    near_i.sampler = near_sampler;
    VkDescriptorImageInfo lin_i{};
    lin_i.sampler = lin_sampler;
    const VkDescriptorImageInfo* img0[4] = {&occ_i, &blura_i, &lit_i, &lit_i};
    const VkDescriptorImageInfo* img2[4] = {&depth_i, &depth_i, &blurb_i,
                                            &blurb_i};
    const VkDescriptorImageInfo* smp[4] = {&near_i, &near_i, &lin_i, &lin_i};
    VkDescriptorSet dst[4] = {blur_h_set, blur_v_set, comp_on_set,
                              comp_off_set};
    for (int s = 0; s < 4; ++s) {
      VkWriteDescriptorSet w[3]{};
      for (int k = 0; k < 3; ++k) {
        w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[k].dstSet = dst[s];
      }
      w[0].dstBinding = 0;
      w[0].descriptorCount = 1;
      w[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      w[0].pImageInfo = img0[s];
      w[1].dstBinding = 1;
      w[1].descriptorCount = 1;
      w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
      w[1].pImageInfo = smp[s];
      w[2].dstBinding = 2;
      w[2].descriptorCount = 1;
      w[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      w[2].pImageInfo = img2[s];
      g.vk.update_sets(g.device, 3, w, 0, nullptr);
    }
    check(true, "ssao-gpu: ssao descriptors written");
  }

  // Record: upload src + LUT + depth/occ, compute pre-pass, shade,
  // blur H/V, composite ON + OFF, read back LDR pair + blurred floats.
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
  // One fullscreen pass record (target enters COLOR_ATTACHMENT_OPTIMAL;
  // positive-height viewport: framebuffer row 0 samples source texel row
  // 0 (vUv (0,0) lands on row 0), so the output rows need no CPU flip
  // before the golden compare — the same row convention the P4-05 IBL TU
  // uses with its CPU tonemap (px(x,y) indexes row y directly). NOTE:
  // the shade pass above keeps the P4-05 TU's negative-height viewport
  // verbatim (its golden was baked with it); only the ssao passes —
  // whose ssao_full.vert puts vUv (0,0) at NDC (-1,-1) — use positive
  // height so composite row y samples lit row y.
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
      // Upload source cube (6 layers).
      barrier_img(cb, src_img.img, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  6, 1);
      for (std::uint32_t layer = 0; layer < 6; ++layer) {
        VkBufferImageCopy rc{};
        rc.bufferOffset = static_cast<VkDeviceSize>(layer) *
                          static_cast<VkDeviceSize>(kSrc) * kSrc * 16;
        rc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
        rc.imageExtent = {kSrc, kSrc, 1};
        g.vk.cmd_copy_buf_img(cb, up_b.buf, src_img.img,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rc);
      }
      // Upload LUT tail (CPU BrdfLutReference at 16px).
      {
        barrier_img(cb, lut_img.img, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0,
                    VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, 1, 1);
        void* lp = nullptr;
        if (g.vk.map_mem(g.device, up_b.mem, src_bytes, lut_bytes, 0, &lp) ==
                VK_SUCCESS &&
            lp != nullptr) {
          auto* ld = static_cast<float*>(lp);
          for (std::uint32_t yy = 0; yy < 16; ++yy) {
            for (std::uint32_t xx = 0; xx < 16; ++xx) {
              float s = 0.0f;
              float b = 0.0f;
              engine::renderer::BrdfLutReference(xx, yy, 16, &s, &b);
              ld[(static_cast<std::size_t>(yy) * 16 + xx) * 2 + 0] = s;
              ld[(static_cast<std::size_t>(yy) * 16 + xx) * 2 + 1] = b;
            }
          }
          g.vk.unmap_mem(g.device, up_b.mem);
        } else {
          ok = false;
        }
        VkBufferImageCopy lc{};
        lc.bufferOffset = static_cast<VkDeviceSize>(src_bytes);
        lc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        lc.imageExtent = {16, 16, 1};
        g.vk.cmd_copy_buf_img(cb, up_b.buf, lut_img.img,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &lc);
        barrier_img(cb, lut_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1, 1);
      }
      // Upload half-res depth + occlusion.
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
      barrier_img(cb, irr_img.img, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_GENERAL, 0, 0,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, 6, 1);
      // Irradiance + prefilter pre-pass (mirrors the P4-05 TU).
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
      // Shade into HDR lit.
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
      barrier_img(cb, lit_img.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1, 1);
      // Blur H (occ/depth -> blur_a), blur V (blur_a/depth -> blur_b).
      barrier_img(cb, blur_a_img.img, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 0,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 1, 1);
      {
        engine::renderer::SsaoBlurPush push{};
        push.texel[0] = 1.0f / static_cast<float>(kHw);
        push.texel[1] = 1.0f / static_cast<float>(kHh);
        push.sigma = kSigma;
        push.horizontal = 1;
        record_full(cb, blur_a_img.view, kHw, kHh, blur_pipe, blur_pipe_layout,
                    1, blur_h_set, &push, sizeof(push),
                    VK_SHADER_STAGE_FRAGMENT_BIT);
      }
      barrier_img(cb, blur_a_img.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1, 1);
      barrier_img(cb, blur_b_img.img, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 0,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 1, 1);
      {
        engine::renderer::SsaoBlurPush push{};
        push.texel[0] = 1.0f / static_cast<float>(kHw);
        push.texel[1] = 1.0f / static_cast<float>(kHh);
        push.sigma = kSigma;
        push.horizontal = 0;
        record_full(cb, blur_b_img.view, kHw, kHh, blur_pipe, blur_pipe_layout,
                    1, blur_v_set, &push, sizeof(push),
                    VK_SHADER_STAGE_FRAGMENT_BIT);
      }
      barrier_img(cb, blur_b_img.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1, 1);
      // Composite ON into LDR (lit * blurred occ -> tonemap).
      barrier_img(cb, ldr_on_img.img, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 0,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 1, 1);
      {
        engine::renderer::SsaoCompositePush push{};
        push.exposure = kExposure;
        push.enabled = 1;
        record_full(cb, ldr_on_img.view, kW, kH, comp_pipe, comp_pipe_layout,
                    1, comp_on_set, &push, sizeof(push),
                    VK_SHADER_STAGE_FRAGMENT_BIT);
      }
      // Composite OFF into LDR (same lit, enabled 0 — the P4-05 path).
      barrier_img(cb, ldr_off_img.img, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 0,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 1, 1);
      {
        engine::renderer::SsaoCompositePush push{};
        push.exposure = kExposure;
        push.enabled = 0;
        record_full(cb, ldr_off_img.view, kW, kH, comp_pipe, comp_pipe_layout,
                    1, comp_off_set, &push, sizeof(push),
                    VK_SHADER_STAGE_FRAGMENT_BIT);
      }
      // Read back both LDR frames + the blurred occlusion floats.
      barrier_img(cb, ldr_on_img.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                  1, 1);
      {
        VkBufferImageCopy rl{};
        rl.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rl.imageExtent = {kW, kH, 1};
        g.vk.cmd_copy_img_buf(cb, ldr_on_img.img,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              read_on_b.buf, 1, &rl);
      }
      barrier_img(cb, ldr_off_img.img,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                  1, 1);
      {
        VkBufferImageCopy rl{};
        rl.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rl.imageExtent = {kW, kH, 1};
        g.vk.cmd_copy_img_buf(cb, ldr_off_img.img,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              read_off_b.buf, 1, &rl);
      }
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
      barrier_img(cb, irr_img.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                  6, 1);
      for (std::uint32_t layer = 0; layer < 6; ++layer) {
        VkBufferImageCopy rl{};
        rl.bufferOffset =
            static_cast<VkDeviceSize>(layer) * kIrr * kIrr * 16;
        rl.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
        rl.imageExtent = {kIrr, kIrr, 1};
        g.vk.cmd_copy_img_buf(cb, irr_img.img,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              read_irr_b.buf, 1, &rl);
      }
      barrier_img(cb, pref_img.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                  6, kPrefMips);
      for (std::uint32_t layer = 0; layer < 6; ++layer) {
        VkBufferImageCopy rl{};
        rl.bufferOffset =
            static_cast<VkDeviceSize>(layer) * kPref * kPref * 16;
        rl.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
        rl.imageExtent = {kPref, kPref, 1};
        g.vk.cmd_copy_img_buf(cb, pref_img.img,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              read_pref_b.buf, 1, &rl);
      }
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

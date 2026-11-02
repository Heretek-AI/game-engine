// bloom.hpp — P4-08: dual-Kawase bloom + PostProcess component.
//
// CPU-side bloom plan (no GPU needed for the contract tests): threshold +
// intensity settings on a blittable PostProcess component (the ECS
// authoring path, same register path as MaterialComponent — see
// gotcha 20), a CPU reference for the prefilter + composite math the
// GPU passes must match, and mip-chain sizing for the dual-filter
// downsample/upsample chain.
//
// GPU chain (test-local, test_bloom.cpp owns the passes; mirrors the
// P4-02 shade->post frame shape on lavapipe):
//
//   HDR scene (from the P4-02 shade pass)
//     -> prefilter (bright-pass: soft-knee) + down0 (1/2 res)
//     -> down1 (1/4) -> down2 (1/8)
//     -> up1 (1/4 = up(down2) + down1) -> up0 (1/2 = up(up1) + down0)
//     -> full (full-res = up(up0) + zero-fine)
//     -> composite: ldr = tonemap(hdr * exposure + bloom * intensity)
//
// intensity 0 reduces the composite to the exact post_composite op order
// (hdr * exposure -> aces -> srgb), so the toggle-off check is exact:
// the intensity-0 frame equals the P4-02 frame bit-for-bit. Threshold
// keeps the grey diffuse half (0.5) out of the bloom chain while the
// emissive-10 half feeds it.
//
// Compiles SDK-free (plain data + math, no GPU); the GPU pass wiring
// (image views + sampler + descriptor writes + fullscreen passes) lands
// in the test TU next to the P4-02 frame wiring.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
#include "engine/renderer/engine_status.hpp"

namespace engine::renderer {

// ECS component twin (blittable, 16B — same register path as
// LocalTransform/WorldTransform; see gotcha 20).
//   threshold: prefilter knee (linear-HDR luminance floor; values below
//     contribute nothing to the bloom chain).
//   intensity: composite strength (0 = toggle-off: output == tonemap(hdr)).
//   mip_count: downsample levels used (1..kBloomMaxMips).
//   flags: reserved (bit 0 = enable; 0 keeps intensity-0 semantics even
//     when intensity > 0, so a disabled component never blooms).
struct PostProcessComponent {
  float threshold{1.0f};
  float intensity{0.0f};
  std::uint32_t mip_count{3};
  std::uint32_t flags{0};
};
static_assert(sizeof(PostProcessComponent) == 16,
              "post-process component is 16B");
static_assert(alignof(PostProcessComponent) == 4,
              "post-process component is 4-byte aligned");

// Bloom chain constants: 3 downsample levels (1/2, 1/4, 1/8 of the
// 320x240 frame), 13-tap dual-Kawase kernel, soft-knee prefilter.
inline constexpr std::uint32_t kBloomMaxMips = 3;
inline constexpr std::uint32_t kBloomKernelTaps = 13;

// Clamp + validate one component in place (NaN/garbage fails closed with
// kInvalidArgument; negative threshold clamps to 0; mip_count clamps to
// [1, kBloomMaxMips]). Null -> kInvalidArgument.
auto ENGINE_CORE_API BloomValidate(PostProcessComponent* c) noexcept
    -> Status;

// Push-constant mirrors (must match bloom_down/up/composite.frag
// byte-for-byte; all <= 128B).
struct BloomDownPush {
  float texel[2]{0.0f, 0.0f};
  float threshold{1.0f};
  std::uint32_t prefilter{0};
};
static_assert(sizeof(BloomDownPush) == 16, "bloom down push is 16B");
struct BloomUpPush {
  float texel[2]{0.0f, 0.0f};
  std::uint32_t pad0{0};
  std::uint32_t pad1{0};
};
static_assert(sizeof(BloomUpPush) == 16, "bloom up push is 16B");
struct BloomCompositePush {
  float exposure{1.0f};
  float intensity{0.0f};
  std::uint32_t pad0{0};
  std::uint32_t pad1{0};
};
static_assert(sizeof(BloomCompositePush) == 16,
              "bloom composite push is 16B");

// Mip sizes for a WxH frame: mip i is ceil(max(1, W>>i)) x
// ceil(max(1, H>>i)) (halving, never 0). out_w/out_h hold kBloomMaxMips
// entries (null-safe: returns false, writes nothing).
[[nodiscard]] inline auto BloomMipSizes(std::uint32_t w, std::uint32_t h,
                                       std::uint32_t* out_w,
                                       std::uint32_t* out_h) noexcept
    -> bool {
  if (out_w == nullptr || out_h == nullptr || w == 0 || h == 0) {
    return false;
  }
  std::uint32_t cw = w;
  std::uint32_t ch = h;
  for (std::uint32_t i = 0; i < kBloomMaxMips; ++i) {
    cw = (cw > 1) ? (cw + 1) / 2 : 1;
    ch = (ch > 1) ? (ch + 1) / 2 : 1;
    out_w[i] = cw;
    out_h[i] = ch;
  }
  return true;
}

// CPU reference: prefilter one HDR texel (soft-knee bright-pass).
//   c: linear-HDR channel value; t: threshold.
//   returns max(0, c - t) with a quadratic knee of width t/2 below t
//   (continuous at the knee, zero below t/2). Mirrors bloom_down.frag
//   prefilter term-for-term; null-safe via reference args only.
[[nodiscard]] inline auto BloomPrefilterCpu(float c, float threshold) noexcept
    -> float {
  if (c <= 0.0f || threshold <= 0.0f) {
    return (c > threshold) ? (c - threshold) : 0.0f;
  }
  const float knee = threshold * 0.5f;
  if (c < threshold - knee) {
    return 0.0f;
  }
  if (c < threshold + knee) {
    const float x = c - (threshold - knee);
    return (x * x) / (4.0f * knee);
  }
  return c - threshold;
}

// CPU reference for the composite pass (mirrors bloom_composite.frag
// term-for-term: c = hdr * exposure + bloom * intensity -> Narkowicz
// ACES -> sRGB encode, single channel). intensity 0 reduces exactly to
// HdrTonemapCpu (x + 0.0 is exact for finite x, and the bloom chain only
// ever produces finite averages) — the toggle-off guarantee.
[[nodiscard]] inline auto BloomCompositeCpu(float hdr, float exposure,
                                           float bloom, float intensity) noexcept
    -> float {
  const float c = hdr * exposure + bloom * intensity;
  const float x = c;
  const float a = x * (2.51f * x + 0.03f);
  const float b = x * (2.43f * x + 0.59f) + 0.14f;
  float m = (b > 1e-6f) ? a / b : 0.0f;
  m = m < 0.0f ? 0.0f : (m > 1.0f ? 1.0f : m);
  const float lo = m * 12.92f;
  const double hi =
      1.055 * std::pow(m > 0.0f ? static_cast<double>(m) : 0.0, 1.0 / 2.4) -
      0.055;
  const auto hi_f = static_cast<float>(hi);
  const float out = (m <= 0.0031308f) ? lo : hi_f;
  return (out < 0.0f) ? 0.0f : ((out > 1.0f) ? 1.0f : out);
}

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif

// ssao.hpp — P4-09: screen-space ambient occlusion (GTAO half-res +
// bilateral blur + composite), CPU contract side.
//
// Merged from sibling cards t_390bf7c4 (GTAO occlusion pass: SsaoHalfSize,
// SsaoGtaoTap/SsaoOcclusionCpu, SsaoGtaoPush) and t_55a30693 (bilateral
// blur + composite + toggle: SsaoComponent/SsaoValidate, SsaoBlurWeight/
// SsaoBlurTap/SsaoBlurAxisCpu, SsaoApplyCpu, SsaoBlurPush/SsaoCompositePush).
// The two halves meet at the half-res occlusion texture (R32_SFLOAT,
// 160x120 for the 320x240 golden).
//
// Pipeline (test-local passes, like bloom/TAA — no new engine class):
//   HDR shade (P4-05 IBL split: chrome/rough) + depth + GTAO occlusion
//     -> bilateral blur H (half-res) -> bilateral blur V (half-res)
//     -> composite: ldr = tonemap(lit * occlusion * exposure)
//   OFF (enabled == 0): lit passes straight to HdrTonemapCpu with the
//   identical op order, so OFF reproduces the P4-05 golden within
//   tolerance (bit-exact on the same driver).
//
// Compiles SDK-free (plain data + math, no GPU); the test-local GLSL
// passes (ssao_gtao.frag, ssao_blur.frag, ssao_composite.frag) mirror the
// CPU reference term-for-term; the golden proves they agree.
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


// Depth step scene used by the proof (linear depth, 0 = near).
inline constexpr float kSsaoNearDepth = 0.2f;
inline constexpr float kSsaoFarDepth = 0.8f;
// Occlusion response: depth-difference gain and overall strength.
inline constexpr float kSsaoDepthScale = 4.0f;
inline constexpr float kSsaoStrength = 1.0f;
// Tap count: 3x3 neighbourhood minus center.
inline constexpr std::uint32_t kSsaoTapCount = 8;

// Half-res target size for a WxH frame: ceil-halve each axis, never 0
// (same rule as BloomMipSizes mip 0). For the 320x240 golden frame the
// occlusion texture is 160x120. Null-safe; rejects zero extents.
[[nodiscard]] inline auto SsaoHalfSize(std::uint32_t w, std::uint32_t h,
                                      std::uint32_t* out_hw,
                                      std::uint32_t* out_hh) noexcept
    -> bool {
  if (out_hw == nullptr || out_hh == nullptr || w == 0 || h == 0) {
    return false;
  }
  *out_hw = (w > 1) ? (w + 1) / 2 : 1;
  *out_hh = (h > 1) ? (h + 1) / 2 : 1;
  return true;
}

// One occlusion tap contribution: occluders are CLOSER neighbours
// (neighbor < center). Returns clamp((center - neighbor) * scale, 0, 1).
[[nodiscard]] inline auto SsaoGtaoTap(float center, float neighbor,
                                     float scale) noexcept -> float {
  const float d = (center - neighbor) * scale;
  if (d <= 0.0f) {
    return 0.0f;
  }
  return (d >= 1.0f) ? 1.0f : d;
}

// Full occlusion for one pixel: 1 - strength * mean(taps).
// neighbors must hold kSsaoTapCount entries (null-safe: null reads 0).
[[nodiscard]] inline auto SsaoOcclusionCpu(float center,
                                          const float* neighbors,
                                          float scale,
                                          float strength) noexcept -> float {
  float sum = 0.0f;
  for (std::uint32_t i = 0; i < kSsaoTapCount; ++i) {
    const float n = (neighbors != nullptr) ? neighbors[i] : 0.0f;
    sum += SsaoGtaoTap(center, n, scale);
  }
  const float occ = 1.0f - strength * (sum / 8.0f);
  if (occ <= 0.0f) {
    return 0.0f;
  }
  return (occ >= 1.0f) ? 1.0f : occ;
}

// Push-constant mirror for ssao_gtao.frag (16B, FRAGMENT stage only).
struct SsaoGtaoPush {
  float texel[2]{0.0f, 0.0f};  // 1/hw, 1/hh (informational; shader fetches)
  float depth_scale{kSsaoDepthScale};
  float strength{kSsaoStrength};
};
static_assert(sizeof(SsaoGtaoPush) == 16, "ssao gtao push is 16B");



// SSAO toggle component (blittable, 16B — same register path as
// PostProcessComponent; see bloom.hpp gotcha 20).
//   enabled: 0 = OFF (bypass GTAO + blur + composite, reproduce P4-05);
//     nonzero = ON (composite multiplies the blurred occlusion).
//   radius: GTAO sample radius in half-res texels (blur task reads it for
//     the depth-sigma scale only; the GTAO task owns sampling).
//   sigma: bilateral depth sigma (linear-depth units); larger blurs
//     across more depth difference. Must be > 0 when enabled.
//   pad: reserved (zero).
struct SsaoComponent {
  std::uint32_t enabled{1};
  float radius{1.0f};
  float sigma{0.02f};
  std::uint32_t pad{0};
};
static_assert(sizeof(SsaoComponent) == 16, "ssao component is 16B");
static_assert(alignof(SsaoComponent) == 4, "ssao component is 4-byte aligned");

// Blur kernel: 5 taps (center + +/-1 + +/-2) per separable pass
// (H then V). 5 taps keep the test-local frame cheap (2 passes x 5 taps
// on 160x120) while still killing single-texel GTAO noise; wider radii
// are the GTAO task's call via radius, not this header's.
inline constexpr std::uint32_t kSsaoBlurTaps = 5;

// Half-res target for a WxH frame: ceil-halve each axis (never 0),
// same rule as BloomMipSizes mip 0. Null-safe: returns false, writes
// nothing. The GTAO task owns the canonical SsaoHalfSizes; this inline
// is the blur-side mirror used by the composite UV math (kept here so
// the blur test TU does not depend on the GTAO half of the header).
[[nodiscard]] inline auto SsaoBlurHalfSize(std::uint32_t w, std::uint32_t h,
                                          std::uint32_t* out_w,
                                          std::uint32_t* out_h) noexcept
    -> bool {
  if (out_w == nullptr || out_h == nullptr || w == 0 || h == 0) {
    return false;
  }
  *out_w = (w > 1) ? (w + 1) / 2 : 1;
  *out_h = (h > 1) ? (h + 1) / 2 : 1;
  return true;
}

// Clamp + validate one component in place (NaN/garbage fails closed
// with kInvalidArgument; sigma <= 0 clamps to the default when enabled,
// radius clamps to [0.5, 4.0]). Null -> kInvalidArgument.
auto ENGINE_CORE_API SsaoValidate(SsaoComponent* c) noexcept -> Status;

// Push-constant mirrors (must match ssao_blur.frag / ssao_composite.frag
// byte-for-byte; all <= 128B).
struct SsaoBlurPush {
  float texel[2]{0.0f, 0.0f};  // 1/half-res-texel size (axis-selected)
  float sigma{0.02f};           // bilateral depth sigma
  std::uint32_t horizontal{1};  // 1 = H pass, 0 = V pass
};
static_assert(sizeof(SsaoBlurPush) == 16, "ssao blur push is 16B");
struct SsaoCompositePush {
  float exposure{1.0f};
  std::uint32_t enabled{1};  // 0 = OFF (lit passes through unmodified)
  std::uint32_t pad0{0};
  std::uint32_t pad1{0};
};
static_assert(sizeof(SsaoCompositePush) == 16, "ssao composite push is 16B");

// Bilateral edge weight: 1.0 when the neighbor sits at the same depth,
// decaying with |d_neighbor - d_center| / sigma (Gaussian falloff).
// Mirrors ssao_blur.frag weight term-for-term. sigma <= 0 -> delta-only
// (1.0 when depths are bit-equal, else 0.0) so a degenerate sigma can
// never produce NaN or a zero normalizer.
[[nodiscard]] inline auto SsaoBlurWeight(float depth_center,
                                        float depth_neighbor,
                                        float sigma) noexcept -> float {
  const float dd = depth_neighbor - depth_center;
  if (!(sigma > 0.0f)) {
    return (dd == 0.0f) ? 1.0f : 0.0f;
  }
  const float t = dd / sigma;
  // exp() on a finite non-positive arg is finite in (0, 1]; clamp the
  // arg defensively (float exp underflows to 0 past ~-87, never NaN).
  const float e = (t * t > 88.0f) ? 0.0f : std::exp(-t * t);
  return (e > 0.0f) ? e : 0.0f;
}

// One bilateral blur tap accumulator step: weight the neighbor occlusion
// by the depth edge weight and accumulate (sum_w += w, sum_o += w * o).
// Mirrors the ssao_blur.frag inner loop term-for-term (same op order:
// weight first, then the two FMAs).
inline void SsaoBlurTap(float occlusion_neighbor, float depth_center,
                        float depth_neighbor, float sigma, float* sum_w,
                        float* sum_o) noexcept {
  if (sum_w == nullptr || sum_o == nullptr) {
    return;
  }
  const float w = SsaoBlurWeight(depth_center, depth_neighbor, sigma);
  *sum_w += w;
  *sum_o += w * occlusion_neighbor;
}

// Full one-axis 5-tap bilateral blur of a single texel. occ/dpt hold
// the 5 taps centered at index 2 (offsets -2..+2 along the pass axis).
// Returns sum_o / sum_w (sum_w > 0 always: the center tap has weight 1).
// Mirrors ssao_blur.frag main() exactly (center included via Tap).
[[nodiscard]] inline auto SsaoBlurAxisCpu(const float occ[5],
                                         const float dpt[5],
                                         float sigma) noexcept -> float {
  float sum_w = 0.0f;
  float sum_o = 0.0f;
  for (int i = 0; i < 5; ++i) {
    SsaoBlurTap(occ[i], dpt[2], dpt[i], sigma, &sum_w, &sum_o);
  }
  return (sum_w > 0.0f) ? sum_o / sum_w : occ[2];
}

// Composite: multiply the blurred occlusion into the lit HDR triple,
// then tonemap with HdrTonemapCpu semantics (Narkowicz ACES + sRGB).
// enabled == 0 skips the multiply (NOT * 1.0f) so OFF is the identical
// op order as the P4-05 shade->tonemap path — the toggle-off guarantee
// (mirrors ssao_composite.frag: the shader branches on pc.enabled and
// the OFF branch samples no occlusion texture at all).
// Returns false on null pointers; out[i] is clamped to [0, 1].
[[nodiscard]] inline auto SsaoApplyCpu(const float lit[3], float occlusion,
                                      std::uint32_t enabled, float exposure,
                                      float out[3]) noexcept -> bool {
  if (lit == nullptr || out == nullptr) {
    return false;
  }
  float shaded[3] = {lit[0], lit[1], lit[2]};
  if (enabled != 0) {
    const float o = (occlusion < 0.0f)   ? 0.0f
                    : (occlusion > 1.0f) ? 1.0f
                                         : occlusion;
    shaded[0] *= o;
    shaded[1] *= o;
    shaded[2] *= o;
  }
  for (int k = 0; k < 3; ++k) {
    const float x = shaded[k] * exposure;
    const float a = x * (2.51f * x + 0.03f);
    const float b = x * (2.43f * x + 0.59f) + 0.14f;
    float m = (b > 1e-6f) ? a / b : 0.0f;
    m = m < 0.0f ? 0.0f : (m > 1.0f ? 1.0f : m);
    const float lo = m * 12.92f;
    const double hi =
        1.055 * std::pow(m > 0.0f ? static_cast<double>(m) : 0.0, 1.0 / 2.4) -
        0.055;
    const auto hi_f = static_cast<float>(hi);
    const float c = (m <= 0.0031308f) ? lo : hi_f;
    out[k] = (c < 0.0f) ? 0.0f : ((c > 1.0f) ? 1.0f : c);
  }
  return true;
}

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif

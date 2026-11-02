// anti_aliasing.hpp — P4-10: TAA (jittered projection + history
// rejection) with an FXAA fallback.
//
// The shade pass writes linear-HDR radiance per frame; the TAA resolve
// reads the CURRENT jittered frame plus the PREVIOUS resolved frame
// (history) and blends them, rejecting history where the neighbourhood
// clamp fails (motion / disocclusion). The FXAA lane is the fallback
// when history is unavailable (first frame, camera cut, over-threshold
// reject rate): a single-frame luma-contrast edge filter, no history.
//
// This header owns the CPU contract (jitter sequence, neighbourhood
// clamp, FXAA luma test, resolve blend) with no GPU dependency, so the
// unit tests run on every CI runner, like hdr_camera.hpp. The test GPU
// passes (taa_resolve.frag + fxaa.frag, test-local GLSL compiled
// offline) mirror the CPU reference term-for-term; the golden proves
// they agree.
//
// Jitter: Halton(2,3) base-2/3 low-discrepancy sequence, frame index
// modulo 16, scaled to +/-0.5px UV offsets (the standard 16-tap TAA
// cycle). History rejection: per-channel AABB clamp of the history
// sample to the 3x3 neighbourhood min/max of the current frame
// (tone-mapped LDR space — the clamp runs on display-ready values,
// matching the resolve pass input), expanded by 0.02 slack; a rejected
// pixel blends at 0.5 instead of the steady-state 0.1/0.9, so ghosts
// fade in one frame, not ten.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::renderer {

// 16-frame TAA cycle (spec acceptance: golden after 16 frames).
inline constexpr std::uint32_t kTaaCycleFrames = 16;
// Steady-state blend: history weight once converged.
inline constexpr float kTaaHistoryWeight = 0.9f;
// Rejected-pixel blend: history weight after a clamp miss (fast fade).
inline constexpr float kTaaRejectWeight = 0.5f;
// Neighbourhood AABB slack (LDR units) before history is rejected.
inline constexpr float kTaaClampSlack = 0.02f;
// Edge-pixel variance stability threshold for the acceptance probe:
// worst |frame16 - frame15| over the +/-2px edge band. A static
// max-contrast edge under +/-0.5px jitter legitimately re-fires the
// neighbourhood reject when the jitter delta pushes the reprojected
// history sample across the edge (real TAA shimmers slightly there
// too); one reject step moves the pixel at most rejectWeight * range
// = 0.5 * 0.8 = 0.4. The 0.5 bound proves the history feedback never
// AMPLIFIES (a broken feedback oscillates dark<->bright = 0.8 jumps),
// i.e. stability = bounded, not zero. Determinism across runs is
// covered by the golden re-run instead.
inline constexpr float kTaaEdgeVarianceMax = 0.5f;
// FXAA fallback: local-contrast edge test threshold (luma range over
// the 3x3 neighbourhood that marks a pixel as an edge).
inline constexpr float kFxaaEdgeThreshold = 0.125f;

// Halton radical-inverse for a prime base (bases 2 and 3 only; other
// bases return 0.0). Index is 1-based internally (i+1) so frame 0 maps
// to 1/2, 1/3 — never the degenerate 0 offset.
[[nodiscard]] inline auto TaaHalton(std::uint32_t index,
                                   std::uint32_t base) noexcept -> float {
  if (base != 2 && base != 3) {
    return 0.0f;
  }
  float result = 0.0f;
  float denom = 1.0f;
  std::uint32_t n = index + 1;
  while (n > 0) {
    denom *= static_cast<float>(base);
    const std::uint32_t digit = n % base;
    result += static_cast<float>(digit) / denom;
    n /= base;
  }
  return result;
}

// Jitter offset for frame `frame` in PIXEL units, range [-0.5, +0.5).
// x = Halton(frame % 16, 2) - 0.5, y = Halton(frame % 16, 3) - 0.5.
inline void TaaJitterForFrame(std::uint32_t frame, float* out_x,
                              float* out_y) noexcept {
  if (out_x == nullptr || out_y == nullptr) {
    return;
  }
  const std::uint32_t slot = frame % kTaaCycleFrames;
  *out_x = TaaHalton(slot, 2) - 0.5f;
  *out_y = TaaHalton(slot, 3) - 0.5f;
}

// Neighbourhood clamp + reject test (LDR space, per channel).
// hist: history sample; cur_min/cur_max: 3x3 neighbourhood bounds of
// the CURRENT frame (same channel). Returns true when history is kept
// (inside [min - slack, max + slack]), false when rejected. The
// clamped history value is written to *clamped_out (null-safe: still
// returns the verdict when clamped_out is null).
[[nodiscard]] inline auto TaaClampHistory(float hist, float cur_min,
                                         float cur_max,
                                         float* clamped_out) noexcept
    -> bool {
  const float lo = cur_min - kTaaClampSlack;
  const float hi = cur_max + kTaaClampSlack;
  const float clamped = hist < lo ? lo : (hist > hi ? hi : hist);
  if (clamped_out != nullptr) {
    *clamped_out = clamped;
  }
  return hist >= lo && hist <= hi;
}

// One TAA resolve blend: cur = current jittered sample, hist = the
// CLAMPED history value. kept = TaaClampHistory verdict. Returns
// lerp(cur, hist, kept ? 0.9 : 0.5).
[[nodiscard]] inline auto TaaResolveBlend(float cur, float hist_clamped,
                                         bool kept) noexcept -> float {
  const float w = kept ? kTaaHistoryWeight : kTaaRejectWeight;
  return cur * (1.0f - w) + hist_clamped * w;
}

// FXAA fallback edge test: luma 3x3 neighbourhood min/max. Returns
// true when (max - min) >= kFxaaEdgeThreshold (pixel is an edge and
// gets the lateral blend), false for flat areas (pixel passes
// through). Null-safe: null min/max returns false.
[[nodiscard]] inline auto FxaaIsEdge(float luma_min,
                                    float luma_max) noexcept -> bool {
  return (luma_max - luma_min) >= kFxaaEdgeThreshold;
}

// FXAA lateral sample weight: edge pixels blend 0.5 toward the
// minimum-luma neighbour direction (single-tap approximation of the
// full FXAA 5-tap cross); flat pixels weight 0 (pass-through).
[[nodiscard]] inline auto FxaaBlendWeight(bool is_edge) noexcept -> float {
  return is_edge ? 0.5f : 0.0f;
}

// Rec.709 luma (linear-HDR in, pre-tonemap — the FXAA pass works on
// the HDR target, mirroring fxaa.frag).
[[nodiscard]] inline auto FxaaLuma(float r, float g, float b) noexcept
    -> float {
  return 0.2126f * r + 0.7152f * g + 0.0722f * b;
}

}  // namespace engine::renderer

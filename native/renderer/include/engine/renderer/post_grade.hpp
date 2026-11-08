// post_grade.hpp — P4-11: color-grading LUT + vignette post effects.
//
// Thin CPU-side contracts for the two PostProcess ordered-list effects
// owned by t_4266178f (LUT = effect id 2, vignette = effect id 3; height
// fog = id 1 belongs to sibling t_a42ec749 — ids are local to this slice,
// no shared enum yet). Renderer-local blittable params, same pattern as
// P4-08 bloom.hpp (PostProcessComponent + Push mirrors + CPU references;
// no engine_abi.h touch, no third-party LUT library — analytic grade).
//
// LUT model (analytic 3-param grade, no texture):
//   c' = clamp(c * gain + lift, 0, 1) per channel.
// Identity (lift 0, gain 1) is an exact passthrough for the toggle-off
// check: grade-identity frame must equal its input bit-for-bit.
// Push GradePush (16B) mirrors post_grade.frag byte-for-byte.
//
// Vignette model:
//   d = length((uv - center) * aspect_fix) / radius; f = 1 - intensity *
//   smoothstep(1 - softness, 1.0, d); c' = c * f.
// intensity 0 is an exact passthrough (f == 1 for every pixel).
// Push VignettePush (32B) mirrors post_vignette.frag byte-for-byte.
#pragma once

#include <algorithm>
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

// Ordered-list effect ids owned by this slice (fog = 1 is the sibling's).
inline constexpr std::uint32_t kPostEffectColorGrade = 2;
inline constexpr std::uint32_t kPostEffectVignette = 3;

// Analytic color grade: per-channel lift (add) + gain (mul).
// Blittable, 4-byte aligned (NOT alignas(64): only built-in transform
// columns get 64B per repo invariants).
struct ColorGradeParams {
  float lift[3]{0.0f, 0.0f, 0.0f};
  float gain[3]{1.0f, 1.0f, 1.0f};
};
static_assert(sizeof(ColorGradeParams) == 24,
              "color grade params must be 24 bytes");
static_assert(alignof(ColorGradeParams) == 4,
              "color grade params must be 4-byte aligned");

// Vignette: darkens toward the frame edges.
//   center: normalized UV center (0.5, 0.5 default).
//   radius: normalized corner distance where full darkening starts.
//   softness: width of the smoothstep falloff band below radius.
//   intensity: 0 = passthrough, 1 = full darkening at corners.
struct VignetteParams {
  float center[2]{0.5f, 0.5f};
  float radius{0.75f};
  float softness{0.35f};
  float intensity{0.45f};
};
static_assert(sizeof(VignetteParams) == 20,
              "vignette params must be 20 bytes");
static_assert(alignof(VignetteParams) == 4,
              "vignette params must be 4-byte aligned");

// Clamp + validate one grade in place (NaN/garbage fails closed with
// kInvalidArgument; gain clamps to >= 0; lift clamps to [-1, 1]).
// Null -> kInvalidArgument.
auto ENGINE_CORE_API ColorGradeValidate(ColorGradeParams* c) noexcept
    -> Status;

// Clamp + validate one vignette in place (NaN fails closed; radius
// clamps to [0.05, 2.0]; softness to [0.01, 1.0]; intensity to [0, 1]).
// Null -> kInvalidArgument.
auto ENGINE_CORE_API VignetteValidate(VignetteParams* c) noexcept -> Status;

// Push-constant mirrors (must match post_grade.frag / post_vignette.frag
// byte-for-byte; all <= 128B).
struct GradePush {
  float lift_gain[6]{0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
  std::uint32_t pad0{0};
  std::uint32_t pad1{0};
};
static_assert(sizeof(GradePush) == 32, "grade push must be 32 bytes");
struct VignettePush {
  float center_radius[3]{0.5f, 0.5f, 0.75f};
  float softness{0.35f};
  float intensity{0.45f};
  float aspect{1.3333334f};
  std::uint32_t pad0{0};
  std::uint32_t pad1{0};
};
static_assert(sizeof(VignettePush) == 32, "vignette push must be 32 bytes");

// CPU reference: grade one channel (mirrors post_grade.frag term-for-term:
// c * gain + lift, clamped to [0, 1]).
[[nodiscard]] inline auto ColorGradeCpu(float c, float lift,
                                       float gain) noexcept -> float {
  const float v = c * gain + lift;
  return (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v);
}

// CPU reference: vignette factor for one UV (mirrors post_vignette.frag
// term-for-term; aspect-corrected distance, smoothstep falloff).
[[nodiscard]] inline auto VignetteFactorCpu(float u, float v, float cu,
                                           float cv, float radius,
                                           float softness, float aspect,
                                           float intensity) noexcept
    -> float {
  const float dx = (u - cu) * aspect;
  const float dy = v - cv;
  const float d = std::sqrt(dx * dx + dy * dy) / radius;
  const float edge0 = 1.0f - softness;
  const float edge1 = 1.0f;
  float t = (d - edge0) / (edge1 - edge0);
  t = (t < 0.0f) ? 0.0f : ((t > 1.0f) ? 1.0f : t);
  const float s = t * t * (3.0f - 2.0f * t);
  return 1.0f - intensity * s;
}

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif

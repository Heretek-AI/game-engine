// gi_shadow.cpp — M3-04 AC-3: SDSM cascade splits + soft-shadow +
// ACES grade CPU references.
#include "engine/renderer/gi_shadow.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::renderer {

auto ShadowMapPlan::init(float view_near, float view_far,
                         float texel_world_base) noexcept -> Status {
  if (!(view_near > 0.0f) || !(view_far > view_near) ||
      !(texel_world_base > 0.0f)) {
    return MakeFail(StatusCode::kInvalidArgument, "shadow plan: bad range");
  }
  // Practical SDSM split: lambda=0.5 log/uniform blend.
  constexpr float kLambda = 0.5f;
  for (std::uint32_t i = 0; i < kShadowCascades; ++i) {
    const float f = static_cast<float>(i + 1) /
                    static_cast<float>(kShadowCascades);
    const float log_split =
        view_near * std::pow(view_far / view_near, f);
    const float uni_split = view_near + (view_far - view_near) * f;
    const float split = kLambda * log_split + (1.0f - kLambda) * uni_split;
    const float prev = (i == 0) ? view_near
                                : cascades_[i - 1].far_dist;
    cascades_[i].near_dist = prev;
    cascades_[i].far_dist = split;
    cascades_[i].tile_x = (i % kShadowAtlasTilesX) * kShadowTileRes;
    cascades_[i].tile_y = (i / kShadowAtlasTilesX) * kShadowTileRes;
    // Texel density halves per cascade outward (2x spacing growth).
    float tw = texel_world_base;
    for (std::uint32_t l = 0; l < i; ++l) {
      tw *= 2.0f;
    }
    cascades_[i].texel_world = tw;
  }
  ready_ = true;
  return MakeOk();
}

auto ShadowMapPlan::cascade(std::uint32_t i) const noexcept
    -> const ShadowCascade& {
  static const ShadowCascade kEmpty{};
  if (!ready_ || i >= kShadowCascades) {
    return kEmpty;
  }
  return cascades_[i];
}

auto ShadowMapPlan::atlas_bytes() noexcept -> std::uint64_t {
  constexpr std::uint64_t kSide = static_cast<std::uint64_t>(kShadowTileRes) *
                                  kShadowAtlasTilesX;
  return kSide * kSide * 4;  // R32F
}

auto ShadowMapPlan::soft_shadow(float blocker_fraction,
                                float penumbra) noexcept -> float {
  float b = blocker_fraction;
  if (!(b >= 0.0f)) {
    b = 0.0f;
  }
  if (b > 1.0f) {
    b = 1.0f;
  }
  float p = penumbra;
  if (!(p >= 0.0f)) {
    p = 0.0f;
  }
  if (p > 1.0f) {
    p = 1.0f;
  }
  // Contact hardening: full blockers stay dark (penumbra only softens
  // partial occlusion — matches the PCSS widen in shadow_trace.slang).
  return 1.0f - b * (0.35f + 0.65f * p);
}

auto ShadowMapPlan::tonemap_grade(const float rgb[3],
                                  const ShadowGradePush& grade, float u,
                                  float v, float out_rgb[3]) noexcept -> void {
  // Narkowicz ACES fit (mirrors PbrAcesTonemap in pbr_common.slang).
  float mapped[3];
  for (int k = 0; k < 3; ++k) {
    const float x = rgb[k] * grade.exposure;
    const float a = x * (2.51f * x + 0.03f);
    const float b = x * (2.43f * x + 0.59f) + 0.14f;
    mapped[k] = a / (b > 1e-6f ? b : 1e-6f);
    if (mapped[k] < 0.0f) {
      mapped[k] = 0.0f;
    }
    if (mapped[k] > 1.0f) {
      mapped[k] = 1.0f;
    }
  }
  // Contrast around the 0.18 middle-grey pivot.
  for (int k = 0; k < 3; ++k) {
    mapped[k] = 0.18f + (mapped[k] - 0.18f) * grade.contrast;
    if (mapped[k] < 0.0f) {
      mapped[k] = 0.0f;
    }
    if (mapped[k] > 1.0f) {
      mapped[k] = 1.0f;
    }
  }
  // Saturation via luma mix.
  {
    const float luma =
        0.2126f * mapped[0] + 0.7152f * mapped[1] + 0.0722f * mapped[2];
    for (int k = 0; k < 3; ++k) {
      mapped[k] = luma + (mapped[k] - luma) * grade.saturation;
      if (mapped[k] < 0.0f) {
        mapped[k] = 0.0f;
      }
      if (mapped[k] > 1.0f) {
        mapped[k] = 1.0f;
      }
    }
  }
  // Vignette: radial falloff from the frame center.
  {
    const float dx = u - 0.5f;
    const float dy = v - 0.5f;
    const float r2 = dx * dx + dy * dy;
    const float vig = 1.0f - grade.vignette * r2 * 2.0f;
    const float vv = vig < 0.0f ? 0.0f : (vig > 1.0f ? 1.0f : vig);
    for (int k = 0; k < 3; ++k) {
      mapped[k] *= vv;
    }
  }
  // Linear -> sRGB encode (mirrors PbrLinearToSrgb).
  for (int k = 0; k < 3; ++k) {
    const float c = mapped[k] < 0.0f ? 0.0f : mapped[k];
    const float lo = c * 12.92f;
    const float hi =
        1.055f * std::pow(c > 0.0f ? c : 0.0f, 1.0f / 2.4f) - 0.055f;
    out_rgb[k] = (c <= 0.0031308f) ? lo : hi;
  }
}

}  // namespace engine::renderer

// terrain_splat_material.cpp — P6-01: slope/height blend weight rules.
#include "engine/terrain/terrain_splat_material.hpp"

namespace engine::terrain {

auto SplatMaterial::resolve_weights(const SplatLayer* layers, float height,
                                    float slope01,
                                    SplatWeights& out) noexcept -> void {
  if (layers == nullptr) {
    out.w[0] = 1.0F;
    out.w[1] = 0.0F;
    out.w[2] = 0.0F;
    out.w[3] = 0.0F;
    return;
  }
  float raw[4]{0.0F, 0.0F, 0.0F, 0.0F};
  for (std::uint32_t i = 0; i < kLayerCount; ++i) {
    const SplatLayer& L = layers[i];
    // Height band: smoothstep-ish tent between min/max. A zero-width band
    // matches everything (open layer, e.g. rock-by-slope only).
    float h_w = 1.0F;
    const float band = L.max_height - L.min_height;
    if (band > 0.0F) {
      if (height <= L.min_height || height >= L.max_height) {
        h_w = 0.0F;
      } else {
        const float t = (height - L.min_height) / band;
        const float edge = t < 1.0F - t ? t : 1.0F - t;
        h_w = edge * 2.0F;  // 0 at band edges, 1 at center
      }
    }
    // Slope cap: linear falloff to 0 at max_slope.
    float s_w = 1.0F;
    if (L.max_slope > 0.0F) {
      if (slope01 >= L.max_slope) {
        s_w = 0.0F;
      } else {
        s_w = 1.0F - slope01 / L.max_slope;
      }
    }
    raw[i] = h_w * s_w * (L.weight < 0.0F ? 0.0F : L.weight);
  }
  float sum = raw[0] + raw[1] + raw[2] + raw[3];
  if (sum <= 0.0F) {
    out.w[0] = 1.0F;
    out.w[1] = 0.0F;
    out.w[2] = 0.0F;
    out.w[3] = 0.0F;
    return;
  }
  const float inv = 1.0F / sum;
  out.w[0] = raw[0] * inv;
  out.w[1] = raw[1] * inv;
  out.w[2] = raw[2] * inv;
  out.w[3] = raw[3] * inv;
}

}  // namespace engine::terrain

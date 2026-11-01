// terrain_splat_material.hpp — P6-01: 4-layer splat material contract.
//
// CPU-side material data for terrain texturing: up to 4 detail layers,
// each with a bindless albedo slot + tiling + blend weight derived from
// slope/height rules. The GPU bindless upload (P4-01 path) consumes the
// resolved `SplatLayer` array; this header owns the CPU rule evaluation
// so tests and the editor (P6-02 splat painting) share one code path.
//
// Blend rules (evaluated per-vertex by `resolve_weights`, CPU mirror of
// the shader logic): layer 0 rock by slope, layer 1 grass by height band,
// layer 2 dirt by height band, layer 3 snow by height + slope cap. The
// caller may override weights per-vertex afterwards (P6-02 painting).
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"

namespace engine::terrain {

// One splat layer: bindless texture reference + tiling + blend params.
struct SplatLayer {
  std::uint32_t albedo_slot{0xFFFFFFFFu};  // bindless slot (0xFFFF.. = none)
  float tiling{16.0F};   // world-space repeat (uv * tiling / terrain_size)
  float min_height{0.0F};  // blend band start (world Y)
  float max_height{1.0F};  // blend band end (world Y)
  float max_slope{1.0F};  // slope (1-ny) above which weight -> 0
  float weight{1.0F};     // global multiplier (painting scales this)
};

// Four blend weights, one per layer, normalized to sum 1.
struct SplatWeights {
  float w[4]{0.0F, 0.0F, 0.0F, 0.0F};
};

class ENGINE_CORE_API SplatMaterial {
 public:
  static constexpr std::uint32_t kLayerCount = 4;

  // Evaluate blend weights for a vertex at (height, slope01 = 1 - ny)
  // against `layers` (exactly 4). Weights are normalized; a degenerate
  // band (all zero) falls back to layer 0 = 1.
  static auto resolve_weights(const SplatLayer* layers, float height,
                              float slope01, SplatWeights& out) noexcept
      -> void;
};

}  // namespace engine::terrain

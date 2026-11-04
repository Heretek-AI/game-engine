// mesh_lod.hpp — P4-12: mesh LOD chain + screen-size selection.
//
// Import time: BuildLodChain() wraps meshopt_simplify into the 4-level
// chain the .mesh artifact stores (LOD0 = full optimized index buffer,
// LOD1..3 = successive simplifies at fixed fractions of the LOD0 index
// count). Runtime: SelectLodIndex() maps a projected screen coverage to
// the chain level, so render_world draws distant meshes with fewer
// triangles. Crossfade is a CPU-side blend factor (CrossfadeFactor()):
// the frame draws the selected level only; the factor lets a future
// two-draw dither pass blend LOD n/n+1 without changing this contract.
//
// All CPU-side, SDK-free, no allocation beyond the caller's vectors.
// Thresholds are fractions of the LOD0 screen coverage at which the
// mesh was authored (SelectLodThresholds): level 0 covers >= 50% of
// the reference coverage, level 1 >= 25%, level 2 >= 12.5%, else 3.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine::renderer {

// LOD chain depth: LOD0 (full) + 3 simplified levels.
inline constexpr std::size_t kMeshLodCount = 4;

// Simplify target for LOD n (n >= 1), as a fraction of the LOD0 index
// count. Geometric halving: 1/2, 1/4, 1/8. Matches BuildLodChain.
inline constexpr std::array<float, kMeshLodCount> kMeshLodRatios = {
    1.0F, 0.5F, 0.25F, 0.125F};

// Minimum surviving index count for a simplified level (4 triangles:
// below this the level is noise, keep the parent instead).
inline constexpr std::size_t kMeshLodMinIndices = 12;

// Screen-coverage thresholds (fraction of the LOD0 reference coverage)
// at which each level is selected. Level 0 needs >= 0.5, level 1
// >= 0.25, level 2 >= 0.125; anything smaller is level 3.
inline constexpr std::array<float, kMeshLodCount> kMeshLodThresholds = {
    1.0F, 0.5F, 0.25F, 0.125F};

// Crossfade band (fraction of the threshold span): within this band
// below a threshold the blend factor ramps 0 -> 1 toward the coarser
// level, so a future dither pass can hide the pop.
inline constexpr float kMeshLodCrossfadeBand = 0.1F;

// Select the LOD level for `coverage_frac` (projected screen coverage /
// LOD0 reference coverage, >= 0). Higher coverage -> finer level.
// Pure function, no state; clamps out-of-range input (negative -> 3,
// >= 1 -> 0; NaN fails safe to 0/empty handled by caller guard).
[[nodiscard]] inline auto SelectLodIndex(float coverage_frac) noexcept
    -> std::size_t {
  if (!(coverage_frac >= kMeshLodThresholds[1])) {
    if (coverage_frac >= kMeshLodThresholds[2]) {
      return 1;
    }
    if (coverage_frac >= kMeshLodThresholds[3]) {
      return 2;
    }
    return 3;
  }
  return 0;
}

// Crossfade factor toward the NEXT coarser level at `coverage_frac`:
// 0.0 = fully on the selected level, 1.0 = fully on level+1. Zero
// outside the band below each threshold (and at the extremes: LOD0
// well above its threshold, LOD3, has no coarser level to fade to).
// `level` must be the SelectLodIndex(coverage_frac) result; mismatched
// input still returns a bounded [0, 1] value.
[[nodiscard]] inline auto CrossfadeFactor(float coverage_frac,
                                          std::size_t level) noexcept
    -> float {
  if (level >= kMeshLodCount - 1) {
    return 0.0F;
  }
  const std::size_t next = level + 1;
  if (next >= kMeshLodThresholds.size()) {
    return 0.0F;
  }
  const float thresh = kMeshLodThresholds[next];
  const float band = thresh * kMeshLodCrossfadeBand;
  if (!(coverage_frac >= 0.0F) || !(band > 0.0F)) {
    return 0.0F;
  }
  if (coverage_frac >= thresh) {
    return 0.0F;
  }
  if (coverage_frac <= thresh - band) {
    return 0.0F;
  }
  const float f = (thresh - coverage_frac) / band;
  if (f <= 0.0F) {
    return 0.0F;
  }
  if (f >= 1.0F) {
    return 1.0F;
  }
  return f;
}

// Coverage fraction from geometry: projected screen height of a sphere
// of `bounding_radius` at `distance` over the `ref_coverage` the mesh
// was authored at. Guards: distance <= 0 -> 1.0 (closest = finest);
// radius <= 0 or ref <= 0 -> 0.0 (degenerate = coarsest).
[[nodiscard]] inline auto CoverageFromDistance(float distance,
                                               float bounding_radius,
                                               float ref_coverage) noexcept
    -> float {
  if (!(distance > 0.0F)) {
    return 1.0F;
  }
  if (!(bounding_radius > 0.0F) || !(ref_coverage > 0.0F)) {
    return 0.0F;
  }
  return (bounding_radius / distance) / ref_coverage;
}

}  // namespace engine::renderer

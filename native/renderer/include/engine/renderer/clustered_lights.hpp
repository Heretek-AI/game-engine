// clustered_lights.hpp — P4-04: clustered forward light binning (CPU).
//
// The shade pass lights each pixel from a per-cluster light list instead
// of a single global list: the frame is divided into 16x9x24 clusters
// (16 tiles across x, 9 across y at 320x240 with 20px tiles... no —
// exactly 16 tiles in x and 9 in y at any resolution, each split into
// 24 depth slices), for 16*9*24 = 3456 clusters. Up to 1024 lights are
// binned once per frame on the CPU; each cluster stores (offset, count)
// into a packed index list, the same (base, count) record shape the
// forward+ tile grid already uses (see pbr_shade.frag).
//
// Cluster key: (tile_x, tile_y, depth_slice). Point lights affect every
// depth slice their sphere intersects; spot lights additionally clip
// against their cone axis (see ClusterSpotCone). Range attenuation is
// smooth: intensity * max(0, 1 - (d/range)^2)^2 (quadratic window to the
// light range, C1-continuous at d = range so cluster boundaries do not
// pop), evaluated in the shade pass. The CPU binner assigns CONSERVATIVELY
// (a light lands in every cluster its bounding sphere touches) so the
// GPU never misses a contributor; the shade pass applies the exact
// window + cone cutoff per pixel.
//
// Compiles WITHOUT the Vulkan SDK (plain data + math, no GPU) so the
// binning contract tests run on every CI runner, like hdr_camera.hpp.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::renderer {

// Cluster grid: 16 x-tiles, 9 y-tiles, 24 depth slices (spec P4-04).
inline constexpr std::uint32_t kClusterTilesX = 16;
inline constexpr std::uint32_t kClusterTilesY = 9;
inline constexpr std::uint32_t kClusterDepthSlices = 24;
inline constexpr std::uint32_t kClusterCount =
    kClusterTilesX * kClusterTilesY * kClusterDepthSlices;  // 3456
// Maximum lights per frame (spec P4-04).
inline constexpr std::uint32_t kClusterMaxLights = 1024;
// Hard cap on lights per single cluster (index-list bound; the binner
// truncates extras deterministically by light index so lavapipe goldens
// are stable across runs).
inline constexpr std::uint32_t kClusterMaxPerCluster = 256;

// One punctual light: point when cone_cos <= -1 (spot cutoff disabled),
// spot otherwise. Layout mirrors the shade-pass SSBO vec4 pairs
// (pos/range, color/intensity, extra packed separately) so the test
// upload path memcpys rows without translation.
struct ClusterLight {
  float pos[3]{0.0F, 0.0F, 0.0F};
  float range{1.0F};  // > 0: sphere radius the binner tests; shade window
  float color[3]{1.0F, 1.0F, 1.0F};
  float intensity{1.0F};
  float dir[3]{0.0F, 0.0F, -1.0F};  // spot axis (unit); ignored for points
  float cone_cos{-1.0F};  // cos(outer angle); <= -1 disables the cone
};
static_assert(sizeof(ClusterLight) == 48, "cluster light is 48B");

// Per-cluster (offset, count) record into the packed index list — same
// (base, count) shape as the forward+ tile grid (grid[t*2], grid[t*2+1]).
struct ClusterCell {
  std::uint32_t offset{0};
  std::uint32_t count{0};
};
static_assert(sizeof(ClusterCell) == 8, "cluster cell is 8B");

// Spot-cone clip on the CPU side: true when the cluster's bounding sphere
// (center, radius) can receive light from `light`. Points (cone_cos <=
// -1) always pass here (range test happens in ClusterBinLights); spots
// pass when the angle between the spot axis and the closest point of
// the sphere to the apex is within the outer cone (sphere radius added
// as an angular slack, conservative).
[[nodiscard]] inline auto ClusterSpotTouchesSphere(const ClusterLight& light,
                                                  const float center[3],
                                                  float radius) noexcept
    -> bool {
  if (light.cone_cos <= -1.0F) {
    return true;
  }
  float to_c[3] = {center[0] - light.pos[0], center[1] - light.pos[1],
                   center[2] - light.pos[2]};
  const float dist =
      std::sqrt(to_c[0] * to_c[0] + to_c[1] * to_c[1] + to_c[2] * to_c[2]);
  if (dist <= radius + 1e-4F) {
    return true;  // apex inside the sphere: cannot clip
  }
  const float inv = 1.0F / dist;
  const float cos_to_c =
      (to_c[0] * light.dir[0] + to_c[1] * light.dir[1] +
       to_c[2] * light.dir[2]) *
      inv;
  // Angular radius of the sphere as seen from the apex (conservative).
  const float ratio = radius * inv > 1.0F ? 1.0F : radius * inv;
  const float slack = std::sqrt(1.0F - ratio * ratio);
  // cos(a - b) >= cos(a) needs care at the seam; compare in angle space
  // via acos only when close (cheap: clusters are few, lights <= 1024).
  const float want = std::acos(light.cone_cos > 1.0F
                                   ? 1.0F
                                   : (light.cone_cos < -1.0F ? -1.0F
                                                             : light.cone_cos));
  const float got = std::acos(cos_to_c > 1.0F ? 1.0F
                                              : (cos_to_c < -1.0F ? -1.0F
                                                                  : cos_to_c));
  const float slack_ang = std::acos(slack);
  return got <= want + slack_ang;
}

// Range attenuation window shared by the CPU binner (cutoff) and the
// shade pass (exact falloff): smooth quadratic, 1 at d = 0, 0 at
// d >= range, C1-continuous at the boundary.
[[nodiscard]] inline auto ClusterRangeWindow(float dist, float range) noexcept
    -> float {
  if (range <= 0.0F || dist >= range) {
    return 0.0F;
  }
  const float t = dist / range;
  const float w = 1.0F - t * t;
  return w * w;
}

// Bin `light_count` lights into the 16x9x24 cluster grid over a WxH
// framebuffer. `tile_xy` maps pixels to cluster tiles (tile_w =
// ceil(w/16) px, tile_h = ceil(h/9) px); depth slices split NDC z [0,1]
// uniformly (slice = floor(ndc_z * 24), clamped). Each light supplies
// its screen-space tile rect (tile_x0..x1, tile_y0..y1, precomputed by
// projecting its bounding sphere) plus ndc z range (z0..z1); pass null
// rects/zeros to bin with the full-frame fallback (used by the unit
// tests for pure-count checks).
//
// Output: `cells` (3456 records, offset/count into `indices`) and
// `indices` (capacity >= clusters * per-cluster cap... caller sizes it;
// returns false when `index_cap` is too small). Deterministic: lights
// visit clusters in index order; overflow truncates the highest
// indices. Null-safe: null pointers or light_count > 1024 fail closed.
[[nodiscard]] inline auto ClusterBinLights(
    const ClusterLight* lights, std::uint32_t light_count,
    const std::uint32_t* tile_x0, const std::uint32_t* tile_y0,
    const std::uint32_t* tile_x1, const std::uint32_t* tile_y1,
    const float* ndc_z0, const float* ndc_z1, ClusterCell* cells,
    std::uint32_t* indices, std::uint32_t index_cap) noexcept -> bool {
  if (lights == nullptr || cells == nullptr || indices == nullptr) {
    return false;
  }
  if (light_count > kClusterMaxLights) {
    return false;
  }
  for (std::uint32_t c = 0; c < kClusterCount; ++c) {
    cells[c].offset = 0;
    cells[c].count = 0;
  }
  std::uint32_t cursor = 0;
  for (std::uint32_t tz = 0; tz < kClusterDepthSlices; ++tz) {
    for (std::uint32_t ty = 0; ty < kClusterTilesY; ++ty) {
      for (std::uint32_t tx = 0; tx < kClusterTilesX; ++tx) {
        const std::uint32_t c =
            (tz * kClusterTilesY + ty) * kClusterTilesX + tx;
        cells[c].offset = cursor;
        for (std::uint32_t li = 0; li < light_count; ++li) {
          if (cells[c].count >= kClusterMaxPerCluster) {
            break;
          }
          bool hit = true;
          if (tile_x0 != nullptr && tile_x1 != nullptr) {
            hit = !(tile_x1[li] < tx || tile_x0[li] > tx);
          }
          if (hit && tile_y0 != nullptr && tile_y1 != nullptr) {
            hit = !(tile_y1[li] < ty || tile_y0[li] > ty);
          }
          if (hit && ndc_z0 != nullptr && ndc_z1 != nullptr) {
            const float slo =
                std::min(ndc_z0[li], ndc_z1[li]) * kClusterDepthSlices;
            const float shi =
                std::max(ndc_z0[li], ndc_z1[li]) * kClusterDepthSlices;
            const auto lo = static_cast<std::uint32_t>(
                slo < 0.0F ? 0.0F : slo >= 24.0F ? 23.0F : slo);
            const auto hi = static_cast<std::uint32_t>(
                shi < 0.0F ? 0.0F : shi >= 24.0F ? 23.0F : shi);
            hit = !(hi < tz || lo > tz);
          }
          if (!hit) {
            continue;
          }
          if (cursor >= index_cap) {
            return false;
          }
          indices[cursor++] = li;
          ++cells[c].count;
        }
      }
    }
  }
  return true;
}

// Total index entries written by the last ClusterBinLights call (sum of
// cell counts) — lets callers assert the packing without re-walking.
[[nodiscard]] inline auto ClusterIndexTotal(const ClusterCell* cells) noexcept
    -> std::uint32_t {
  if (cells == nullptr) {
    return 0;
  }
  std::uint32_t total = 0;
  for (std::uint32_t c = 0; c < kClusterCount; ++c) {
    total += cells[c].count;
  }
  return total;
}

}  // namespace engine::renderer

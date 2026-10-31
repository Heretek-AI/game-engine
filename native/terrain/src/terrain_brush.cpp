// terrain_brush.cpp — P6-02: brush dab math + stroke journal.
#include "engine/terrain/terrain_brush.hpp"

#include <cmath>
#include <cstring>

#include "engine/terrain/terrain_heightmap.hpp"
#include "engine/terrain/terrain_splat_material.hpp"

namespace engine::terrain {
namespace {

// Grid sample world position.
inline auto sample_x(const HeightmapGrid& g, std::uint32_t x) noexcept
    -> float {
  return static_cast<float>(x) * g.spacing;
}

inline auto sample_z(const HeightmapGrid& g, std::uint32_t z) noexcept
    -> float {
  return static_cast<float>(z) * g.spacing;
}

inline auto valid_grid(const HeightmapGrid& g) noexcept -> bool {
  return g.samples != nullptr && g.width > 0 && g.height > 0 &&
         g.spacing > 0.0F;
}

}  // namespace

auto TerrainBrush::falloff(float dist, float radius) noexcept -> float {
  if (!(radius > 0.0F) || !(dist < radius)) {
    return 0.0F;
  }
  if (dist <= 0.0F) {
    return 1.0F;
  }
  const float t = 1.0F - dist / radius;  // 1 at center -> 0 at edge
  return t * t * (3.0F - 2.0F * t);      // smoothstep
}

auto TerrainBrush::brush_bbox(const HeightmapGrid& grid, float center_x,
                              float center_z, float radius,
                              std::uint32_t& out_x0, std::uint32_t& out_z0,
                              std::uint32_t& out_x1,
                              std::uint32_t& out_z1) noexcept -> bool {
  out_x0 = 0;
  out_z0 = 0;
  out_x1 = 0;
  out_z1 = 0;
  if (!valid_grid(grid) || !(radius > 0.0F)) {
    return false;
  }
  const float inv = 1.0F / grid.spacing;
  // Half-open sample range whose world pos is within radius. Use floor
  // for the low edge and ceil+1 for the high edge so every sample with
  // d < radius is covered; apply_* re-checks the exact distance, so an
  // over-wide box only costs work, never correctness.
  const float fx0 = (center_x - radius) * inv;
  const float fz0 = (center_z - radius) * inv;
  const float fx1 = (center_x + radius) * inv;
  const float fz1 = (center_z + radius) * inv;
  long x0 = static_cast<long>(std::floor(fx0));
  long z0 = static_cast<long>(std::floor(fz0));
  long x1 = static_cast<long>(std::ceil(fx1)) + 1;
  long z1 = static_cast<long>(std::ceil(fz1)) + 1;
  if (x0 < 0) {
    x0 = 0;
  }
  if (z0 < 0) {
    z0 = 0;
  }
  if (x1 > static_cast<long>(grid.width)) {
    x1 = static_cast<long>(grid.width);
  }
  if (z1 > static_cast<long>(grid.height)) {
    z1 = static_cast<long>(grid.height);
  }
  if (x1 <= x0 || z1 <= z0) {
    return false;  // brush misses the grid entirely
  }
  out_x0 = static_cast<std::uint32_t>(x0);
  out_z0 = static_cast<std::uint32_t>(z0);
  out_x1 = static_cast<std::uint32_t>(x1);
  out_z1 = static_cast<std::uint32_t>(z1);
  return true;
}

auto TerrainBrush::apply_brush(const HeightmapGrid& grid, float* samples,
                               const BrushParams& params,
                               float* scratch) noexcept -> bool {
  if (!valid_grid(grid) || samples == nullptr || !(params.radius > 0.0F) ||
      !(params.strength >= 0.0F)) {
    return false;
  }
  if (params.mode == BrushMode::kSmooth && scratch == nullptr) {
    return false;
  }
  std::uint32_t x0 = 0;
  std::uint32_t z0 = 0;
  std::uint32_t x1 = 0;
  std::uint32_t z1 = 0;
  if (!brush_bbox(grid, params.center_x, params.center_z, params.radius, x0,
                  z0, x1, z1)) {
    return true;  // valid no-op: brush misses the grid
  }
  // Smooth reads the pre-stroke neighborhood: snapshot the bbox first.
  // (scratch must be a width*height temp, NOT an alias of samples.)
  if (params.mode == BrushMode::kSmooth) {
    if (scratch == samples) {
      return false;
    }
    const std::size_t w = grid.width;
    for (std::uint32_t z = z0; z < z1; ++z) {
      std::memcpy(scratch + static_cast<std::size_t>(z) * w + x0,
                  samples + static_cast<std::size_t>(z) * w + x0,
                  static_cast<std::size_t>(x1 - x0) * sizeof(float));
    }
  }

  const float r = params.radius;
  const float r2 = r * r;
  const std::size_t w = grid.width;
  const std::uint32_t gw = grid.width;
  const std::uint32_t gh = grid.height;
  for (std::uint32_t z = z0; z < z1; ++z) {
    const float sz = sample_z(grid, z);
    for (std::uint32_t x = x0; x < x1; ++x) {
      const float sx = sample_x(grid, x);
      const float dx = sx - params.center_x;
      const float dz = sz - params.center_z;
      const float d2 = dx * dx + dz * dz;
      if (!(d2 < r2)) {
        continue;  // outside the radius: bit-exact no-touch
      }
      const float dist = std::sqrt(d2);
      const float f = falloff(dist, r);
      if (f <= 0.0F) {
        continue;
      }
      float& h = samples[static_cast<std::size_t>(z) * w + x];
      switch (params.mode) {
        case BrushMode::kRaise:
          h += params.strength * f;
          break;
        case BrushMode::kLower:
          h -= params.strength * f;
          break;
        case BrushMode::kSmooth: {
          // Box-blur average over the pre-stroke snapshot (clamped 3x3).
          float sum = 0.0F;
          float n = 0.0F;
          const std::uint32_t ax0 = (x > 0) ? x - 1 : x;
          const std::uint32_t az0 = (z > 0) ? z - 1 : z;
          const std::uint32_t ax1 = (x + 1 < gw) ? x + 2 : x + 1;
          const std::uint32_t az1 = (z + 1 < gh) ? z + 2 : z + 1;
          for (std::uint32_t az = az0; az < az1; ++az) {
            for (std::uint32_t ax = ax0; ax < ax1; ++ax) {
              sum += scratch[static_cast<std::size_t>(az) * w + ax];
              n += 1.0F;
            }
          }
          const float avg = (n > 0.0F) ? sum / n : h;
          const float t = params.strength > 1.0F ? 1.0F : params.strength;
          h += (avg - h) * (t * f);
          break;
        }
        case BrushMode::kFlatten: {
          const float t = params.strength > 1.0F ? 1.0F : params.strength;
          h += (params.target_height - h) * (t * f);
          break;
        }
      }
    }
  }
  return true;
}

auto TerrainBrush::paint_splat(const HeightmapGrid& grid,
                               SplatWeights* weights, float center_x,
                               float center_z, float radius, float strength,
                               std::uint32_t layer) noexcept -> bool {
  if (!valid_grid(grid) || weights == nullptr || !(radius > 0.0F) ||
      !(strength >= 0.0F) || layer >= 4) {
    return false;
  }
  std::uint32_t x0 = 0;
  std::uint32_t z0 = 0;
  std::uint32_t x1 = 0;
  std::uint32_t z1 = 0;
  if (!brush_bbox(grid, center_x, center_z, radius, x0, z0, x1, z1)) {
    return true;
  }
  const float r2 = radius * radius;
  const float t_full = strength > 1.0F ? 1.0F : strength;
  const std::size_t w = grid.width;
  for (std::uint32_t z = z0; z < z1; ++z) {
    const float sz = sample_z(grid, z);
    for (std::uint32_t x = x0; x < x1; ++x) {
      const float dx = sample_x(grid, x) - center_x;
      const float dz = sz - center_z;
      const float d2 = dx * dx + dz * dz;
      if (!(d2 < r2)) {
        continue;
      }
      const float f = falloff(std::sqrt(d2), radius) * t_full;
      if (f <= 0.0F) {
        continue;
      }
      SplatWeights& sw = weights[static_cast<std::size_t>(z) * w + x];
      for (std::uint32_t i = 0; i < 4; ++i) {
        const float target = (i == layer) ? 1.0F : 0.0F;
        sw.w[i] += (target - sw.w[i]) * f;
      }
      const float sum = sw.w[0] + sw.w[1] + sw.w[2] + sw.w[3];
      if (sum > 0.0F) {
        const float inv = 1.0F / sum;
        sw.w[0] *= inv;
        sw.w[1] *= inv;
        sw.w[2] *= inv;
        sw.w[3] *= inv;
      } else {
        sw.w[0] = 1.0F;
        sw.w[1] = 0.0F;
        sw.w[2] = 0.0F;
        sw.w[3] = 0.0F;
      }
    }
  }
  return true;
}

auto TerrainBrush::begin_stroke(StrokeJournal& journal,
                                const HeightmapGrid& grid,
                                const float* samples,
                                const SplatWeights* weights, float center_x,
                                float center_z, float radius,
                                bool capture_heights,
                                bool capture_splat) noexcept -> bool {
  if (journal.entries == nullptr || journal.max_entries == 0 ||
      journal.entry_capacity == 0) {
    return false;
  }
  if (!valid_grid(grid) || !(radius > 0.0F)) {
    return false;
  }
  if (capture_heights &&
      (samples == nullptr || journal.height_store == nullptr)) {
    return false;
  }
  if (capture_splat && (weights == nullptr || journal.splat_store == nullptr)) {
    return false;
  }
  if (!capture_heights && !capture_splat) {
    return false;
  }
  std::uint32_t x0 = 0;
  std::uint32_t z0 = 0;
  std::uint32_t x1 = 0;
  std::uint32_t z1 = 0;
  if (!brush_bbox(grid, center_x, center_z, radius, x0, z0, x1, z1)) {
    return false;  // nothing to snapshot
  }
  const std::uint32_t count = (x1 - x0) * (z1 - z0);
  if (count == 0 || count > journal.entry_capacity) {
    return false;
  }
  // Slot: next free, or evict oldest when full.
  std::uint32_t slot = 0;
  if (journal.count < journal.max_entries) {
    slot = (journal.head + journal.count) % journal.max_entries;
    ++journal.count;
  } else {
    slot = journal.head;
    journal.head = (journal.head + 1) % journal.max_entries;
  }
  StrokeEntry& e = journal.entries[slot];
  e.x0 = x0;
  e.z0 = z0;
  e.x1 = x1;
  e.z1 = z1;
  e.count = count;
  e.has_heights = capture_heights;
  e.has_splat = capture_splat;
  e.height_offset = slot * journal.entry_capacity;
  e.splat_offset = slot * journal.entry_capacity;
  const std::size_t w = grid.width;
  if (capture_heights) {
    float* dst =
        journal.height_store + static_cast<std::size_t>(e.height_offset);
    for (std::uint32_t z = z0; z < z1; ++z) {
      std::memcpy(dst,
                  samples + static_cast<std::size_t>(z) * w + x0,
                  static_cast<std::size_t>(x1 - x0) * sizeof(float));
      dst += (x1 - x0);
    }
  }
  if (capture_splat) {
    SplatWeights* dst =
        journal.splat_store + static_cast<std::size_t>(e.splat_offset);
    for (std::uint32_t z = z0; z < z1; ++z) {
      std::memcpy(dst, weights + static_cast<std::size_t>(z) * w + x0,
                  static_cast<std::size_t>(x1 - x0) * sizeof(SplatWeights));
      dst += (x1 - x0);
    }
  }
  return true;
}

auto TerrainBrush::undo_stroke(StrokeJournal& journal, float* samples,
                               SplatWeights* weights, std::uint32_t grid_width,
                               std::uint32_t grid_height) noexcept -> bool {
  if (journal.entries == nullptr || journal.count == 0) {
    return false;
  }
  const std::uint32_t slot =
      (journal.head + journal.count - 1) % journal.max_entries;
  const StrokeEntry& e = journal.entries[slot];
  if (e.x1 <= e.x0 || e.z1 <= e.z0 || e.x1 > grid_width ||
      e.z1 > grid_height || e.count != (e.x1 - e.x0) * (e.z1 - e.z0)) {
    return false;
  }
  if (e.has_heights) {
    if (samples == nullptr || journal.height_store == nullptr) {
      return false;
    }
    const float* src = journal.height_store +
                       static_cast<std::size_t>(e.height_offset);
    const std::size_t row = static_cast<std::size_t>(e.x1 - e.x0);
    for (std::uint32_t z = e.z0; z < e.z1; ++z) {
      std::memcpy(samples + static_cast<std::size_t>(z) * grid_width + e.x0,
                  src, row * sizeof(float));
      src += row;
    }
  }
  if (e.has_splat) {
    if (weights == nullptr || journal.splat_store == nullptr) {
      return false;
    }
    const SplatWeights* src = journal.splat_store +
                              static_cast<std::size_t>(e.splat_offset);
    const std::size_t row = static_cast<std::size_t>(e.x1 - e.x0);
    for (std::uint32_t z = e.z0; z < e.z1; ++z) {
      std::memcpy(weights + static_cast<std::size_t>(z) * grid_width + e.x0,
                  src, row * sizeof(SplatWeights));
      src += row;
    }
  }
  --journal.count;
  return true;
}

auto TerrainBrush::undo_count(const StrokeJournal& journal) noexcept
    -> std::uint32_t {
  if (journal.entries == nullptr) {
    return 0;
  }
  return journal.count;
}

}  // namespace engine::terrain

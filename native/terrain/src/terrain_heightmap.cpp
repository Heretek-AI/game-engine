// terrain_heightmap.cpp — P6-01: R16 PNG heightmap decode + sampling.
#include "engine/terrain/terrain_heightmap.hpp"

#include <cmath>

// stb_image reaches this TU via the engine_stb_image static lib headers
// (PUBLIC include from native/assets). Only the declaration is needed
// here; the implementation lives in the single stb_image_impl.c TU.
#if defined(ENGINE_TERRAIN_HAS_STB)
#include "stb_image.h"
#endif

namespace engine::terrain {

auto Heightmap::load_png(const std::uint8_t* data, std::size_t size,
                         float* out_samples, std::uint32_t out_capacity,
                         float spacing, float min_height, float max_height,
                         HeightmapGrid& out_grid,
                         int& channels_out) noexcept -> bool {
#if defined(ENGINE_TERRAIN_HAS_STB)
  if (data == nullptr || size == 0 || size > 0x7FFFFFFF ||
      out_samples == nullptr || spacing <= 0.0F || max_height < min_height) {
    return false;
  }
  int w = 0;
  int h = 0;
  int comp = 0;
  // 16-bit decode when present; falls back to 8-bit automatically.
  stbi_us* px16 = nullptr;
  std::uint8_t* px8 = nullptr;
  bool is16 = stbi_is_16_bit_from_memory(data, static_cast<int>(size)) != 0;
  if (is16) {
    px16 = stbi_load_16_from_memory(data, static_cast<int>(size), &w, &h,
                                    &comp, 1);
  } else {
    px8 = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &comp,
                                1);
  }
  if ((px16 == nullptr && px8 == nullptr) || w <= 0 || h <= 0) {
    if (px16 != nullptr) {
      stbi_image_free(px16);
    }
    if (px8 != nullptr) {
      stbi_image_free(px8);
    }
    return false;
  }
  const std::uint64_t need = static_cast<std::uint64_t>(w) *
                             static_cast<std::uint64_t>(h);
  if (need > out_capacity) {
    stbi_image_free(px16 != nullptr ? static_cast<void*>(px16)
                                    : static_cast<void*>(px8));
    return false;
  }
  const float range = max_height - min_height;
  for (std::uint64_t i = 0; i < need; ++i) {
    float t = 0.0F;
    if (px16 != nullptr) {
      t = static_cast<float>(px16[i]) / 65535.0F;
    } else {
      t = static_cast<float>(px8[i]) / 255.0F;
    }
    out_samples[i] = min_height + t * range;
  }
  stbi_image_free(px16 != nullptr ? static_cast<void*>(px16)
                                  : static_cast<void*>(px8));
  channels_out = comp;
  out_grid.samples = out_samples;
  out_grid.width = static_cast<std::uint32_t>(w);
  out_grid.height = static_cast<std::uint32_t>(h);
  out_grid.spacing = spacing;
  out_grid.min_height = min_height;
  out_grid.max_height = max_height;
  return true;
#else
  (void)data;
  (void)size;
  (void)out_samples;
  (void)out_capacity;
  (void)spacing;
  (void)min_height;
  (void)max_height;
  (void)out_grid;
  (void)channels_out;
  return false;
#endif
}

auto Heightmap::sample_height(const HeightmapGrid& grid, float x,
                              float z) noexcept -> float {
  if (grid.samples == nullptr || grid.width == 0 || grid.height == 0 ||
      grid.spacing <= 0.0F) {
    return 0.0F;
  }
  const float gx = x / grid.spacing;
  const float gz = z / grid.spacing;
  const float max_x = static_cast<float>(grid.width - 1);
  const float max_z = static_cast<float>(grid.height - 1);
  float cx = gx < 0.0F ? 0.0F : (gx > max_x ? max_x : gx);
  float cz = gz < 0.0F ? 0.0F : (gz > max_z ? max_z : gz);
  const auto x0 = static_cast<std::uint32_t>(cx);
  const auto z0 = static_cast<std::uint32_t>(cz);
  const std::uint32_t x1 = (x0 + 1 < grid.width) ? x0 + 1 : x0;
  const std::uint32_t z1 = (z0 + 1 < grid.height) ? z0 + 1 : z0;
  const float fx = cx - static_cast<float>(x0);
  const float fz = cz - static_cast<float>(z0);
  const float h00 =
      grid.samples[static_cast<std::size_t>(z0) * grid.width + x0];
  const float h10 =
      grid.samples[static_cast<std::size_t>(z0) * grid.width + x1];
  const float h01 =
      grid.samples[static_cast<std::size_t>(z1) * grid.width + x0];
  const float h11 =
      grid.samples[static_cast<std::size_t>(z1) * grid.width + x1];
  const float hx0 = h00 + (h10 - h00) * fx;
  const float hx1 = h01 + (h11 - h01) * fx;
  return hx0 + (hx1 - hx0) * fz;
}

auto Heightmap::sample_normal(const HeightmapGrid& grid, float x, float z,
                              float& nx, float& ny, float& nz) noexcept
    -> void {
  if (grid.samples == nullptr || grid.width == 0 || grid.height == 0 ||
      grid.spacing <= 0.0F) {
    nx = 0.0F;
    ny = 1.0F;
    nz = 0.0F;
    return;
  }
  const float e = grid.spacing;
  const float h_l = sample_height(grid, x - e, z);
  const float h_r = sample_height(grid, x + e, z);
  const float h_d = sample_height(grid, x, z - e);
  const float h_u = sample_height(grid, x, z + e);
  const float dhdx = (h_r - h_l) / (2.0F * e);
  const float dhdz = (h_u - h_d) / (2.0F * e);
  float inv = 1.0F / std::sqrt(dhdx * dhdx + 1.0F + dhdz * dhdz);
  nx = -dhdx * inv;
  ny = inv;
  nz = -dhdz * inv;
}

}  // namespace engine::terrain

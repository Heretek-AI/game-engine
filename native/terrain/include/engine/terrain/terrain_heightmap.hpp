// terrain_heightmap.hpp — P6-01: CPU heightmap asset loaded from R16 PNG.
//
// Owns the terrain elevation grid: `width x height` samples at `spacing`
// meters, `min_height..max_height` world range. Loaded from a 16-bit
// grayscale PNG via stb_image (`stbi_load_16_from_memory`, 1 channel);
// the raw sample s maps to min_height + (s/65535) * (max_height-min_height).
//
// Sampling is bilinear over the grid; `sample_height()` clamps to the
// border texel outside [0,(w-1)*spacing]x[0,(h-1)*spacing]. Normals come
// from central differences of `sample_height` (border: one-sided), with
// +Y up, right-handed: n = normalize(-dhdx, 1, -dhdz).
//
// Lifetime: init() copies the decoded grid into a caller-provided buffer
// (no malloc after init); destroy() releases nothing (caller frees).
// All entries noexcept; the decode itself may allocate transiently inside
// stb_image (load-time only, never the frame path).
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"

namespace engine::terrain {

// Decoded elevation grid: row-major, samples[y * width + x].
struct HeightmapGrid {
  const float* samples{nullptr};  // borrowed (caller-owned buffer)
  std::uint32_t width{0};
  std::uint32_t height{0};
  float spacing{1.0F};     // meters between adjacent samples (x and z)
  float min_height{0.0F};  // world height of sample value 0
  float max_height{10.0F};  // world height of sample value 65535
};

class ENGINE_CORE_API Heightmap {
 public:
  Heightmap() noexcept = default;
  Heightmap(const Heightmap&) = delete;
  auto operator=(const Heightmap&) -> Heightmap& = delete;
  ~Heightmap() noexcept = default;

  // Decode an R16 (or 8-bit grayscale) PNG from memory into `out_samples`
  // ([out_w x out_h] floats, caller-allocated, capacity >= w*h). Sets
  // `out_grid` to view the buffer with the given world mapping. Returns
  // false when the PNG is missing/malformed, not grayscale, or larger
  // than the caller buffer. `channels_out` receives the PNG channel
  // count (1 or 2 with alpha — alpha is dropped).
  [[nodiscard]] auto load_png(const std::uint8_t* data, std::size_t size,
                              float* out_samples, std::uint32_t out_capacity,
                              float spacing, float min_height,
                              float max_height, HeightmapGrid& out_grid,
                              int& channels_out) noexcept -> bool;

  // Bilinear elevation at world (x, z). Clamps to the border texel.
  [[nodiscard]] static auto sample_height(const HeightmapGrid& grid, float x,
                                          float z) noexcept -> float;

  // Surface normal at world (x, z) via central differences (+Y up).
  static auto sample_normal(const HeightmapGrid& grid, float x,
                            float z, float& nx, float& ny,
                            float& nz) noexcept -> void;
};

}  // namespace engine::terrain

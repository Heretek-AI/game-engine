// tilemap.hpp — P6-08: grid tilemap component, tile palette, merged collider.
//
// CPU-side only (always built, no GPU needed), mirroring the P6-06
// sprite_batch pattern:
//
//   TilemapGrid  { width, height, tile_size, origin[2] } — the grid frame.
//   TilePalette  tile id -> atlas UV cell + solid flag (editor paint source).
//   TileColliderRect { min[2], max[2] } — one merged solid region, world units.
//
// Merge (TilemapMergeSolids): greedy maximal-rectangle pass over a solid
// bitmask (row-major, 1 = solid). Row-wise maximal runs first, then vertical
// extension while the run span stays fully solid. Deterministic: output
// order is row-major by (first_row, first_col). Area is conserved
// (sum of rect areas == solid tile count). No malloc on the merge path
// beyond the caller's buffers; noexcept throughout.
//
// Raster (TilemapRasterize): deterministic RGBA8 paint of a tile id grid
// for the CPU golden — no GPU needed. Each tile fills tile_px square
// pixels with its palette color (checkerboard shade variant by (x+y)&1
// so grid alignment is visible); id 0 (empty) paints the background.
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"

namespace engine::renderer {

// Grid frame: width x height tiles, each tile_size world units, origin at
// the grid's (0,0) corner in world space. 24 bytes, align 4.
struct TilemapGrid {
  std::uint32_t width{0};
  std::uint32_t height{0};
  float tile_size{1.0F};
  float origin[2]{0.0F, 0.0F};
  std::uint32_t pad0{0};
};
static_assert(sizeof(TilemapGrid) == 24, "TilemapGrid must be 24 bytes");
static_assert(alignof(TilemapGrid) == 4, "TilemapGrid must be 4-byte aligned");

// One palette entry: which atlas UV cell a tile id draws from, plus the
// solid flag the collider merge reads. 20 bytes, align 4.
struct TilePaletteEntry {
  std::uint32_t atlas_id{0};
  float uv_rect[4]{0.0F, 0.0F, 1.0F, 1.0F};  // u0,v0,u1,v1 in atlas space
  std::uint32_t solid{0};                    // != 0 => collider merge treats as solid
};
static_assert(sizeof(TilePaletteEntry) == 24,
              "TilePaletteEntry must be 24 bytes");
static_assert(alignof(TilePaletteEntry) == 4,
              "TilePaletteEntry must be 4-byte aligned");

// One merged solid region in world units [min, max). 16 bytes, align 4.
struct TileColliderRect {
  float min_x{0.0F};
  float min_y{0.0F};
  float max_x{0.0F};
  float max_y{0.0F};
};
static_assert(sizeof(TileColliderRect) == 16,
              "TileColliderRect must be 16 bytes");
static_assert(alignof(TileColliderRect) == 4,
              "TileColliderRect must be 4-byte aligned");

// Merge solid tiles into maximal rectangles.
//
// solid: row-major width*height bytes (row y spans [y*width,(y+1)*width)),
//   nonzero = solid. grid: frame (only width/height/origin/tile_size used).
// out: capacity max_rects; returns rect count (0 on any null/empty input).
// When out is too small, returns the REQUIRED count and writes nothing
// (same small-buffer contract as SpritePlan).
ENGINE_CORE_API auto TilemapMergeSolids(const std::uint8_t* solid,
                                        const TilemapGrid& grid,
                                        TileColliderRect* out,
                                        std::size_t max_rects) noexcept
    -> std::size_t;

// Paint a tile-id grid into an RGBA8 buffer (deterministic, CPU only).
//
// tile_ids: row-major width*height uint32 ids (0 = empty -> background).
// colors: palette color per id (RGBA8 packed 0xRRGGBBAA), colors[id_count].
// out: width*tile_px by height*tile_px RGBA8 pixels.
// Returns false on any null/zero/bad-range input; true on success.
// Tile (x,y) paints the tile_px square at pixel origin
// (x*tile_px, (height-1-y)*tile_px) — row 0 (bottom) lands at the image
// bottom so the golden reads bottom-up like the world.
ENGINE_CORE_API auto TilemapRasterize(const std::uint32_t* tile_ids,
                                      std::uint32_t width, std::uint32_t height,
                                      const std::uint32_t* colors,
                                      std::uint32_t color_count,
                                      std::uint8_t* out_rgba8,
                                      std::uint32_t tile_px,
                                      std::uint32_t background) noexcept
    -> bool;

}  // namespace engine::renderer

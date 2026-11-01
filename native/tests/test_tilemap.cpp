// test_tilemap.cpp — P6-08 acceptance: grid tilemap + palette + merged collider.
//
// CPU raster golden (no GPU needed — mirrors the P6-06 CPU/GPU split where
// the CPU side carries the acceptance contract):
//   * 16x16 test map with two solid regions (a 6x4 block + an L of two
//     bars) merges to exactly 3 rects (block=1, L=2); area is conserved
//     (sum of rect areas == solid tile count * tile_size^2);
//   * rect bounds are exact in world units;
//   * null/empty/small-buffer contracts hold (small buffer reports required
//     count and writes nothing);
//   * TilemapRenderer2D == 32B, TilemapCollider2D == 16B, register_render()
//     registers Engine.Runtime.TilemapRenderer2D + TilemapCollider2D;
//   * CPU raster of the test map (16x16 tiles @ 16px = 256x256 RGBA8)
//     matches golden tilemap_16x256x256.
//
// Prints:
//   [PASS] tilemap: merged count 3
//   [PASS] golden: tilemap_16x256x256
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

#include "engine/ecs/ecs_render_components.hpp"
#include "engine/ecs/ecs_world.hpp"
#include "engine/renderer/tilemap.hpp"
#include "golden_compare.hpp"

namespace {

auto failures = 0;

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}

using engine::ecs::EcsWorld;
using engine::renderer::TileColliderRect;
using engine::renderer::TilemapGrid;
using engine::renderer::TilemapMergeSolids;
using engine::renderer::TilemapRasterize;

static_assert(sizeof(engine::renderer::TilemapGrid) == 24,
              "tilemap grid is 24 bytes");
static_assert(sizeof(engine::renderer::TilePaletteEntry) == 24,
              "tile palette entry is 24 bytes");
static_assert(sizeof(TileColliderRect) == 16, "tile collider rect is 16 bytes");
static_assert(sizeof(engine::ecs::TilemapRenderer2D) == 32,
              "TilemapRenderer2D is 32 bytes");
static_assert(sizeof(engine::ecs::TilemapCollider2D) == 16,
              "TilemapCollider2D is 16 bytes");

// Test map: 16x16, tile_size 1, origin (0,0).
//   Region A: 6x4 solid block at x[2..7], y[2..5]  -> 1 rect.
//   Region B: L-shape: bar x[10..13], y[2..9] + foot x[10..15], y[2..3].
//             Greedy row-major merge: row y=2 span x[10..15] extends down
//             only where x[10..13] stays solid -> rect1 x[10..15]x[2..3],
//             rect2 x[10..13]x[4..9]. L = 2 rects.
// Total: 3 merged regions; solid tiles = 24 + 12 + 24 = 60.
constexpr std::uint32_t kW = 16;
constexpr std::uint32_t kH = 16;
constexpr std::uint32_t kTilePx = 16;
constexpr std::uint32_t kPx = kW * kTilePx;  // 256

void fill_test_map(std::vector<std::uint8_t>& solid,
                   std::vector<std::uint32_t>& ids) {
  solid.assign(kW * kH, 0);
  ids.assign(kW * kH, 0);
  auto set = [&](std::uint32_t x, std::uint32_t y, std::uint32_t id) {
    solid[static_cast<std::size_t>(y) * kW + x] = (id != 0) ? 1 : 0;
    ids[static_cast<std::size_t>(y) * kW + x] = id;
  };
  for (std::uint32_t y = 2; y <= 5; ++y) {
    for (std::uint32_t x = 2; x <= 7; ++x) {
      set(x, y, 1);
    }
  }
  for (std::uint32_t y = 2; y <= 9; ++y) {
    for (std::uint32_t x = 10; x <= 13; ++x) {
      set(x, y, 2);
    }
  }
  for (std::uint32_t y = 2; y <= 3; ++y) {
    for (std::uint32_t x = 14; x <= 15; ++x) {
      set(x, y, 2);
    }
  }
}

void test_merge_cpu() {
  TilemapGrid grid{};
  grid.width = kW;
  grid.height = kH;
  grid.tile_size = 1.0F;

  std::vector<std::uint8_t> solid;
  std::vector<std::uint32_t> ids;
  fill_test_map(solid, ids);

  std::vector<TileColliderRect> rects(16);
  const std::size_t n =
      TilemapMergeSolids(solid.data(), grid, rects.data(), rects.size());
  check(n == 3, "tilemap: merged count 3");

  // Area conserved: 24 + 12 + 24 = 60 tiles at 1x1.
  float area = 0.0F;
  for (std::size_t i = 0; i < n && i < rects.size(); ++i) {
    area += (rects[i].max_x - rects[i].min_x) *
            (rects[i].max_y - rects[i].min_y);
  }
  check(area == 60.0F, "tilemap: merged area 60");

  // Exact bounds, row-major order: block first, then L rows.
  bool bounds = n == 3;
  if (bounds) {
    bounds &= rects[0].min_x == 2.0F && rects[0].min_y == 2.0F &&
              rects[0].max_x == 8.0F && rects[0].max_y == 6.0F;
    bounds &= rects[1].min_x == 10.0F && rects[1].min_y == 2.0F &&
              rects[1].max_x == 16.0F && rects[1].max_y == 4.0F;
    bounds &= rects[2].min_x == 10.0F && rects[2].min_y == 4.0F &&
              rects[2].max_x == 14.0F && rects[2].max_y == 10.0F;
  }
  check(bounds, "tilemap: rect bounds exact");

  // Null/empty contracts.
  check(TilemapMergeSolids(nullptr, grid, rects.data(), rects.size()) == 0,
        "tilemap: null solid rejected");
  TilemapGrid empty{};
  check(TilemapMergeSolids(solid.data(), empty, rects.data(), rects.size()) ==
            0,
        "tilemap: empty grid rejected");
  // Small buffer: reports required count, writes nothing.
  std::vector<TileColliderRect> tiny(2);
  tiny[0].min_x = 999.0F;
  const std::size_t need =
      TilemapMergeSolids(solid.data(), grid, tiny.data(), tiny.size());
  check(need == 3 && tiny[0].min_x == 0.0F,
        "tilemap: small buffer reports 3, writes nothing");
  check(TilemapMergeSolids(solid.data(), grid, nullptr, 0) == 3,
        "tilemap: count-only query is 3");

  // Origin + tile_size shift world bounds.
  TilemapGrid shifted = grid;
  shifted.origin[0] = 10.0F;
  shifted.origin[1] = -5.0F;
  shifted.tile_size = 2.0F;
  std::vector<TileColliderRect> rs(16);
  const std::size_t ns =
      TilemapMergeSolids(solid.data(), shifted, rs.data(), rs.size());
  check(ns == 3 && rs[0].min_x == 14.0F && rs[0].min_y == -1.0F &&
            rs[0].max_x == 26.0F && rs[0].max_y == 7.0F,
        "tilemap: origin/tile_size shift exact");

  // ECS registration: native component sizes the managed mirrors depend on.
  EcsWorld world;
  check(world.construct(), "tilemap: world construct ok");
  if (world.valid()) {
    check(world.register_render(), "tilemap: register_render incl tilemap ok");
  }
  world.destroy();
}

void test_raster_golden() {
  std::vector<std::uint8_t> solid;
  std::vector<std::uint32_t> ids;
  fill_test_map(solid, ids);
  // Palette colors (packed 0xRRGGBBAA): bg dark, id1 orange, id2 teal.
  const std::uint32_t colors[3] = {0x00000000, 0xE08A20FF, 0x208A80FF};
  std::vector<std::uint8_t> px(static_cast<std::size_t>(kPx) * kPx * 4);
  const bool ok =
      TilemapRasterize(ids.data(), kW, kH, colors, 3, px.data(), kTilePx,
                       0x14181FFF);
  check(ok, "tilemap: raster ok");
  if (!ok) {
    ++failures;
    std::printf("[FAIL] golden: tilemap_16x256x256 raster\n");
    return;
  }
  // Null/empty gates.
  check(!TilemapRasterize(nullptr, kW, kH, colors, 3, px.data(), kTilePx,
                          0x14181FFF),
        "tilemap: null ids rejected");
  check(!TilemapRasterize(ids.data(), 0, kH, colors, 3, px.data(), kTilePx,
                          0x14181FFF),
        "tilemap: zero width rejected");
  // 65536 * 65536 px wraps 32 bits: must be rejected before any write.
  check(!TilemapRasterize(ids.data(), 65536U, kH, colors, 3, px.data(), 65536U,
                          0x14181FFF),
        "tilemap: pixel-extent overflow rejected");
  failures += engine::test::golden_check(
      "tilemap_16x256x256",
      std::span<const std::uint8_t>(px.data(), px.size()), kPx, kPx);
}

}  // namespace

auto main() -> int {
  setvbuf(stdout, nullptr, _IONBF, 0);
  test_merge_cpu();
  test_raster_golden();
  std::printf(failures == 0 ? "TILEMAP_OK\n" : "TILEMAP_FAILURES=%d\n",
              failures);
  return failures == 0 ? 0 : 1;
}

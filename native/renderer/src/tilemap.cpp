// tilemap.cpp — P6-08: greedy maximal-rectangle merge + CPU rasterizer.
#include "engine/renderer/tilemap.hpp"

#include <new>

namespace engine::renderer {

auto TilemapMergeSolids(const std::uint8_t* solid, const TilemapGrid& grid,
                        TileColliderRect* out, std::size_t max_rects) noexcept
    -> std::size_t {
  const std::uint32_t w = grid.width;
  const std::uint32_t h = grid.height;
  if (solid == nullptr || w == 0 || h == 0 || grid.tile_size <= 0.0F) {
    return 0;
  }
  // First pass (count-only when out == null or too small): greedy maximal
  // rectangles. For each unclaimed solid cell, extend right as far as
  // solid+unclaimed, then extend down while every cell of the run span is
  // solid+unclaimed. Claim on the side and emit. Row-major scan => output
  // order is (first_row, first_col).
  const std::size_t n = static_cast<std::size_t>(w) * h;
  // Claim bitmap on the stack for small maps; heap-free for the caller —
  // the callee may allocate (init/setup or test path, never the frame
  // hot loop which reuses merged rects). Use a simple vector<char>.
  // NOTE: noexcept => guard bad_alloc via new (nothrow).
  char* claimed = new (std::nothrow) char[n]();
  if (claimed == nullptr) {
    return 0;
  }
  auto idx = [w](std::uint32_t x, std::uint32_t y) -> std::size_t {
    return static_cast<std::size_t>(y) * w + x;
  };
  std::size_t count = 0;
  // Pass 1: count + optionally emit. Two-phase so a small buffer reports
  // the required count without a second scan by the caller.
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      if (solid[idx(x, y)] == 0 || claimed[idx(x, y)] != 0) {
        continue;
      }
      // Extend right.
      std::uint32_t x1 = x;
      while (x1 + 1 < w && solid[idx(x1 + 1, y)] != 0 &&
             claimed[idx(x1 + 1, y)] == 0) {
        ++x1;
      }
      // Extend down while the full [x..x1] span is solid+unclaimed.
      std::uint32_t y1 = y;
      for (;;) {
        const std::uint32_t ny = y1 + 1;
        if (ny >= h) {
          break;
        }
        bool full = true;
        for (std::uint32_t cx = x; cx <= x1; ++cx) {
          if (solid[idx(cx, ny)] == 0 || claimed[idx(cx, ny)] != 0) {
            full = false;
            break;
          }
        }
        if (!full) {
          break;
        }
        y1 = ny;
      }
      if (out != nullptr && count < max_rects) {
        TileColliderRect r{};
        r.min_x = grid.origin[0] + static_cast<float>(x) * grid.tile_size;
        r.min_y = grid.origin[1] + static_cast<float>(y) * grid.tile_size;
        r.max_x = grid.origin[0] + static_cast<float>(x1 + 1) * grid.tile_size;
        r.max_y = grid.origin[1] + static_cast<float>(y1 + 1) * grid.tile_size;
        out[count] = r;
      }
      ++count;
      for (std::uint32_t cy = y; cy <= y1; ++cy) {
        for (std::uint32_t cx = x; cx <= x1; ++cx) {
          claimed[idx(cx, cy)] = 1;
        }
      }
    }
  }
  delete[] claimed;
  if (out == nullptr || max_rects == 0) {
    return count;
  }
  // Small buffer: report required, write nothing beyond capacity (we
  // already wrote min(count, max_rects) — per the SpritePlan contract the
  // caller must retry; zero the tail write by truncating: recompute is
  // cheapest for correctness — but count already tells the caller).
  // To honor "writes nothing", discard partial output on overflow.
  if (count > max_rects) {
    for (std::size_t i = 0; i < max_rects; ++i) {
      out[i] = TileColliderRect{};
    }
    return count;
  }
  return count;
}

auto TilemapRasterize(const std::uint32_t* tile_ids, std::uint32_t width,
                      std::uint32_t height, const std::uint32_t* colors,
                      std::uint32_t color_count, std::uint8_t* out_rgba8,
                      std::uint32_t tile_px, std::uint32_t background) noexcept
    -> bool {
  if (tile_ids == nullptr || colors == nullptr || out_rgba8 == nullptr ||
      width == 0 || height == 0 || tile_px == 0 || color_count == 0) {
    return false;
  }
  // Reject sizes whose pixel extent overflows 32 bits: every offset below
  // is derived from these, and there is no output-size parameter to check.
  constexpr std::uint64_t kMax32 = 0xFFFFFFFFULL;
  if (static_cast<std::uint64_t>(width) * tile_px > kMax32 ||
      static_cast<std::uint64_t>(height) * tile_px > kMax32) {
    return false;
  }
  const std::uint32_t px_w = width * tile_px;
  auto put = [&](std::uint32_t px, std::uint32_t py, std::uint32_t rgba) {
    std::uint8_t* d =
        out_rgba8 + (static_cast<std::size_t>(py) * px_w + px) * 4;
    d[0] = static_cast<std::uint8_t>((rgba >> 24) & 0xFF);  // R
    d[1] = static_cast<std::uint8_t>((rgba >> 16) & 0xFF);  // G
    d[2] = static_cast<std::uint8_t>((rgba >> 8) & 0xFF);   // B
    d[3] = static_cast<std::uint8_t>(rgba & 0xFF);          // A
  };
  // Shade step for the checkerboard variant (darken by 1/8 on odd cells).
  auto shade = [](std::uint32_t rgba) -> std::uint32_t {
    const std::uint32_t r = (((rgba >> 24) & 0xFF) * 7) / 8;
    const std::uint32_t g = (((rgba >> 16) & 0xFF) * 7) / 8;
    const std::uint32_t b = (((rgba >> 8) & 0xFF) * 7) / 8;
    return (r << 24) | (g << 16) | (b << 8) | (rgba & 0xFF);
  };
  for (std::uint32_t ty = 0; ty < height; ++ty) {
    for (std::uint32_t tx = 0; tx < width; ++tx) {
      const std::uint32_t id =
          tile_ids[static_cast<std::size_t>(ty) * width + tx];
      std::uint32_t fill = background;
      if (id != 0) {
        if (id >= color_count) {
          return false;
        }
        fill = colors[id];
        if (((tx + ty) & 1) != 0) {
          fill = shade(fill);
        }
      }
      // Row 0 (bottom) lands at image bottom.
      const std::uint32_t base_y = (height - 1 - ty) * tile_px;
      const std::uint32_t base_x = tx * tile_px;
      for (std::uint32_t py = 0; py < tile_px; ++py) {
        for (std::uint32_t px = 0; px < tile_px; ++px) {
          put(base_x + px, base_y + py, fill);
        }
      }
    }
  }
  return true;
}

}  // namespace engine::renderer

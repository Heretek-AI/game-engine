// sprite_batch.hpp — P6-06: batched 2D sprite renderer with atlases.
//
// CPU-side batch planner (always built, no GPU needed): sprites are
// submitted per frame, sorted by (sorting_layer, sort_order), grouped
// into one draw call per contiguous atlas run. The acceptance contract
// is: N sprites sharing one atlas emit exactly 1 draw call; sprites
// spread over K atlases emit exactly K draw calls (one per atlas).
//
// Per-sprite record (submitted by the caller, typically one per Flecs
// SpriteRenderer entity):
//   SpriteItem { atlas_id, sorting_layer, sort_order, pos[2], size[2],
//                uv_rect[4] (u0,v0,u1,v1 into the atlas), color[4] }
//
// Batch output:
//   SpriteBatch { atlas_id, first_sprite, sprite_count } — a contiguous
//   range into the SORTED sprite array (sort into the caller's buffer
//   in place). plan() sorts + emits batches; draw_calls() == batches.
//
// Vertex layout emitted per sprite (quad, 4 verts / 6 indices):
//   SpriteVertex { pos[2], uv[2], color[4] } = 32 bytes.
//
// Orthographic camera (same header — P6-06 ships both):
//   OrthoCamera2D { height_world, aspect, pos[2], pixel_perfect,
//                   pixels_per_unit, viewport_px[2] }
//   OrthoViewProj() builds the column-major view-projection matrix
//   (Vulkan clip: Y flipped, depth 0..1). With pixel_perfect != 0 one
//   world unit covers pixels_per_unit screen pixels (visible height =
//   viewport_px[1] / pixels_per_unit, height_world ignored) and the camera
//   position rounds to the 1/ppu grid, so texels land on screen pixels.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"

namespace engine::renderer {

// Submitted sprite: one quad instance.
// 60 bytes, align 4 (plain data, no frame-path allocation).
struct SpriteItem {
  std::uint32_t atlas_id{0};
  std::int32_t sorting_layer{0};
  std::int32_t sort_order{0};
  float pos[2]{0.0F, 0.0F};   // world-space center
  float size[2]{1.0F, 1.0F};  // world-space extents
  float uv_rect[4]{0.0F, 0.0F, 1.0F, 1.0F};  // u0,v0,u1,v1 in atlas space
  float color[4]{1.0F, 1.0F, 1.0F, 1.0F};
};
static_assert(sizeof(SpriteItem) == 60, "SpriteItem must be 60 bytes");
static_assert(alignof(SpriteItem) == 4, "SpriteItem must be 4-byte aligned");

// One draw call: contiguous range of the sorted sprite array sharing
// one atlas (16 bytes, align 4).
struct SpriteBatch {
  std::uint32_t atlas_id{0};
  std::uint32_t first_sprite{0};
  std::uint32_t sprite_count{0};
  std::uint32_t pad0{0};
};
static_assert(sizeof(SpriteBatch) == 16, "SpriteBatch must be 16 bytes");

// Interleaved quad vertex: pos[2], uv[2], color[4] = 8 floats = 32B.
struct SpriteVertex {
  float pos[2]{0.0F, 0.0F};
  float uv[2]{0.0F, 0.0F};
  float color[4]{1.0F, 1.0F, 1.0F, 1.0F};
};
static_assert(sizeof(SpriteVertex) == 32, "SpriteVertex must be 32 bytes");

// Sort key: (sorting_layer, sort_order, atlas_id). Atlas is the LAST
// key so draw order is never broken to save draw calls — batching only
// merges contiguous same-atlas runs (one draw call per run).
inline auto SpriteLess(const SpriteItem& a, const SpriteItem& b) noexcept
    -> bool {
  if (a.sorting_layer != b.sorting_layer) {
    return a.sorting_layer < b.sorting_layer;
  }
  if (a.sort_order != b.sort_order) {
    return a.sort_order < b.sort_order;
  }
  return a.atlas_id < b.atlas_id;
}

// Stable sort + batch plan. Sorts `sprites[0..count)` in place, writes
// up to `batch_cap` batches into `batches_out`, returns batch count
// (or 0 on null/empty input). When the batch buffer is too small,
// returns the required count without writing (caller retries bigger).
// Null sprites with count > 0, or null batches_out with cap > 0,
// return 0.
inline auto SpritePlan(SpriteItem* sprites, std::size_t count,
                       SpriteBatch* batches_out,
                       std::size_t batch_cap) noexcept -> std::size_t {
  if (sprites == nullptr || count == 0) {
    return 0;
  }
  std::stable_sort(sprites, sprites + count, SpriteLess);
  // Count runs first (needed for the too-small early-out).
  std::size_t runs = 1;
  for (std::size_t i = 1; i < count; ++i) {
    if (sprites[i].atlas_id != sprites[i - 1].atlas_id) {
      ++runs;
    }
  }
  if (batches_out == nullptr || batch_cap < runs) {
    return runs;
  }
  std::size_t b = 0;
  std::size_t start = 0;
  for (std::size_t i = 1; i <= count; ++i) {
    if (i == count || sprites[i].atlas_id != sprites[i - 1].atlas_id) {
      SpriteBatch batch{};
      batch.atlas_id = sprites[start].atlas_id;
      batch.first_sprite = static_cast<std::uint32_t>(start);
      batch.sprite_count = static_cast<std::uint32_t>(i - start);
      batches_out[b++] = batch;
      start = i;
    }
  }
  return b;
}

// Emit 4 interleaved verts + 6 indices for sprites[first..first+n).
// `vert_base`/`index_base` are starting offsets; index_out receives
// absolute (base-added) indices. Returns false on null/out-of-range.
inline auto SpriteEmitQuads(const SpriteItem* sprites, std::size_t count,
                            std::uint32_t first, std::uint32_t n,
                            SpriteVertex* vert_out, std::uint32_t vert_base,
                            std::uint32_t* index_out,
                            std::uint32_t index_base) noexcept -> bool {
  if (sprites == nullptr || vert_out == nullptr || index_out == nullptr) {
    return false;
  }
  if (static_cast<std::size_t>(first) + n > count) {
    return false;
  }
  for (std::uint32_t k = 0; k < n; ++k) {
    const SpriteItem& s = sprites[static_cast<std::size_t>(first) + k];
    const float hx = s.size[0] * 0.5F;
    const float hy = s.size[1] * 0.5F;
    const float u0 = s.uv_rect[0];
    const float v0 = s.uv_rect[1];
    const float u1 = s.uv_rect[2];
    const float v1 = s.uv_rect[3];
    SpriteVertex* v = vert_out + (static_cast<std::size_t>(vert_base) + k * 4);
    // BL, BR, TR, TL (CCW front face).
    v[0].pos[0] = s.pos[0] - hx;
    v[0].pos[1] = s.pos[1] - hy;
    v[0].uv[0] = u0;
    v[0].uv[1] = v0;
    v[1].pos[0] = s.pos[0] + hx;
    v[1].pos[1] = s.pos[1] - hy;
    v[1].uv[0] = u1;
    v[1].uv[1] = v0;
    v[2].pos[0] = s.pos[0] + hx;
    v[2].pos[1] = s.pos[1] + hy;
    v[2].uv[0] = u1;
    v[2].uv[1] = v1;
    v[3].pos[0] = s.pos[0] - hx;
    v[3].pos[1] = s.pos[1] + hy;
    v[3].uv[0] = u0;
    v[3].uv[1] = v1;
    for (int c = 0; c < 4; ++c) {
      for (int j = 0; j < 4; ++j) {
        v[j].color[c] = s.color[c];
      }
    }
    std::uint32_t* ix =
        index_out + (static_cast<std::size_t>(index_base) + k * 6);
    const std::uint32_t b = vert_base + k * 4;
    ix[0] = b;
    ix[1] = b + 1;
    ix[2] = b + 2;
    ix[3] = b;
    ix[4] = b + 2;
    ix[5] = b + 3;
  }
  return true;
}

// 2D orthographic camera (40 bytes, align 4).
struct OrthoCamera2D {
  float height_world{10.0F};  // vertical extent in world units (free camera)
  float aspect{16.0F / 9.0F};
  float pos[2]{0.0F, 0.0F};  // camera center in world units
  std::uint32_t pixel_perfect{0};  // nonzero: height from viewport/ppu, snapped
  float pixels_per_unit{100.0F};
  float viewport_px[2]{640.0F, 360.0F};
  float near_z{-10.0F};
  float far_z{10.0F};
};
static_assert(sizeof(OrthoCamera2D) == 40, "OrthoCamera2D must be 40 bytes");
static_assert(alignof(OrthoCamera2D) == 4, "OrthoCamera2D must be 4-aligned");

// Column-major Vulkan-clip ortho view-projection (Y flipped, depth
// 0..1). With pixel_perfect, the visible height is viewport_px[1] /
// pixels_per_unit and the camera center rounds to the 1/ppu grid, so
// texel edges land exactly on screen pixels.
inline auto OrthoViewProj(const OrthoCamera2D& cam,
                          float out_vp[16]) noexcept -> bool {
  if (out_vp == nullptr) {
    return false;
  }
  if (!(cam.height_world > 0.0F) || !(cam.aspect > 0.0F) ||
      !(cam.pixels_per_unit > 0.0F) || !(cam.viewport_px[0] > 0.0F) ||
      !(cam.viewport_px[1] > 0.0F)) {
    return false;
  }
  if (!(cam.far_z > cam.near_z)) {
    return false;
  }
  float cx = cam.pos[0];
  float cy = cam.pos[1];
  float height = cam.height_world;
  if (cam.pixel_perfect != 0) {
    height = cam.viewport_px[1] / cam.pixels_per_unit;
    // Round to the nearest texel (double: no int overflow, no 2-px
    // bucket at the origin).
    const double ppu = static_cast<double>(cam.pixels_per_unit);
    cx = static_cast<float>(std::round(static_cast<double>(cx) * ppu) / ppu);
    cy = static_cast<float>(std::round(static_cast<double>(cy) * ppu) / ppu);
  }
  const float hw = height * 0.5F;
  const float hh = hw * cam.aspect;
  const float l = cx - hh;
  const float r = cx + hh;
  const float b = cy - hw;
  const float t = cy + hw;
  const float n = cam.near_z;
  const float f = cam.far_z;
  // Vulkan clip: x in [l,r] -> [-1,1], y in [b,t] -> [1,-1] (flipped),
  // z in [n,f] -> [0,1].
  for (int i = 0; i < 16; ++i) {
    out_vp[i] = 0.0F;
  }
  out_vp[0] = 2.0F / (r - l);
  out_vp[5] = -2.0F / (t - b);
  out_vp[10] = 1.0F / (f - n);
  out_vp[12] = -(r + l) / (r - l);
  out_vp[13] = (t + b) / (t - b);
  out_vp[14] = -n / (f - n);
  out_vp[15] = 1.0F;
  return true;
}

}  // namespace engine::renderer

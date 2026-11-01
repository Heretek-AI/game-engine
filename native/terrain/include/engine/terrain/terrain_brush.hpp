// terrain_brush.hpp — P6-02: terrain editing brushes + stroke undo.
//
// CPU-side height/splat painting over the P6-01 HeightmapGrid contract.
// All functions are pure grid math (no malloc, noexcept): the caller owns
// every buffer, including the undo journal.
//
// Height brushes (operate on a mutable float grid, same layout as
// HeightmapGrid::samples — row-major, samples[y*width+x]):
//   raise   — h += strength * falloff(d/radius)      (strength > 0)
//   lower   — h -= strength * falloff(d/radius)      (strength > 0)
//   smooth  — h = lerp(h, neighborhood_avg, strength) (box blur of the
//             pre-stroke grid inside the radius; strength 0..1)
//   flatten — h = lerp(h, target_height, strength * falloff)
//
// Falloff is smoothstep(1 - d/radius) so the brush reaches exactly 0 at
// d == radius: samples at distance >= radius are never touched (the
// acceptance test asserts this bit-exactly). Radius is in world units;
// sample positions are (x*spacing, z*spacing), brush center in world xz.
//
// Splat painting (per-vertex 4-layer weights, SplatWeights layout from
// terrain_splat_material.hpp): paint_splat lerps weights[i] toward the
// target layer mask (1 at `layer`, 0 elsewhere) by strength*falloff, then
// renormalizes to sum 1. Stored per-vertex in a caller grid parallel to
// the height grid (weights[y*width+x]).
//
// Undo: every stroke is bracketed by begin_stroke()/end_stroke() on a
// StrokeJournal. begin_stroke snapshots the brush bounding box (before
// image); end_stroke truncates the journal to the committed region.
// undo_stroke() restores the newest entry bit-exactly (memcpy) and pops
// it. Journal storage is caller-provided (fixed capacity, ring-evicted
// oldest when full). One entry covers both heights and splat weights so
// a mixed stroke undoes atomically.
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"

namespace engine::terrain {

struct HeightmapGrid;
struct SplatWeights;

// Brush operation selector (matches the editor UI enum 1:1).
enum class BrushMode : std::uint8_t {
  kRaise = 0,
  kLower = 1,
  kSmooth = 2,
  kFlatten = 3,
};

// Brush parameters for one dab (single stamp at center_xz).
struct BrushParams {
  BrushMode mode{BrushMode::kRaise};
  float center_x{0.0F};    // world-space brush center
  float center_z{0.0F};
  float radius{1.0F};      // world units, must be > 0
  float strength{0.25F};   // raise/lower: height delta at center;
                           // smooth/flatten/splat: 0..1 blend factor
  float target_height{0.0F};  // flatten target (world Y)
};

// One undo entry: bounding box + before-images for heights and splat.
// Storage is borrowed from the journal's caller buffers.
struct StrokeEntry {
  std::uint32_t x0{0};
  std::uint32_t z0{0};
  std::uint32_t x1{0};  // exclusive
  std::uint32_t z1{0};  // exclusive
  std::uint32_t height_offset{0};  // float offset into journal height store
  std::uint32_t splat_offset{0};   // SplatWeights offset into splat store
  std::uint32_t count{0};          // samples covered ((x1-x0)*(z1-z0))
  bool has_heights{false};
  bool has_splat{false};
};

// Fixed-capacity stroke journal. Backing stores are caller-owned:
//   height_store — capacity entry_capacity * max_entries floats
//   splat_store  — capacity entry_capacity * max_entries SplatWeights
//   entries      — capacity max_entries StrokeEntry
// entry_capacity must cover the largest brush bbox the caller will stamp
// (caller can query brush_bbox() first and size accordingly, or clamp the
// radius). When full, the oldest entry is evicted (ring).
struct StrokeJournal {
  StrokeEntry* entries{nullptr};
  float* height_store{nullptr};
  SplatWeights* splat_store{nullptr};
  std::uint32_t max_entries{0};
  std::uint32_t entry_capacity{0};  // samples per entry
  std::uint32_t count{0};           // live entries
  std::uint32_t head{0};            // ring start (oldest)
};

class ENGINE_CORE_API TerrainBrush {
 public:
  // Smoothstep falloff: 0 at d >= radius, 1 at center. Pure function for
  // tests and the managed mirror.
  [[nodiscard]] static auto falloff(float dist, float radius) noexcept
      -> float;

  // Bounding box of samples touched by a dab, clamped to the grid.
  // Returns false on null grid/samples or radius <= 0 (out untouched).
  static auto brush_bbox(const HeightmapGrid& grid, float center_x,
                         float center_z, float radius, std::uint32_t& out_x0,
                         std::uint32_t& out_z0, std::uint32_t& out_x1,
                         std::uint32_t& out_z1) noexcept -> bool;

  // Stamp one height dab. `samples` must be the grid's mutable buffer
  // (width*height floats). `scratch` (same size, caller-owned) is used
  // only by kSmooth as the pre-stroke read source — pass nullptr for
  // other modes (for kSmooth it must alias a snapshot of `samples`
  // taken before the dab; the helper snapshots internally when
  // scratch == samples is NOT allowed — caller passes a temp).
  // Returns false on bad args (grid untouched).
  static auto apply_brush(const HeightmapGrid& grid, float* samples,
                          const BrushParams& params,
                          float* scratch) noexcept -> bool;

  // Stamp one splat dab over a per-vertex weight grid parallel to the
  // height grid. `weights` has width*height SplatWeights. Paints toward
  // `layer` (0..3). Returns false on bad args (grid untouched).
  static auto paint_splat(const HeightmapGrid& grid, SplatWeights* weights,
                          float center_x, float center_z, float radius,
                          float strength, std::uint32_t layer) noexcept
      -> bool;

  // Snapshot the brush bbox into the journal (call before apply_brush /
  // paint_splat). Captures heights when capture_heights, splat when
  // capture_splat. Returns false when the journal is null/full-overflow
  // (bbox larger than entry_capacity) or args bad.
  static auto begin_stroke(StrokeJournal& journal, const HeightmapGrid& grid,
                           const float* samples, const SplatWeights* weights,
                           float center_x, float center_z, float radius,
                           bool capture_heights,
                           bool capture_splat) noexcept -> bool;

  // Restore the newest entry bit-exactly into samples/weights and pop
  // it. Either buffer may be nullptr when the entry has no data of that
  // kind. Returns false when empty or args mismatch (nothing restored).
  static auto undo_stroke(StrokeJournal& journal, float* samples,
                          SplatWeights* weights, std::uint32_t grid_width,
                          std::uint32_t grid_height) noexcept -> bool;

  // Live entry count (0 when journal null).
  [[nodiscard]] static auto undo_count(const StrokeJournal& journal) noexcept
      -> std::uint32_t;
};

}  // namespace engine::terrain

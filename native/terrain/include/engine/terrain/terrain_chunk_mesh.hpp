// terrain_chunk_mesh.hpp — P6-01: chunked terrain mesh with LOD.
//
// The heightmap grid is split into `chunks_x x chunks_z` tiles, each
// `chunk_samples x chunk_samples` vertices (shared border samples: chunk
// (cx,cz) covers samples [cx*(cs-1), cx*(cs-1)+cs)). Every chunk builds
// a full-resolution index buffer plus one decimated LOD index buffer per
// level (vertex step 2^level, minimum 2x2 verts). LOD is a pure index
// decimation: vertices are shared across levels, so switching LOD swaps
// the index range, never the vertex buffer.
//
// `build_chunk()` writes positions (x = sample*spacing, y = height,
// z = sample*spacing), normals (from Heightmap::sample_normal), and uvs
// into caller buffers; `lod_index_count()` reports the index count per
// level. `select_lod()` picks the level by camera distance with
// hysteresis-free thresholds (P6-02 editor tools reuse the chunk grid).
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"

namespace engine::terrain {

// One terrain vertex: position + normal + uv. 32 bytes, 4-byte aligned.
struct TerrainVertex {
  float px{0.0F};
  float py{0.0F};
  float pz{0.0F};
  float nx{0.0F};
  float ny{1.0F};
  float nz{0.0F};
  float u{0.0F};
  float v{0.0F};
};

static_assert(sizeof(TerrainVertex) == 32, "TerrainVertex must be 32 bytes");

struct HeightmapGrid;

class ENGINE_CORE_API ChunkMesh {
 public:
  // Max LOD levels stored per chunk (level 0 = full res).
  static constexpr std::uint32_t kMaxLodLevels = 4;

  // Index count for one chunk at `lod` (vertex step 2^lod). Zero when the
  // chunk is degenerate (chunk_samples < 2) or lod >= kMaxLodLevels.
  [[nodiscard]] static auto lod_index_count(std::uint32_t chunk_samples,
                                            std::uint32_t lod) noexcept
      -> std::uint32_t;

  // Total index capacity for all LOD levels of one chunk.
  [[nodiscard]] static auto lod_total_indices(std::uint32_t chunk_samples) noexcept
      -> std::uint32_t;

  // Build one chunk: fills `out_verts` (chunk_samples^2) and `out_indices`
  // (all LOD levels back-to-back, level 0 first). `lod_offsets[l]` receives
  // the start index of level l (levels with 0 tris repeat the previous
  // offset). World origin: chunk (cx,cz) starts at sample
  // (cx*(cs-1), cz*(cs-1)). Returns false on null/out-of-range args.
  [[nodiscard]] static auto build_chunk(const HeightmapGrid& grid,
                                        std::uint32_t chunk_x,
                                        std::uint32_t chunk_z,
                                        std::uint32_t chunk_samples,
                                        TerrainVertex* out_verts,
                                        std::uint32_t* out_indices,
                                        std::uint32_t* lod_offsets) noexcept
      -> bool;

  // LOD level for a camera distance: level = first l with
  // dist < lod_distances[l], else kMaxLodLevels-1. Distances must be
  // ascending; null/empty distances => level 0.
  [[nodiscard]] static auto select_lod(float distance,
                                       const float* lod_distances,
                                       std::uint32_t lod_count) noexcept
      -> std::uint32_t;
};

}  // namespace engine::terrain

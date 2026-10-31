// terrain_chunk_mesh.cpp — P6-01: chunk build + LOD index decimation.
#include "engine/terrain/terrain_chunk_mesh.hpp"

#include "engine/terrain/terrain_heightmap.hpp"

namespace engine::terrain {

auto ChunkMesh::lod_index_count(std::uint32_t chunk_samples,
                                std::uint32_t lod) noexcept -> std::uint32_t {
  if (chunk_samples < 2 || lod >= kMaxLodLevels) {
    return 0;
  }
  const std::uint32_t step = 1u << lod;
  const std::uint32_t quads = (chunk_samples - 1) / step;
  if (quads == 0) {
    return 0;
  }
  return quads * quads * 6u;
}

auto ChunkMesh::lod_total_indices(std::uint32_t chunk_samples) noexcept
    -> std::uint32_t {
  std::uint32_t total = 0;
  for (std::uint32_t l = 0; l < kMaxLodLevels; ++l) {
    total += lod_index_count(chunk_samples, l);
  }
  return total;
}

auto ChunkMesh::build_chunk(const HeightmapGrid& grid, std::uint32_t chunk_x,
                            std::uint32_t chunk_z,
                            std::uint32_t chunk_samples,
                            TerrainVertex* out_verts,
                            std::uint32_t* out_indices,
                            std::uint32_t* lod_offsets) noexcept -> bool {
  if (grid.samples == nullptr || grid.width < chunk_samples ||
      grid.height < chunk_samples || chunk_samples < 2 ||
      out_verts == nullptr || out_indices == nullptr ||
      lod_offsets == nullptr) {
    return false;
  }
  const std::uint32_t stride = chunk_samples - 1;
  // Chunk origin in sample space; clamp so the last chunk never runs past
  // the grid edge (grids whose dims are not a multiple of stride reuse the
  // border samples).
  std::uint32_t ox = chunk_x * stride;
  std::uint32_t oz = chunk_z * stride;
  if (ox + chunk_samples > grid.width) {
    if (grid.width < chunk_samples) {
      return false;
    }
    ox = grid.width - chunk_samples;
  }
  if (oz + chunk_samples > grid.height) {
    if (grid.height < chunk_samples) {
      return false;
    }
    oz = grid.height - chunk_samples;
  }
  const float extent = static_cast<float>(grid.width - 1) * grid.spacing;
  for (std::uint32_t vz = 0; vz < chunk_samples; ++vz) {
    for (std::uint32_t vx = 0; vx < chunk_samples; ++vx) {
      const std::uint32_t sx = ox + vx;
      const std::uint32_t sz = oz + vz;
      const float wx = static_cast<float>(sx) * grid.spacing;
      const float wz = static_cast<float>(sz) * grid.spacing;
      float nx = 0.0F;
      float ny = 1.0F;
      float nz = 0.0F;
      Heightmap::sample_normal(grid, wx, wz, nx, ny, nz);
      TerrainVertex& v =
          out_verts[static_cast<std::size_t>(vz) * chunk_samples + vx];
      v.px = wx;
      v.py = grid.samples[static_cast<std::size_t>(sz) * grid.width + sx];
      v.pz = wz;
      v.nx = nx;
      v.ny = ny;
      v.nz = nz;
      v.u = extent > 0.0F ? wx / extent : 0.0F;
      v.v = extent > 0.0F ? wz / extent : 0.0F;
    }
  }
  std::uint32_t cursor = 0;
  std::uint32_t prev_offset = 0;
  for (std::uint32_t l = 0; l < kMaxLodLevels; ++l) {
    // Degenerate levels (quads == 0) repeat the previous offset so the
    // [offset[l], offset[l+1]) range is empty instead of dangling.
    const std::uint32_t count = lod_index_count(chunk_samples, l);
    if (count == 0) {
      lod_offsets[l] = prev_offset;
      continue;
    }
    lod_offsets[l] = cursor;
    prev_offset = cursor;
    const std::uint32_t step = 1u << l;
    for (std::uint32_t qz = 0; qz + step < chunk_samples; qz += step) {
      for (std::uint32_t qx = 0; qx + step < chunk_samples; qx += step) {
        const std::uint32_t a = qz * chunk_samples + qx;
        const std::uint32_t b = qz * chunk_samples + qx + step;
        const std::uint32_t c = (qz + step) * chunk_samples + qx;
        const std::uint32_t d = (qz + step) * chunk_samples + qx + step;
        // Counter-clockwise winding seen from +Y (front face up).
        out_indices[cursor++] = a;
        out_indices[cursor++] = c;
        out_indices[cursor++] = b;
        out_indices[cursor++] = b;
        out_indices[cursor++] = c;
        out_indices[cursor++] = d;
      }
    }
  }
  return true;
}

auto ChunkMesh::select_lod(float distance, const float* lod_distances,
                           std::uint32_t lod_count) noexcept -> std::uint32_t {
  if (lod_distances == nullptr || lod_count == 0) {
    return 0;
  }
  if (lod_count > kMaxLodLevels) {
    lod_count = kMaxLodLevels;
  }
  for (std::uint32_t l = 0; l < lod_count; ++l) {
    if (distance < lod_distances[l]) {
      return l;
    }
  }
  return lod_count - 1;
}

}  // namespace engine::terrain

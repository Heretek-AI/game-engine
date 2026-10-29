// sdf_brick_builder.hpp — M3-04: sparse 3D Signed Distance Field bricks.
//
// AC-1: Generate sparse 3D SDF bricks for imported meshes on background
// worker threads.
//
// A MeshSdf is a sparse set of 8^3 volumetric bricks covering the mesh
// surface band. Each brick stores SIGNED distances at its (8^3) texels in
// int8-quantized half-voxel units (range [-4,+4] voxels maps to
// [-128,+127]) plus a normalized brick origin in mesh-local units.
// Only bricks intersected by the surface band are materialized: empty
// space (fully-inside / fully-outside beyond the band) is skipped, so a
// typical closed mesh costs a thin shell of bricks — O(surface area),
// not O(volume).
//
// Distance model (CPU reference + test oracle): exact point-to-triangle
// unsigned distance over all mesh triangles (brute force, tests use tiny
// meshes), minus an inside/outside sign from a parity ray cast along +X
// (odd crossings = inside). Downstream consumers (radiance cascade
// sphere-tracing, sky occlusion) sample with trilinear filtering in
// mesh-local space; bricks carry their own AABB so a non-resident sample
// falls back to a conservative far-field value (half the brick extent).
//
// Threading: the builder is a stateless offline converter — one
// MeshSdfBuilder per background worker thread (no shared mutable state,
// no mallocs on the frame path — all scratch lives in caller-provided
// stack buffers at build() time). Mesh vertex/index slices are borrowed
// (caller keeps them alive).
//
// This header compiles SDK-free (no Vulkan types) so the CPU contract is
// unit-tested on every CI runner; the GPU path uploads built bricks into
// a 3D texture (R8_SNORM, one byte per texel) in a follow-up task.
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
#include "engine/renderer/engine_status.hpp"

namespace engine::renderer {

// Brick resolution: 8 texels per axis (512B per brick at int8).
inline constexpr std::uint32_t kSdfBrickRes = 8;
// Max quantization range in voxel units (int8 maps [-kSdfQuantRange,
// +kSdfQuantRange] to [-127,+127]; saturates beyond).
inline constexpr float kSdfQuantRangeVox = 4.0f;
// Surface band: bricks whose closest surface distance is within this many
// voxels of a texel are materialized; bricks fully outside the band are
// skipped (sparsity).
inline constexpr float kSdfBandVox = 2.5f;
// Hard cap on bricks per mesh (32^3 brick address space covers a brick
// grid up to 256^3 texels; typical shells need far fewer).
inline constexpr std::uint32_t kSdfMaxBricks = 32768;
// Hard cap on triangles per build call (keeps the brute-force
// point-to-triangle loop bounded; importers chunk larger meshes).
inline constexpr std::uint32_t kSdfMaxTriangles = 1u << 20;

// One sparse SDF brick: origin in mesh-local units + int8 texels (x-fastest).
struct SdfBrick {
  float origin[3];     // mesh-local coords of texel (0,0,0) corner
  float texel_size;    // mesh-local units per texel
  std::int8_t texels[kSdfBrickRes * kSdfBrickRes * kSdfBrickRes];
};

// Built SDF for one mesh: sparse brick set + mesh-local AABB + stats.
struct MeshSdf {
  // Brick storage borrowed from the caller (build() fills it; brick_count
  // reports how many were materialized, <= capacity).
  SdfBrick* bricks{nullptr};
  std::uint32_t capacity{0};
  std::uint32_t brick_count{0};
  float aabb_min[3]{0.0f, 0.0f, 0.0f};
  float aabb_max[3]{0.0f, 0.0f, 0.0f};
  float texel_size{0.0f};
  std::uint32_t dropped_bricks{0};  // band bricks skipped for lack of room
};

// Build parameters: texel size in mesh-local units + brick capacity.
struct SdfBuildParams {
  float texel_size{0.05f};
  std::uint32_t brick_capacity{kSdfMaxBricks};
};

// Stateless offline converter (one instance per background worker).
class ENGINE_CORE_API MeshSdfBuilder {
 public:
  MeshSdfBuilder() noexcept = default;
  MeshSdfBuilder(const MeshSdfBuilder&) = delete;
  auto operator=(const MeshSdfBuilder&) -> MeshSdfBuilder& = delete;
  ~MeshSdfBuilder() noexcept = default;

  // Build the sparse SDF for one triangle mesh. `positions` holds
  // vertex_count xyz triples; `indices` holds index_count (multiple of 3)
  // entries into positions. `out->bricks` must point at caller storage for
  // params.brick_capacity bricks. Returns ok with out->brick_count set;
  // degenerate input (no verts/tris, bad texel size) -> kInvalidArgument;
  // capacity overflow is NOT an error: extra bricks are counted in
  // dropped_bricks so the importer can grow and retry.
  auto build(const float* positions, std::uint32_t vertex_count,
             const std::uint32_t* indices, std::uint32_t index_count,
             const SdfBuildParams& params, MeshSdf* out) noexcept -> Status;

  // Sample the built SDF at a mesh-local point (trilinear, in voxel
  // units). Non-resident points return the conservative far-field value
  // (+band). Used by tests + the cascade integrator oracle.
  [[nodiscard]] static auto sample_voxel(const MeshSdf& sdf, float x, float y,
                                         float z) noexcept -> float;

  // Sizing helper: worst-case brick bytes for a mesh AABB at a texel size
  // (dense grid equivalent — the sparse shell is always <= this).
  [[nodiscard]] static auto dense_brick_bytes(const float aabb_min[3],
                                              const float aabb_max[3],
                                              float texel_size) noexcept
      -> std::uint64_t;

 private:
  // Exact unsigned distance from point p to triangle (a,b,c).
  static auto point_triangle_dist(const float p[3], const float a[3],
                                  const float b[3], const float c[3]) noexcept
      -> float;
  // Parity ray cast along +X: true = inside (odd crossings).
  auto point_inside(const float p[3], const float* positions,
                    const std::uint32_t* indices,
                    std::uint32_t tri_count) const noexcept -> bool;
};

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif

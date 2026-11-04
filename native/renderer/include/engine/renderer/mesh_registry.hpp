// mesh_registry.hpp — P1-08 mesh_id -> GpuMesh lookup + P4-12 LOD groups.
//
// MeshRegistry is a plain index table (mesh_id is the vector index, 0
// is reserved/invalid). The test uploads one shared cube and hands
// every entity's MeshRenderer.mesh_id the same id; the renderer looks
// each entity's mesh up per draw. Out-of-range or empty slots resolve
// to nullptr so a stale mesh_id fails a draw closed instead of reading
// garbage.
//
// P4-12: LOD groups sit alongside the flat table (existing find/add
// untouched). A group owns up to kMeshLodCount borrowed GpuMesh levels
// (LOD0 finest first) plus an optional screen-coverage reference;
// find_lod() maps a group + camera distance to the level's mesh via
// CoverageFromDistance/SelectLodIndex (mesh_lod.hpp). Plain (non-LOD)
// meshes keep flowing through find() exactly as before.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/core/engine_core_export.hpp"
#include "engine/renderer/mesh_lod.hpp"
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

namespace engine::renderer {

class GpuMesh;

// One LOD group: up to kMeshLodCount borrowed level meshes (LOD0 =
// finest), the authored reference coverage, and the world-space
// bounding radius the CoverageFromDistance math needs. Unset levels
// are nullptr; the effective level count is the leading run of
// non-null levels (level_count()).
struct MeshLodGroup {
  std::array<const GpuMesh*, kMeshLodCount> levels{};
  float ref_coverage{1.0F};
  float bounding_radius{1.0F};
};

class ENGINE_CORE_API MeshRegistry {
 public:
  MeshRegistry() = default;
  MeshRegistry(const MeshRegistry&) = delete;
  auto operator=(const MeshRegistry&) -> MeshRegistry& = delete;
  ~MeshRegistry() = default;

  // Register `mesh` (borrowed — the caller owns its lifetime) and
  // return its mesh_id (>= 1). Null is rejected (returns 0).
  auto add(const GpuMesh* mesh) noexcept -> std::uint32_t;

  // Register one LOD group (meshes borrowed, same ownership as add)
  // and return its group_id (>= 1). Null/empty (no non-null levels)
  // is rejected (returns 0). The mesh_id namespace is shared with
  // add(): ids are unique across plain meshes and LOD groups, and
  // find() resolves a group id to its LOD0 mesh so legacy draw paths
  // keep working unchanged.
  auto add_lod(const MeshLodGroup& group) noexcept -> std::uint32_t;

  // Resolve a mesh_id to the registered mesh, or nullptr when the id is
  // 0, out of range, or the slot is empty. LOD group ids resolve to
  // their LOD0 mesh.
  [[nodiscard]] auto find(std::uint32_t mesh_id) const noexcept
      -> const GpuMesh*;

  // Resolve a LOD group id + camera distance to the selected level's
  // mesh (SelectLodIndex over CoverageFromDistance). Returns nullptr
  // for id 0, out-of-range ids, plain (non-group) ids, or groups with
  // no live level. `out_level` (optional) receives the selected
  // level index (0 = finest); stays untouched on failure.
  [[nodiscard]] auto find_lod(std::uint32_t group_id, float distance,
                              std::size_t* out_level = nullptr) const noexcept
      -> const GpuMesh*;

  // Level count of a group id (leading non-null levels), 0 when the id
  // is not a live group.
  [[nodiscard]] auto lod_level_count(std::uint32_t group_id) const noexcept
      -> std::size_t;

  // Drop all registrations (meshes themselves are untouched).
  auto clear() noexcept -> void;

  [[nodiscard]] auto size() const noexcept -> std::size_t {
    return slots_.size();
  }

 private:
  struct Slot {
    const GpuMesh* mesh{nullptr};
    bool is_group{false};
    MeshLodGroup group{};
  };
  std::vector<Slot> slots_;
};

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif

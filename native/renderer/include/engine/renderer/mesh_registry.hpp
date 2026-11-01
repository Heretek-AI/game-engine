// mesh_registry.hpp — P1-08: mesh_id -> GpuMesh lookup.
//
// MeshRegistry is a plain index table (mesh_id is the vector index, 0
// is reserved/invalid). The test uploads one shared cube and hands
// every entity's MeshRenderer.mesh_id the same id; the renderer looks
// each entity's mesh up per draw. Out-of-range or empty slots resolve
// to nullptr so a stale mesh_id fails a draw closed instead of reading
// garbage.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/core/engine_core_export.hpp"
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

namespace engine::renderer {

class GpuMesh;

class ENGINE_CORE_API MeshRegistry {
 public:
  MeshRegistry() = default;
  MeshRegistry(const MeshRegistry&) = delete;
  auto operator=(const MeshRegistry&) -> MeshRegistry& = delete;
  ~MeshRegistry() = default;

  // Register `mesh` (borrowed — the caller owns its lifetime) and
  // return its mesh_id (>= 1). Null is rejected (returns 0).
  auto add(const GpuMesh* mesh) noexcept -> std::uint32_t;

  // Resolve a mesh_id to the registered mesh, or nullptr when the id is
  // 0, out of range, or the slot is empty.
  [[nodiscard]] auto find(std::uint32_t mesh_id) const noexcept
      -> const GpuMesh*;

  // Drop all registrations (meshes themselves are untouched).
  auto clear() noexcept -> void;

  [[nodiscard]] auto size() const noexcept -> std::size_t {
    return slots_.size();
  }

 private:
  std::vector<const GpuMesh*> slots_;
};

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif

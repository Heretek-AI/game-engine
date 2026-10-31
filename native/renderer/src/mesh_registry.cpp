// mesh_registry.cpp — P1-08 mesh table; see header.
#include "engine/renderer/mesh_registry.hpp"

#include <cstddef>
#include <cstdint>

namespace engine::renderer {

auto MeshRegistry::add(const GpuMesh* mesh) noexcept -> std::uint32_t {
  if (mesh == nullptr) {
    return 0;
  }
  slots_.push_back(mesh);
  const std::size_t id = slots_.size();
  if (id == 0 || id > static_cast<std::size_t>(UINT32_MAX)) {
    if (!slots_.empty()) {
      slots_.pop_back();
    }
    return 0;
  }
  return static_cast<std::uint32_t>(id);
}

auto MeshRegistry::find(std::uint32_t mesh_id) const noexcept
    -> const GpuMesh* {
  if (mesh_id == 0 ||
      static_cast<std::size_t>(mesh_id) > slots_.size()) {
    return nullptr;
  }
  return slots_[static_cast<std::size_t>(mesh_id) - 1];
}

auto MeshRegistry::clear() noexcept -> void { slots_.clear(); }

}  // namespace engine::renderer

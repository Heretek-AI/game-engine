// mesh_registry.cpp — P1-08 mesh table + P4-12 LOD groups; see header.
#include "engine/renderer/mesh_registry.hpp"

#include <cstddef>
#include <cstdint>

namespace engine::renderer {

auto MeshRegistry::add(const GpuMesh* mesh) noexcept -> std::uint32_t {
  if (mesh == nullptr) {
    return 0;
  }
  Slot slot{};
  slot.mesh = mesh;
  slot.is_group = false;
  slots_.push_back(slot);
  const std::size_t id = slots_.size();
  if (id == 0 || id > static_cast<std::size_t>(UINT32_MAX)) {
    if (!slots_.empty()) {
      slots_.pop_back();
    }
    return 0;
  }
  return static_cast<std::uint32_t>(id);
}

auto MeshRegistry::add_lod(const MeshLodGroup& group) noexcept
    -> std::uint32_t {
  // At least LOD0 must be live; deeper holes terminate the chain (the
  // effective levels are the leading run of non-null entries).
  if (group.levels[0] == nullptr) {
    return 0;
  }
  Slot slot{};
  slot.mesh = group.levels[0];
  slot.is_group = true;
  slot.group = group;
  slots_.push_back(slot);
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
  return slots_[static_cast<std::size_t>(mesh_id) - 1].mesh;
}

auto MeshRegistry::find_lod(std::uint32_t group_id, float distance,
                             std::size_t* out_level) const noexcept
    -> const GpuMesh* {
  if (group_id == 0 ||
      static_cast<std::size_t>(group_id) > slots_.size()) {
    return nullptr;
  }
  const Slot& slot = slots_[static_cast<std::size_t>(group_id) - 1];
  if (!slot.is_group) {
    return nullptr;
  }
  std::size_t count = 0;
  for (const GpuMesh* level : slot.group.levels) {
    if (level == nullptr) {
      break;
    }
    ++count;
  }
  if (count == 0) {
    return nullptr;
  }
  const float coverage = CoverageFromDistance(
      distance, slot.group.bounding_radius, slot.group.ref_coverage);
  std::size_t level = SelectLodIndex(coverage);
  if (level >= count) {
    level = count - 1;
  }
  // Walk past holes (should not happen past the leading run, but a
  // cleared mesh must fail closed, never read a null level).
  while (level > 0 && slot.group.levels[level] == nullptr) {
    --level;
  }
  const GpuMesh* mesh = slot.group.levels[level];
  if (mesh == nullptr) {
    return nullptr;
  }
  if (out_level != nullptr) {
    *out_level = level;
  }
  return mesh;
}

auto MeshRegistry::lod_level_count(std::uint32_t group_id) const noexcept
    -> std::size_t {
  if (group_id == 0 ||
      static_cast<std::size_t>(group_id) > slots_.size()) {
    return 0;
  }
  const Slot& slot = slots_[static_cast<std::size_t>(group_id) - 1];
  if (!slot.is_group) {
    return 0;
  }
  std::size_t count = 0;
  for (const GpuMesh* level : slot.group.levels) {
    if (level == nullptr) {
      break;
    }
    ++count;
  }
  return count;
}

auto MeshRegistry::clear() noexcept -> void { slots_.clear(); }

}  // namespace engine::renderer

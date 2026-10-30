// generic_systems.cpp — ABI 4.2 generic component/system tail; see header.
//
// NOTE (ILLU-M3A split): this file holds registration + entity create/get/set.
// ILLU-M3B appends register_system/run below the get() definition, before the
// closing namespace.
#include "generic_systems.hpp"

#include <cstdio>
#include <cstring>

namespace engine::bridge {
namespace {

constexpr std::size_t kMaxNameLen = 255;

// Component size/alignment as Flecs recorded it; nullptr when `id` is not a
// component (or not alive).
auto TypeInfoOf(ecs_world_t* world, uint64_t id) noexcept
    -> const ecs_type_info_t* {
  if (id == 0 || !ecs_is_alive(world, static_cast<ecs_entity_t>(id))) {
    return nullptr;
  }
  return ecs_get_type_info(world, static_cast<ecs_id_t>(id));
}

auto IsPowerOfTwo(uint32_t v) noexcept -> bool {
  return v != 0U && (v & (v - 1U)) == 0U;
}

}  // namespace

auto GenericSystems::shutdown() noexcept -> void {
  for (auto& s : systems_) {
    if (s.query != nullptr) {
      ecs_query_fini(s.query);
      s.query = nullptr;
    }
  }
  systems_.clear();
}

auto GenericSystems::register_component(ecs_world_t* world, const char* name,
                                        uint32_t size,
                                        uint32_t alignment) noexcept
    -> uint64_t {
  if (world == nullptr || name == nullptr || name[0] == '\0') {
    return 0;
  }
  if (std::strlen(name) > kMaxNameLen || size == 0U ||
      !IsPowerOfTwo(alignment) || alignment > 64U || size % alignment != 0U) {
    return 0;
  }
  // sep = "" keeps "Game.Mover" one flat name (the default '.' separator
  // would look up a "Mover" child of a "Game" scope instead).
  const ecs_entity_t existing =
      ecs_lookup_path_w_sep(world, 0, name, "", "", false);
  if (existing != 0) {
    const ecs_type_info_t* ti = TypeInfoOf(world, existing);
    if (ti != nullptr && ti->size == static_cast<ecs_size_t>(size) &&
        ti->alignment == static_cast<ecs_size_t>(alignment)) {
      return existing;
    }
    // Different layout (or a non-component entity with this name). Never
    // call ecs_component_init here: Flecs aborts the process on a size
    // mismatch (INVALID_COMPONENT_SIZE), even in release builds.
    return 0;
  }
  ecs_entity_desc_t ed{};
  ed.name = name;
  ed.sep = "";
  ed.root_sep = "";
  const ecs_entity_t ent = ecs_entity_init(world, &ed);
  if (ent == 0) {
    return 0;
  }
  ecs_component_desc_t cd{};
  cd.entity = ent;
  cd.type.size = static_cast<ecs_size_t>(size);
  cd.type.alignment = static_cast<ecs_size_t>(alignment);
  return ecs_component_init(world, &cd);
}

auto GenericSystems::create_entity(ecs_world_t* world) noexcept -> uint64_t {
  if (world == nullptr || in_run_) {
    return 0;
  }
  return ecs_new(world);
}

auto GenericSystems::set(ecs_world_t* world, uint64_t entity,
                         uint64_t component, const void* data,
                         uint32_t size) noexcept -> int32_t {
  if (world == nullptr || data == nullptr) {
    return ENGINE_ABI_ERR_NULL_ARG;
  }
  if (in_run_) {
    return ENGINE_ABI_ERR_IN_RUN;
  }
  if (entity == 0 || !ecs_is_alive(world, static_cast<ecs_entity_t>(entity))) {
    return ENGINE_ABI_ERR_BAD_ARG;
  }
  const ecs_type_info_t* ti = TypeInfoOf(world, component);
  if (ti == nullptr) {
    return ENGINE_ABI_ERR_BAD_ARG;
  }
  if (ti->size != static_cast<ecs_size_t>(size)) {
    return ENGINE_ABI_ERR_SIZE_MISMATCH;
  }
  ecs_set_id(world, static_cast<ecs_entity_t>(entity),
             static_cast<ecs_id_t>(component), static_cast<std::size_t>(size),
             data);
  return 0;
}

auto GenericSystems::get(ecs_world_t* world, uint64_t entity,
                         uint64_t component, void* out,
                         uint32_t size) noexcept -> int32_t {
  if (world == nullptr || out == nullptr) {
    return ENGINE_ABI_ERR_NULL_ARG;
  }
  if (entity == 0 || !ecs_is_alive(world, static_cast<ecs_entity_t>(entity))) {
    return ENGINE_ABI_ERR_BAD_ARG;
  }
  const ecs_type_info_t* ti = TypeInfoOf(world, component);
  if (ti == nullptr) {
    return ENGINE_ABI_ERR_BAD_ARG;
  }
  if (ti->size != static_cast<ecs_size_t>(size)) {
    return ENGINE_ABI_ERR_SIZE_MISMATCH;
  }
  const void* src = ecs_get_id(world, static_cast<ecs_entity_t>(entity),
                               static_cast<ecs_id_t>(component));
  if (src == nullptr) {
    return ENGINE_ABI_ERR_NOT_FOUND;
  }
  std::memcpy(out, src, size);
  return 0;
}

}  // namespace engine::bridge

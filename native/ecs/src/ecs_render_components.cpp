// ecs_render_components.cpp — P1-08 registration helper; see header.
#include "engine/ecs/ecs_render_components.hpp"

#include <cstdint>
#include <cstring>

#include "flecs.h"

namespace engine::ecs {

namespace {

constexpr std::size_t kMaxNameLen = 255;

auto IsPowerOfTwo(std::uint32_t v) noexcept -> bool {
  return v != 0U && (v & (v - 1U)) == 0U;
}

auto TypeInfoOf(ecs_world_t* world, std::uint64_t id) noexcept
    -> const ecs_type_info_t* {
  if (id == 0 || !ecs_is_alive(world, static_cast<ecs_entity_t>(id))) {
    return nullptr;
  }
  return ecs_get_type_info(world, static_cast<ecs_id_t>(id));
}

}  // namespace

auto RegisterRenderComponent(ecs_world_t* world, const char* name,
                             std::uint32_t size, std::uint32_t alignment) noexcept
    -> std::uint64_t {
  if (world == nullptr || name == nullptr || name[0] == '\0') {
    return 0;
  }
  if (std::strlen(name) > kMaxNameLen || size == 0U ||
      !IsPowerOfTwo(alignment) || alignment > 64U || size % alignment != 0U) {
    return 0;
  }
  // sep = "" keeps "Engine.Runtime.Camera" one flat name (the default '.'
  // separator would look up a "Camera" child of an "Engine.Runtime"
  // scope instead).
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

}  // namespace engine::ecs

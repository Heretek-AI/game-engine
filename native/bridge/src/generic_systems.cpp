// generic_systems.cpp — ABI 4.2 generic component/system tail; see header.
#include "generic_systems.hpp"

#include <cstddef>
#include <cstring>

#include "engine/ecs/ecs_render_components.hpp"
#include "name_copy.hpp"

namespace engine::bridge {
namespace {

// Component size/alignment as Flecs recorded it; nullptr when `id` is not a
// component (or not alive).
auto TypeInfoOf(ecs_world_t* world, uint64_t id) noexcept
    -> const ecs_type_info_t* {
  if (id == 0 || !ecs_is_alive(world, static_cast<ecs_entity_t>(id))) {
    return nullptr;
  }
  return ecs_get_type_info(world, static_cast<ecs_id_t>(id));
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
  // Single implementation lives in engine_core_ecs
  // (RegisterRenderComponent): identical idempotent lookup-or-init, so
  // the ECS module's render registration and this ABI entry can never
  // drift apart (gotcha 20).
  const std::uint64_t id = engine::ecs::RegisterRenderComponent(
      world, name, static_cast<std::uint32_t>(size),
      static_cast<std::uint32_t>(alignment));
  return id;
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

auto GenericSystems::register_system(ecs_world_t* world, const char* name,
                                     const uint64_t* components,
                                     const uint32_t* access,
                                     uint32_t term_count,
                                     EngineSystemCallback callback,
                                     void* user_data) noexcept -> int32_t {
  if (world == nullptr || components == nullptr || access == nullptr ||
      callback == nullptr) {
    return ENGINE_ABI_ERR_NULL_ARG;
  }
  if (in_run_) {
    return ENGINE_ABI_ERR_IN_RUN;
  }
  if (term_count == 0U || term_count > ENGINE_ABI_MAX_TERMS) {
    return ENGINE_ABI_ERR_BAD_ARG;
  }
  System sys{};
  ecs_query_desc_t qd{};
  for (uint32_t i = 0; i < term_count; ++i) {
    const ecs_type_info_t* ti = TypeInfoOf(world, components[i]);
    if (ti == nullptr) {
      return ENGINE_ABI_ERR_BAD_ARG;
    }
    if (access[i] != ENGINE_ABI_ACCESS_IN &&
        access[i] != ENGINE_ABI_ACCESS_INOUT) {
      return ENGINE_ABI_ERR_BAD_ARG;
    }
    for (uint32_t k = 0; k < i; ++k) {
      if (components[k] == components[i]) {
        return ENGINE_ABI_ERR_BAD_ARG;  // duplicate term
      }
    }
    qd.terms[i].id = static_cast<ecs_id_t>(components[i]);
    // Owned components only: every field is then a real column array
    // (an inherited/shared match would be a single pointer, not an array).
    qd.terms[i].src.id = EcsSelf;
    qd.terms[i].inout = static_cast<int16_t>(
        (access[i] == ENGINE_ABI_ACCESS_IN) ? EcsIn : EcsInOut);
    sys.sizes[i] = static_cast<int32_t>(ti->size);
  }
  qd.cache_kind = EcsQueryCacheAuto;
  sys.query = ecs_query_init(world, &qd);
  if (sys.query == nullptr) {
    return ENGINE_ABI_ERR_INTERNAL;
  }
  if (name != nullptr) {
    // Bounded copy keeps truncation explicit (clamped to buffer capacity).
    CopySystemName(sys.name, name);
  }
  sys.callback = callback;
  sys.user_data = user_data;
  sys.term_count = term_count;
  try {
    systems_.push_back(sys);
  } catch (...) {
    ecs_query_fini(sys.query);
    return ENGINE_ABI_ERR_INTERNAL;
  }
  return static_cast<int32_t>(systems_.size() - 1U);
}

auto GenericSystems::run(ecs_world_t* world, float dt) noexcept -> int32_t {
  if (world == nullptr) {
    return ENGINE_ABI_ERR_NULL_ARG;
  }
  if (in_run_) {
    return ENGINE_ABI_ERR_IN_RUN;
  }
  in_run_ = true;
  for (const System& sys : systems_) {
    ecs_iter_t it = ecs_query_iter(world, sys.query);
    while (ecs_query_next(&it)) {
      if (it.count <= 0) {
        continue;
      }
      EngineChunkColumns view{};
      for (uint32_t i = 0; i < sys.term_count; ++i) {
        view.columns[i] = ecs_field_w_size(
            &it, static_cast<std::size_t>(sys.sizes[i]),
            static_cast<int8_t>(i));
        view.sizes[i] = sys.sizes[i];
      }
      view.entities = reinterpret_cast<const uint64_t*>(it.entities);  // NOLINT
      view.count = it.count;
      view.term_count = static_cast<int32_t>(sys.term_count);
      view.user_data = sys.user_data;
      sys.callback(&view, dt);
    }
  }
  in_run_ = false;
  return 0;
}

}  // namespace engine::bridge

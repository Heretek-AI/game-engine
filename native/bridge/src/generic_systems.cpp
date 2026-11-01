// generic_systems.cpp — ABI 4.2 generic component/system tail; see header.
#include "generic_systems.hpp"

#include <cstddef>
#include <cstring>

#include "name_copy.hpp"

namespace engine::bridge {
namespace {

// kMaxNameLen caps component name length in register_component; system
// names are clamped separately by CopySystemName.
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

namespace {
// Max field-name bytes copied into a FieldRow (same cap as component names).
constexpr std::size_t kMaxFieldNameLen = 255;
}  // namespace

auto GenericSystems::RegisteredSizeAlign(ecs_world_t* world, const char* name,
                                         uint32_t* out_size,
                                         uint32_t* out_align) noexcept
    -> bool {
  if (world == nullptr || name == nullptr || name[0] == '\0') {
    return false;
  }
  const ecs_entity_t existing =
      ecs_lookup_path_w_sep(world, 0, name, "", "", false);
  if (existing == 0) {
    return false;
  }
  const ecs_type_info_t* ti = TypeInfoOf(world, existing);
  if (ti == nullptr) {
    return false;
  }
  if (out_size != nullptr) {
    *out_size = static_cast<uint32_t>(ti->size);
  }
  if (out_align != nullptr) {
    *out_align = static_cast<uint32_t>(ti->alignment);
  }
  return true;
}

auto GenericSystems::SeedBuiltinRows(const char* name) noexcept -> void {
  if (name == nullptr || std::strcmp(name, "LocalTransform") != 0) {
    return;
  }
  if (fields_.find(name) != fields_.end()) {
    return;  // user description wins
  }
  try {
    fields_[name] = std::vector<FieldRow>{
        {"position", 0, ENGINE_ABI_FIELD_VEC3},
        {"rotation", 16, ENGINE_ABI_FIELD_QUAT},
        {"scale", 32, ENGINE_ABI_FIELD_VEC3},
    };
  } catch (...) {
    // OOM: readers report undescribed; scene falls back to $bytes.
  }
}

auto GenericSystems::describe(ecs_world_t* world, const char* name,
                              const EngineFieldDesc* fields,
                              uint32_t count) noexcept -> int32_t {
  if (world == nullptr || name == nullptr) {
    return ENGINE_ABI_ERR_NULL_ARG;
  }
  SeedBuiltinRows(name);
  uint32_t size = 0;
  if (!RegisteredSizeAlign(world, name, &size, nullptr)) {
    return ENGINE_ABI_ERR_NOT_FOUND;
  }
  if (count > 0 && fields == nullptr) {
    return ENGINE_ABI_ERR_NULL_ARG;
  }
  std::vector<FieldRow> rows;
  for (uint32_t i = 0; i < count; ++i) {
    const EngineFieldDesc& f = fields[i];
    if (f.name == nullptr || f.name[0] == '\0' ||
        std::strlen(f.name) > kMaxFieldNameLen) {
      return ENGINE_ABI_ERR_BAD_ARG;
    }
    if (f.type > ENGINE_ABI_FIELD_ASSET_REF) {
      return ENGINE_ABI_ERR_BAD_ARG;
    }
    if (f.offset >= size) {
      return ENGINE_ABI_ERR_BAD_ARG;
    }
    FieldRow row;
    row.name = f.name;
    row.offset = f.offset;
    row.type = f.type;
    try {
      rows.push_back(std::move(row));
    } catch (...) {
      return ENGINE_ABI_ERR_INTERNAL;
    }
  }
  try {
    fields_[name] = std::move(rows);
  } catch (...) {
    return ENGINE_ABI_ERR_INTERNAL;
  }
  return 0;
}

auto GenericSystems::field_count(ecs_world_t* world, const char* name,
                                 uint32_t* out_count) noexcept -> int32_t {
  if (world == nullptr || name == nullptr || out_count == nullptr) {
    return ENGINE_ABI_ERR_NULL_ARG;
  }
  SeedBuiltinRows(name);
  uint32_t size = 0;
  if (!RegisteredSizeAlign(world, name, &size, nullptr)) {
    return ENGINE_ABI_ERR_NOT_FOUND;
  }
  const auto it = fields_.find(name);
  *out_count = (it == fields_.end())
                   ? 0U
                   : static_cast<uint32_t>(it->second.size());
  return 0;
}

auto GenericSystems::field_at(ecs_world_t* world, const char* name,
                              uint32_t index, char* out_name,
                              uint32_t name_cap, uint32_t* out_offset,
                              uint32_t* out_type) noexcept -> int32_t {
  if (world == nullptr || name == nullptr || out_name == nullptr ||
      out_offset == nullptr || out_type == nullptr) {
    return ENGINE_ABI_ERR_NULL_ARG;
  }
  if (name_cap == 0) {
    return ENGINE_ABI_ERR_BAD_ARG;
  }
  SeedBuiltinRows(name);
  uint32_t size = 0;
  if (!RegisteredSizeAlign(world, name, &size, nullptr)) {
    return ENGINE_ABI_ERR_NOT_FOUND;
  }
  const auto it = fields_.find(name);
  if (it == fields_.end() || index >= it->second.size()) {
    return ENGINE_ABI_ERR_BAD_ARG;
  }
  const FieldRow& row = it->second[index];
  const std::size_t copy =
      (row.name.size() < static_cast<std::size_t>(name_cap - 1))
          ? row.name.size()
          : static_cast<std::size_t>(name_cap - 1);
  std::memcpy(out_name, row.name.data(), copy);
  out_name[copy] = '\0';
  *out_offset = row.offset;
  *out_type = row.type;
  return 0;
}

}  // namespace engine::bridge

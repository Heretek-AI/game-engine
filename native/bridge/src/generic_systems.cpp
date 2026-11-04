// generic_systems.cpp — ABI 4.2 generic component/system tail; see header.
#include "generic_systems.hpp"

#include <cstddef>
#include <cstring>
#include <string_view>

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

// P3-04 lane size table indexed by ENGINE_FIELD_* (0..10).
constexpr uint32_t kFieldLaneSizes[11] = {4, 4, 4, 1, 8, 12, 16, 16, 16, 8, 8};

// (name, offset, type) rows; types are ENGINE_FIELD_* values.
struct BuiltinField {
  const char* name;
  uint32_t offset;
  uint32_t type;
};

// LocalTransform 64B: position vec3 @0, rotation quat @12, scale vec3 @28.
constexpr BuiltinField kLocalTransformFields[] = {
    {"position", 0, 5}, {"rotation", 12, 7}, {"scale", 28, 5}};
// Camera 16B.
constexpr BuiltinField kCameraFields[] = {{"fov_y_rad", 0, 0},
                                          {"near_z", 4, 0},
                                          {"far_z", 8, 0},
                                          {"is_main", 12, 2}};
// MeshRenderer 20B: mesh_id is asset-ref (inspector keys its asset
// TextBox+drop off type 10; the scene writer keeps its bespoke path).
constexpr BuiltinField kMeshRendererFields[] = {{"mesh_id", 0, 10},
                                                {"base_color", 4, 8}};
// DirectionalLight 16B: native lane name is `shadow`; scene JSON keeps
// the legacy `intensity` key (reader accepts both).
constexpr BuiltinField kDirectionalLightFields[] = {{"direction", 0, 5},
                                                    {"shadow", 12, 0}};

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
  if (id != 0 && world != nullptr && name != nullptr) {
    // P3-04: auto-describe built-ins on first registration. Re-registration
    // is idempotent (describe overwrites with the identical table).
    const BuiltinField* fields = nullptr;
    uint32_t count = 0;
    uint32_t want_size = 0;
    const std::string_view n(name);
    if (n == "LocalTransform") {
      fields = kLocalTransformFields;
      count = 3;
      want_size = 64;
    } else if (n == "Engine.Runtime.Camera") {
      fields = kCameraFields;
      count = 4;
      want_size = 16;
    } else if (n == "Engine.Runtime.MeshRenderer") {
      fields = kMeshRendererFields;
      count = 2;
      want_size = 20;
    } else if (n == "Engine.Runtime.DirectionalLight") {
      fields = kDirectionalLightFields;
      count = 2;
      want_size = 16;
    }
    if (fields != nullptr && size == want_size) {
      EngineFieldDesc rows[4];
      for (uint32_t i = 0; i < count; ++i) {
        rows[i].name = fields[i].name;
        rows[i].offset = fields[i].offset;
        rows[i].type = fields[i].type;
      }
      // Best-effort: validation cannot fail for these pre-validated
      // tables; ignore the code (a drift fails closed = undescribed).
      (void)describe(world, name, rows, count);
    }
  }
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
  sys.phase = ENGINE_PHASE_UPDATE;
  try {
    systems_.push_back(sys);
  } catch (...) {
    ecs_query_fini(sys.query);
    return ENGINE_ABI_ERR_INTERNAL;
  }
  return static_cast<int32_t>(systems_.size() - 1U);
}

// --- P3-04 built-in descriptions -------------------------------------------
// Hardcoded field tables (plan §7). Installed automatically when the
// matching built-in is registered through the ABI path
// (GenericSystems::register_component, which both paths funnel through:
// the plan's "built-ins call the same core as the ABI entry"), via the
// same describe() validation. Sizes are checked against the live
// component so a layout drift fails closed (no description) instead of
// describing wrong offsets.


auto GenericSystems::run(ecs_world_t* world, float dt) noexcept -> int32_t {
  return run(world, dt, nullptr);
}

auto GenericSystems::run(ecs_world_t* world, float dt, SystemTimingSink* sink) noexcept
    -> int32_t {
  return dispatch(world, -1, dt, sink, true);
}

auto GenericSystems::set_phase(uint32_t system, uint32_t phase) noexcept
    -> int32_t {
  if (in_run_) {
    return ENGINE_ABI_ERR_IN_RUN;
  }
  if (system >= systems_.size() || phase >= ENGINE_PHASE_COUNT) {
    return ENGINE_ABI_ERR_BAD_ARG;
  }
  systems_[system].phase = phase;
  return 0;
}

auto GenericSystems::run_phase(ecs_world_t* world, uint32_t phase, float dt,
                               SystemTimingSink* sink) noexcept -> int32_t {
  if (phase >= ENGINE_PHASE_COUNT) {
    return ENGINE_ABI_ERR_BAD_ARG;
  }
  return dispatch(world, static_cast<int64_t>(phase), dt, sink, false);
}

auto GenericSystems::dispatch(ecs_world_t* world, int64_t phase, float dt,
                              SystemTimingSink* sink, bool resize) noexcept
    -> int32_t {
  if (world == nullptr) {
    return ENGINE_ABI_ERR_NULL_ARG;
  }
  if (in_run_) {
    return ENGINE_ABI_ERR_IN_RUN;
  }
  in_run_ = true;
  if (resize && sink != nullptr && sink->resize != nullptr) {
    sink->resize(sink->ctx, systems_.size());
  }
  uint32_t sys_index = 0;
  for (const System& sys : systems_) {
    if (phase >= 0 && static_cast<int64_t>(sys.phase) != phase) {
      ++sys_index;
      continue;
    }
    const auto t0 = std::chrono::steady_clock::now();
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
    if (sink != nullptr && sink->record != nullptr) {
      const auto t1 = std::chrono::steady_clock::now();
      const float ms = static_cast<float>(
          std::chrono::duration<double, std::milli>(t1 - t0).count());
      sink->record(sink->ctx, sys_index, ms >= 0.0F ? ms : 0.0F);
    }
    ++sys_index;
  }
  in_run_ = false;
  return 0;
}

// --- P3-04 component field descriptions ------------------------------------

auto GenericSystems::describe(ecs_world_t* world, const char* name,
                              const EngineFieldDesc* fields,
                              uint32_t count) noexcept -> int32_t {
  if (world == nullptr || name == nullptr || name[0] == '\0') {
    return ENGINE_ABI_ERR_NULL_ARG;
  }
  if (count > 0 && fields == nullptr) {
    return ENGINE_ABI_ERR_NULL_ARG;
  }
  if (count > 256) {
    return ENGINE_ABI_ERR_BAD_ARG;
  }
  // describe requires a prior register (same lookup register uses).
  const ecs_entity_t existing =
      ecs_lookup_path_w_sep(world, 0, name, "", "", false);
  if (existing == 0) {
    return ENGINE_ABI_ERR_NOT_FOUND;
  }
  const ecs_type_info_t* ti = TypeInfoOf(world, existing);
  if (ti == nullptr) {
    return ENGINE_ABI_ERR_NOT_FOUND;
  }
  const auto comp_size = static_cast<uint32_t>(ti->size);
  // Validate everything before touching the store (all-or-nothing).
  ComponentDescription next;
  next.fields.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const EngineFieldDesc& f = fields[i];
    if (f.name == nullptr || f.name[0] == '\0') {
      return ENGINE_ABI_ERR_BAD_ARG;
    }
    if (f.type > ENGINE_FIELD_ASSET_REF) {
      return ENGINE_ABI_ERR_BAD_ARG;
    }
    const uint32_t lane = kFieldLaneSizes[f.type];
    // Overflow-safe: offset > size OR lane > size - offset.
    if (f.offset > comp_size || lane > comp_size - f.offset) {
      return ENGINE_ABI_ERR_BAD_ARG;
    }
    for (const auto& prev : next.fields) {
      if (prev.name == f.name) {
        return ENGINE_ABI_ERR_BAD_ARG;  // duplicate name
      }
      // Overlap check: [a,b) vs [c,d) overlap iff a < d && c < b.
      const uint32_t pa = prev.offset;
      const uint32_t pb = prev.offset + kFieldLaneSizes[prev.type];
      const uint32_t fa = f.offset;
      const uint32_t fb = f.offset + lane;
      if (fa < pb && pa < fb) {
        return ENGINE_ABI_ERR_BAD_ARG;
      }
    }
    ComponentDescription::Field nf;
    nf.name = f.name;  // copy into native-owned storage
    nf.offset = f.offset;
    nf.type = f.type;
    try {
      next.fields.push_back(std::move(nf));
    } catch (...) {
      return ENGINE_ABI_ERR_INTERNAL;
    }
  }
  try {
    descriptions_[name] = std::move(next);  // overwrite (hot-reload)
  } catch (...) {
    return ENGINE_ABI_ERR_INTERNAL;
  }
  return 0;
}

auto GenericSystems::field_count(ecs_world_t* world, const char* name) noexcept
    -> int32_t {
  if (world == nullptr || name == nullptr || name[0] == '\0') {
    return ENGINE_ABI_ERR_NULL_ARG;
  }
  // Unknown name: NOT_FOUND. Note a describe-before-register also lands
  // here (no description was ever stored for it).
  const auto it = descriptions_.find(name);
  if (it == descriptions_.end()) {
    // Distinguish "registered but undescribed" (0) from "unknown"
    // (NOT_FOUND): check the live world registry.
    const ecs_entity_t existing =
        ecs_lookup_path_w_sep(world, 0, name, "", "", false);
    if (existing == 0 || TypeInfoOf(world, existing) == nullptr) {
      return ENGINE_ABI_ERR_NOT_FOUND;
    }
    // Registered but never described: legal, 0 (plan §3.2 rule 7 reads
    // count >= 0 with 0 for empty/undescribed-known).
    return 0;
  }
  return static_cast<int32_t>(it->second.fields.size());
}

auto GenericSystems::field_at(ecs_world_t* world, const char* name,
                              uint32_t index,
                              EngineFieldDesc* out_desc) noexcept -> int32_t {
  if (world == nullptr || name == nullptr || name[0] == '\0' ||
      out_desc == nullptr) {
    return ENGINE_ABI_ERR_NULL_ARG;
  }
  const auto it = descriptions_.find(name);
  if (it == descriptions_.end()) {
    return ENGINE_ABI_ERR_NOT_FOUND;
  }
  if (index >= it->second.fields.size()) {
    return ENGINE_ABI_ERR_NOT_FOUND;
  }
  const auto& f = it->second.fields[index];
  out_desc->name = f.name.c_str();  // native-owned; valid until destroy
  out_desc->offset = f.offset;      // or re-describe of this component
  out_desc->type = f.type;
  return 0;
}

}  // namespace engine::bridge

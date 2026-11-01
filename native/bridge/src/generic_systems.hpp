// generic_systems.hpp — ABI 4.2: name-registered components + term-list
// systems for Engine.Analyzers-generated IComponent code.
//
// ABI 4.4 (P3-04): component field metadata. component_describe stores
// per-field (name, offset, type) rows keyed by the registered component
// name; field_count/field_at read them back for the C# editor.
// LocalTransform describes itself natively at first use. Descriptions
// survive re-registration with the same layout.
//
// Owned by EngineWorldImpl (engine_abi.cpp). Every method takes the raw
// Flecs world; nothing here owns the world. Not thread-safe: register and
// run are serialized by the caller (same rule as the rest of the ABI).
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/bridge/engine_abi.h"
#include "flecs.h"

namespace engine::bridge {

class GenericSystems {
 public:
  GenericSystems() = default;
  GenericSystems(const GenericSystems&) = delete;
  auto operator=(const GenericSystems&) -> GenericSystems& = delete;
  GenericSystems(GenericSystems&&) = delete;
  auto operator=(GenericSystems&&) -> GenericSystems& = delete;
  ~GenericSystems() = default;

  // Finalizes every cached query. Call BEFORE the Flecs world is destroyed.
  auto shutdown() noexcept -> void;

  auto register_component(ecs_world_t* world, const char* name, uint32_t size,
                          uint32_t alignment) noexcept -> uint64_t;
  auto create_entity(ecs_world_t* world) noexcept -> uint64_t;
  auto set(ecs_world_t* world, uint64_t entity, uint64_t component,
           const void* data, uint32_t size) noexcept -> int32_t;
  auto get(ecs_world_t* world, uint64_t entity, uint64_t component, void* out,
           uint32_t size) noexcept -> int32_t;
  // P3-05: remove `component` from `entity` (no-op when absent). Returns
  // 0 on success or an ENGINE_ABI_ERR_* code.
  auto remove(ecs_world_t* world, uint64_t entity,
              uint64_t component) noexcept -> int32_t;
  auto register_system(ecs_world_t* world, const char* name,
                       const uint64_t* components, const uint32_t* access,
                       uint32_t term_count, EngineSystemCallback callback,
                       void* user_data) noexcept -> int32_t;
  auto run(ecs_world_t* world, float dt) noexcept -> int32_t;

  // ABI 4.4 field metadata (P3-04). describe validates against the
  // registered component size and replaces any prior rows; field_count
  // reads the row count (0 when undescribed); field_at copies one row
  // (name truncated to name_cap - 1 chars + NUL).
  auto describe(ecs_world_t* world, const char* name,
                const EngineFieldDesc* fields, uint32_t count) noexcept
      -> int32_t;
  auto field_count(ecs_world_t* world, const char* name,
                   uint32_t* out_count) noexcept -> int32_t;
  auto field_at(ecs_world_t* world, const char* name, uint32_t index,
                char* out_name, uint32_t name_cap, uint32_t* out_offset,
                uint32_t* out_type) noexcept -> int32_t;

 private:
  struct System {
    char name[64];
    ecs_query_t* query;
    EngineSystemCallback callback;
    void* user_data;
    uint32_t term_count;
    int32_t sizes[ENGINE_ABI_MAX_TERMS];
  };

  std::vector<System> systems_;
  bool in_run_{false};

  struct FieldRow {
    std::string name;
    uint32_t offset;
    uint32_t type;
  };

  // Size + alignment as Flecs recorded it (0/0 when never registered).
  auto RegisteredSizeAlign(ecs_world_t* world, const char* name,
                           uint32_t* out_size,
                           uint32_t* out_align) noexcept -> bool;

  // Lazily seeds the native built-in description (LocalTransform:
  // position vec3 @0, rotation quat @16, scale vec3 @32) when `name`
  // matches and no user description exists yet. No-op otherwise.
  auto SeedBuiltinRows(const char* name) noexcept -> void;

  // Field metadata keyed by registered component name. Descriptions
  // survive re-registration with the same layout.
  std::unordered_map<std::string, std::vector<FieldRow>> fields_;
};

}  // namespace engine::bridge

// generic_systems.hpp — ABI 4.2: name-registered components + term-list
// systems for Engine.Analyzers-generated IComponent code.
//
// Owned by EngineWorldImpl (engine_abi.cpp). Every method takes the raw
// Flecs world; nothing here owns the world. Not thread-safe: register and
// run are serialized by the caller (same rule as the rest of the ABI).
#pragma once

#include <cstdint>
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
  auto register_system(ecs_world_t* world, const char* name,
                       const uint64_t* components, const uint32_t* access,
                       uint32_t term_count, EngineSystemCallback callback,
                       void* user_data) noexcept -> int32_t;
  auto run(ecs_world_t* world, float dt) noexcept -> int32_t;

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
};

}  // namespace engine::bridge

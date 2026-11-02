// generic_systems.hpp — ABI 4.2: name-registered components + term-list
// systems for Engine.Analyzers-generated IComponent code.
//
// Owned by EngineWorldImpl (engine_abi.cpp). Every method takes the raw
// Flecs world; nothing here owns the world. Not thread-safe: register and
// run are serialized by the caller (same rule as the rest of the ABI).
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/bridge/engine_abi.h"
#include "flecs.h"

namespace engine::bridge {

// Forward-declared: GenericSystems must not include the full profiler
// header (bridge TU hygiene — the profiler lives in engine_abi.cpp's TU).
// The timed run() overload takes an opaque sink with this exact
// vtable-free shape: two function pointers + context, called once per
// system.
struct SystemTimingSink {
  void* ctx{nullptr};
  void (*resize)(void* ctx, std::size_t n){nullptr};
  void (*record)(void* ctx, uint32_t system, float ms){nullptr};
};

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
  // P3-09 overload: identical dispatch, plus per-system CPU timings into
  // `sink` (null sink == untimed run). Timings are steady_clock deltas
  // per system in milliseconds, measured around that system's table loop.
  auto run(ecs_world_t* world, float dt, SystemTimingSink* sink) noexcept -> int32_t;

  // P2-03 phase routing. Every system starts in ENGINE_PHASE_UPDATE.
  // set_phase moves one system (index from register_system) to another
  // pipeline phase (ENGINE_PHASE_*). Returns 0 or an ENGINE_ABI_ERR_* code.
  auto set_phase(uint32_t system, uint32_t phase) noexcept -> int32_t;
  // Runs only the systems whose phase == `phase`, in registration order.
  // Timings go to `sink` via record() only (the caller resizes once per
  // frame), so a frame's phases share one profiler row per system.
  auto run_phase(ecs_world_t* world, uint32_t phase, float dt,
                 SystemTimingSink* sink) noexcept -> int32_t;
  [[nodiscard]] auto system_count() const noexcept -> std::size_t {
    return systems_.size();
  }

 private:
  struct System {
    char name[64];
    ecs_query_t* query;
    EngineSystemCallback callback;
    void* user_data;
    uint32_t term_count;
    uint32_t phase;
    int32_t sizes[ENGINE_ABI_MAX_TERMS];
  };

  // Shared dispatch: runs every system when `phase` < 0, else only the
  // systems in that phase. resize: call sink->resize first (legacy run).
  auto dispatch(ecs_world_t* world, int64_t phase, float dt,
                SystemTimingSink* sink, bool resize) noexcept -> int32_t;

  std::vector<System> systems_;
  bool in_run_{false};
};

}  // namespace engine::bridge

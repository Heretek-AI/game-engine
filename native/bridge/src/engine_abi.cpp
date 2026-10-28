// engine_abi.cpp — M1-04: implements the C99 ABI (engine_abi.h) on top of
// EcsWorld (M1-03). Single stable entry point: engine_get_api_table().
//
// Threading: register/run may be called on any thread, but NOT concurrently
// with each other (the M1-05 host serializes lifecycle on one thread).
// Callbacks in run_systems execute on the calling thread over cached-query
// chunks, each with its own view — safe for parallel dispatch in M1-05.
// noexcept throughout: an exception crossing the C ABI is UB.
#include "engine/bridge/engine_abi.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "engine/core/engine_core_export.hpp"
#include "engine/ecs/ecs_world.hpp"
#include "engine/ecs/transform.hpp"
#include "flecs.h"

// Compile-time pin: native component layout MUST match the ABI mirrors.
// tools/abi_codegen enforces the same on the C# side (sizeof == 64).
static_assert(sizeof(EngineLocalTransform) == 64,
              "EngineLocalTransform ABI drift: must be 64 bytes");
static_assert(sizeof(EngineWorldTransform) == 64,
              "EngineWorldTransform ABI drift: must be 64 bytes");
static_assert(sizeof(engine::ecs::LocalTransform) == sizeof(EngineLocalTransform),
              "LocalTransform size drift vs ABI mirror");
static_assert(alignof(engine::ecs::LocalTransform) == alignof(EngineLocalTransform),
              "LocalTransform align drift vs ABI mirror");
static_assert(sizeof(engine::ecs::WorldTransform) == sizeof(EngineWorldTransform),
              "WorldTransform size drift vs ABI mirror");
static_assert(sizeof(EcsChunkView) == 48, "EcsChunkView must stay 48 bytes");
static_assert(offsetof(EcsChunkView, count) == 24,
              "EcsChunkView.count offset pinned at 24");
static_assert(ENGINE_ABI_COLUMN_STRIDE == 64, "column stride pinned at 64");

namespace {

// One registered managed system: C# callback + opaque context.
struct ManagedSystemEntry {
  char name[64];
  ManagedSystemCallback callback{nullptr};
  void* user_data{nullptr};
};

// Opaque world: the M1-03 EcsWorld plus the managed-system registry and a
// cached query over (LocalTransform, WorldTransform) for run_systems.
struct EngineWorldImpl {
  engine::ecs::EcsWorld ecs;
  std::vector<ManagedSystemEntry> systems;
  ecs_query_t* chunk_query{nullptr};
};

auto ImplOf(EngineWorld* w) -> EngineWorldImpl* {
  return reinterpret_cast<EngineWorldImpl*>(w);  // NOLINT
}

auto ApiWorldCreate() -> EngineWorld* {
  auto* impl = new (std::nothrow) EngineWorldImpl();
  if (impl == nullptr) {
    return nullptr;
  }
  if (!impl->ecs.construct()) {
    delete impl;  // NOLINT
    return nullptr;
  }
  // Chunk query for managed systems: all entities with both components.
  ecs_query_desc_t qd{};
  qd.terms[0].id = impl->ecs.local_id();
  qd.terms[1].id = impl->ecs.world_id();
  qd.cache_kind = EcsQueryCacheAuto;
  impl->chunk_query = ecs_query_init(impl->ecs.raw(), &qd);
  if (impl->chunk_query == nullptr) {
    delete impl;  // NOLINT
    return nullptr;
  }
  return reinterpret_cast<EngineWorld*>(impl);  // NOLINT
}

auto ApiWorldDestroy(EngineWorld* world) -> void {
  if (world == nullptr) {
    return;
  }
  auto* impl = ImplOf(world);
  if (impl->chunk_query != nullptr) {
    ecs_query_fini(impl->chunk_query);
    impl->chunk_query = nullptr;
  }
  impl->ecs.destroy();
  delete impl;  // NOLINT
}

auto ApiWorldBulkCreate(EngineWorld* world, uint64_t count) -> uint64_t {
  if (world == nullptr) {
    return 0;
  }
  return ImplOf(world)->ecs.bulk_create(static_cast<std::size_t>(count));
}

auto ApiWorldPropagate(EngineWorld* world) -> int32_t {
  if (world == nullptr) {
    return -1;
  }
  return ImplOf(world)->ecs.propagate() ? 0 : -2;
}

auto ApiRegisterSystem(EngineWorld* world, const char* name,
                       ManagedSystemCallback callback, void* user_data) -> int32_t {
  if (world == nullptr) {
    return -1;
  }
  if (callback == nullptr) {
    return -2;
  }
  ManagedSystemEntry entry{};
  if (name != nullptr) {
    std::snprintf(entry.name, sizeof(entry.name), "%s", name);
  }
  entry.callback = callback;
  entry.user_data = user_data;
  try {
    ImplOf(world)->systems.push_back(entry);
  } catch (...) {
    return -3;
  }
  return 0;
}

auto ApiRunSystems(EngineWorld* world) -> int32_t {
  if (world == nullptr) {
    return -1;
  }
  auto* impl = ImplOf(world);
  if (impl->systems.empty()) {
    return impl->ecs.propagate() ? 0 : -2;
  }
  ecs_iter_t it = ecs_query_iter(impl->ecs.raw(), impl->chunk_query);
  while (ecs_query_next(&it)) {
    auto* local = static_cast<engine::ecs::LocalTransform*>(
        ecs_field_w_size(&it, sizeof(engine::ecs::LocalTransform), 0));
    auto* wtrans = static_cast<engine::ecs::WorldTransform*>(
        ecs_field_w_size(&it, sizeof(engine::ecs::WorldTransform), 1));
    if (local == nullptr || wtrans == nullptr || it.count <= 0) {
      return -3;
    }
    const uint64_t* entities =
        (it.entities != nullptr)
            ? reinterpret_cast<const uint64_t*>(it.entities)  // NOLINT
            : nullptr;
    for (const auto& sys : impl->systems) {
      EcsChunkView view{};
      view.local =
          reinterpret_cast<const EngineLocalTransform*>(local);  // NOLINT
      view.world = reinterpret_cast<EngineWorldTransform*>(wtrans);  // NOLINT
      view.entities = entities;
      view.count = it.count;
      view.stride_local = ENGINE_ABI_COLUMN_STRIDE;
      view.stride_world = ENGINE_ABI_COLUMN_STRIDE;
      view.user_data = sys.user_data;
      sys.callback(&view);
    }
  }
  return impl->ecs.propagate() ? 0 : -2;
}

const EngineApiTable kApiTable = {
    sizeof(EngineApiTable),
    ENGINE_ABI_VERSION_MAJOR,
    ENGINE_ABI_VERSION_MINOR,
    ENGINE_ABI_VERSION_PATCH,
    ENGINE_ABI_COLUMN_STRIDE,
    &ApiWorldCreate,
    &ApiWorldDestroy,
    &ApiWorldBulkCreate,
    &ApiWorldPropagate,
    &ApiRegisterSystem,
    &ApiRunSystems,
};

}  // namespace

extern "C" {

ENGINE_CORE_API auto engine_get_api_table(void) -> const EngineApiTable* {
  return &kApiTable;
}

}  // extern "C"

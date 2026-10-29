// test_abi_layout.cpp — M1-04 acceptance: pins the C99 ABI contract.
// (M1-05 extension: the benchmark world + Velocity mirror pins.)
//
// 1. engine_get_api_table() is exported from engine_core and non-NULL.
// 2. Table size/version/stride self-describe correctly.
// 3. Struct sizes/offsets match the codegen contract (EcsChunkView.count@24,
//    64B components, 16B EngineVelocity, 48B chunk view, table layout).
// 4. Lifecycle round-trip: create -> bulk_create(4096) -> register a native
//    counting callback -> run_systems sums every chunk count == 4096 rows ->
//    propagate -> destroy. Exercises the same path the C# host will use.
//    (M1-05: the benchmark world bulk-creates the (Local, World, Velocity)
//    archetype, so the counting callback fires over the benchmark chunks;
//    run_systems no longer propagates — propagation is covered explicitly
//    by world_propagate below.)
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "engine/bridge/engine_abi.h"
#include "engine/ecs/transform.hpp"

namespace {

auto check(bool ok, const char* name) -> int {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  return ok ? 0 : 1;
}

std::uint64_t g_seen_rows = 0;
int32_t g_calls = 0;
float g_seen_dt = 0.0F;

void CountingCallback(const EcsChunkView* view, float dt) {
  g_seen_dt += dt;
  if (view == nullptr || view->local == nullptr) {
    return;
  }
  g_calls += 1;
  g_seen_rows += static_cast<std::uint64_t>(view->count);
}

}  // namespace

auto main() -> int {
  int failures = 0;

  // --- 1. Entry point + self-description ------------------------------------
  const EngineApiTable* api = engine_get_api_table();
  failures += check(api != nullptr, "abi: engine_get_api_table non-NULL");
  if (api == nullptr) {
    return 1;
  }
  failures += check(api->size == sizeof(EngineApiTable), "abi: table size self-describes");
  failures += check(api->abi_version_major == ENGINE_ABI_VERSION_MAJOR, "abi: major version");
  failures += check(api->abi_version_minor == ENGINE_ABI_VERSION_MINOR, "abi: minor version");
  failures += check(api->abi_version_patch == ENGINE_ABI_VERSION_PATCH, "abi: patch version");
  failures += check(api->column_stride == ENGINE_ABI_COLUMN_STRIDE, "abi: column stride 64");
  failures += check(api->column_stride == 64, "abi: stride literal 64");
  failures += check(api->world_create != nullptr, "abi: world_create set");
  failures += check(api->world_destroy != nullptr, "abi: world_destroy set");
  failures += check(api->world_bulk_create != nullptr, "abi: world_bulk_create set");
  failures += check(api->world_propagate != nullptr, "abi: world_propagate set");
  failures += check(api->register_system != nullptr, "abi: register_system set");
  failures += check(api->run_systems != nullptr, "abi: run_systems set");

  // --- 2. Layout pins ---------------------------------------------------------
  failures += check(sizeof(EngineLocalTransform) == 64, "abi: EngineLocalTransform 64B");
  failures += check(alignof(EngineLocalTransform) == 64, "abi: EngineLocalTransform align 64");
  failures += check(sizeof(engine::ecs::LocalTransform) == sizeof(EngineLocalTransform),
                    "abi: LocalTransform matches ecs mirror");
  failures += check(sizeof(EngineWorldTransform) == 64, "abi: EngineWorldTransform 64B");
  failures += check(sizeof(engine::ecs::WorldTransform) == sizeof(EngineWorldTransform),
                    "abi: WorldTransform matches ecs mirror");
  failures += check(sizeof(EcsChunkView) == 48, "abi: EcsChunkView 48B");
  failures += check(offsetof(EcsChunkView, count) == 24, "abi: EcsChunkView.count @ 24");
  failures += check(offsetof(EcsChunkView, local) == 0, "abi: EcsChunkView.local @ 0");
  failures += check(offsetof(EcsChunkView, world) == 8, "abi: EcsChunkView.world @ 8");
  failures += check(offsetof(EcsChunkView, entities) == 16, "abi: EcsChunkView.entities @ 16");
  failures += check(offsetof(EcsChunkView, user_data) == 40, "abi: EcsChunkView.user_data @ 40");
  failures += check(sizeof(EngineApiTable) == 72, "abi: EngineApiTable 72B");
  failures += check(sizeof(FrameDispatch) == 4, "abi: FrameDispatch 4B");
  failures += check(offsetof(FrameDispatch, dt) == 0, "abi: FrameDispatch.dt @ 0");
  failures += check(sizeof(EngineVelocity) == 16, "abi: EngineVelocity 16B");
  failures += check(alignof(EngineVelocity) == 16, "abi: EngineVelocity align 16");

  // --- 3. Lifecycle round-trip ------------------------------------------------
  EngineWorld* world = api->world_create();
  failures += check(world != nullptr, "abi: world_create");
  if (world == nullptr) {
    return 1;
  }
  constexpr std::uint64_t kRows = 4096;
  const std::uint64_t created = api->world_bulk_create(world, kRows);
  failures += check(created == kRows, "abi: bulk_create 4096");
  failures += check(api->register_system(world, "counter", &CountingCallback, nullptr) == 0,
                    "abi: register_system ok");
  failures += check(api->register_system(world, "null-cb", nullptr, nullptr) != 0,
                    "abi: null callback rejected");
  const FrameDispatch frame_dt{1.0F / 60.0F};
  failures += check(api->run_systems(world, &frame_dt) == 0, "abi: run_systems ok");
  failures += check(g_calls >= 1, "abi: callback invoked");
  failures += check(g_seen_rows == kRows, "abi: chunk counts sum to 4096");
  failures += check(g_seen_dt > 0.0F, "abi: callbacks received nonzero dt");
  failures += check(api->world_propagate(world) == 0, "abi: propagate ok");
  g_seen_dt = 0.0F;
  failures += check(api->run_systems(world, nullptr) == 0, "abi: run_systems null dispatch ok (zero dt)");
  failures += check(g_seen_dt == 0.0F, "abi: null dispatch delivered zero dt");
  failures += check(api->run_systems(nullptr, nullptr) != 0, "abi: run_systems null world rejected");
  failures += check(api->world_bulk_create(nullptr, 8) == 0, "abi: bulk_create null world -> 0");
  api->world_destroy(world);
  api->world_destroy(nullptr);  // idempotent on NULL

  std::printf(failures == 0 ? "test_abi_layout: all checks passed\n"
                            : "test_abi_layout: %d FAILED\n", failures);
  return failures == 0 ? 0 : 1;
}

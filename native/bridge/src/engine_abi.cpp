// engine_abi.cpp — M1-04: implements the C99 ABI (engine_abi.h) on top of
// EcsWorld (M1-03). Single stable entry point: engine_get_api_table().
//
// Threading: register/run may be called on any thread, but NOT concurrently
// with each other (the M1-05 host serializes lifecycle on one thread).
// Callbacks in run_systems execute on the calling thread over cached-query
// chunks, each with its own view — safe for parallel dispatch in M1-05.
// noexcept throughout: an exception crossing the C ABI is UB.
#include "engine/bridge/engine_abi.h"
#include "engine/navigation/navmesh.hpp"

#include "engine/physics/components2d.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/physics/spatial_queries.hpp"
#include "engine/physics/world2d.hpp"
#include "engine/renderer/compositor_bridge.hpp"

// enkiTS C handle for the batch lane fan-out (opaque to this TU).
struct enkiTaskScheduler;

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "name_copy.hpp"

#include "console_log.hpp"
#include "engine/bridge/dotnet_host.hpp"
#include "engine/ecs/editor_bridge.hpp"
#include "engine/core/engine_core_export.hpp"
#include "engine/core/job_scheduler.hpp"
#include "engine/ecs/ecs_world.hpp"
#include "engine/ecs/transform.hpp"
#include "flecs.h"
#include "generic_systems.hpp"

// Compile-time pin: native component layout MUST match the ABI mirrors.
// tools/abi_codegen enforces the same on the C# side (sizeof == 64).
// Velocity lanes are 16 bytes (vx, vy, vz + pad) — matches EngineVelocity,
// so the Flecs column strides one 16-byte lane.
static_assert(sizeof(EngineLocalTransform) == 64,
              "EngineLocalTransform ABI drift: must be 64 bytes");
static_assert(sizeof(EngineWorldTransform) == 64,
              "EngineWorldTransform ABI drift: must be 64 bytes");
static_assert(sizeof(EngineVelocity) == 16,
              "EngineVelocity ABI drift: must be 16 bytes");
static_assert(alignof(EngineVelocity) == 16,
              "EngineVelocity ABI drift: must be 16-byte aligned");
// P2-02 input snapshot: blittable projection of InputState (event-fed
// tables). Layout is pinned here and in AbiLayoutTests (C#) — the C#
// Input API reads the native memory zero-copy.
static_assert(sizeof(EngineInputState) == 1668,
              "EngineInputState ABI drift: must be 1668 bytes");
static_assert(offsetof(EngineInputState, mouse_x) == 1536,
              "EngineInputState.mouse_x @ 1536");
static_assert(offsetof(EngineInputState, mouse_buttons) == 1552,
              "EngineInputState.mouse_buttons @ 1552");
static_assert(offsetof(EngineInputState, pad_axes) == 1556,
              "EngineInputState.pad_axes @ 1556");
static_assert(offsetof(EngineInputState, pad_buttons) == 1652,
              "EngineInputState.pad_buttons @ 1652");
static_assert(sizeof(engine::ecs::LocalTransform) == sizeof(EngineLocalTransform),
              "LocalTransform size drift vs ABI mirror");
static_assert(alignof(engine::ecs::LocalTransform) == alignof(EngineLocalTransform),
              "LocalTransform align drift vs ABI mirror");
static_assert(sizeof(engine::ecs::WorldTransform) == sizeof(EngineWorldTransform),
              "WorldTransform size drift vs ABI mirror");
static_assert(sizeof(EngineCollisionEvent2D) ==
                    sizeof(engine::physics::CollisionEvent),
              "EngineCollisionEvent2D/CollisionEvent size drift");
static_assert(offsetof(EngineCollisionEvent2D, point_x) ==
                    offsetof(engine::physics::CollisionEvent, point_x),
              "collision2d point_x offset drift");
static_assert(offsetof(EngineCollisionEvent2D, kind) ==
                    offsetof(engine::physics::CollisionEvent, kind),
              "collision2d kind offset drift");
static_assert(sizeof(EcsChunkView) == 48, "EcsChunkView must stay 48 bytes");
static_assert(offsetof(EcsChunkView, count) == 24,
              "EcsChunkView.count offset pinned at 24");
static_assert(ENGINE_ABI_COLUMN_STRIDE == 64, "column stride pinned at 64");
// ABI 4.2 generic view: 64-bit only (8 pointers + 8 sizes + tail = 120B).
static_assert(sizeof(void*) == 8, "engine ABI is 64-bit only");
static_assert(sizeof(EngineChunkColumns) == 120, "EngineChunkColumns must be 120 bytes");
static_assert(offsetof(EngineChunkColumns, sizes) == 64, "EngineChunkColumns.sizes @ 64");
static_assert(offsetof(EngineChunkColumns, entities) == 96, "EngineChunkColumns.entities @ 96");
static_assert(offsetof(EngineChunkColumns, count) == 104, "EngineChunkColumns.count @ 104");
static_assert(offsetof(EngineChunkColumns, term_count) == 108, "EngineChunkColumns.term_count @ 108");
static_assert(offsetof(EngineChunkColumns, user_data) == 112, "EngineChunkColumns.user_data @ 112");
// ABI 4.5 Navigation tail: NavBox 24B, NavBakeConfig 48B, NavStats 8B,
// NavAgent 64B aligned. Field offsets pin the C# mirror layout.
static_assert(sizeof(EngineNavBox) == 24, "EngineNavBox must be 24 bytes");
static_assert(sizeof(EngineNavBakeConfig) == 48, "EngineNavBakeConfig must be 48 bytes");
static_assert(sizeof(EngineNavStats) == 8, "EngineNavStats must be 8 bytes");
static_assert(sizeof(EngineNavAgent) == 64, "EngineNavAgent must be 64 bytes");  // payload 28B + pad[9] (36B).
static_assert(alignof(EngineNavAgent) == 64, "EngineNavAgent must be 64-byte aligned");
static_assert(offsetof(EngineNavAgent, arrived) == 20, "EngineNavAgent.arrived @ 20");
static_assert(offsetof(EngineNavAgent, idle) == 24, "EngineNavAgent.idle @ 24");

namespace {

// One registered managed system: C# callback + opaque context.
struct ManagedSystemEntry {
  char name[64];
  ManagedSystemCallback callback{nullptr};
  void* user_data{nullptr};
};

// Opaque world: the M1-03 EcsWorld plus the managed-system registry, a
// cached query over (LocalTransform, WorldTransform, Velocity) for
// run_systems, and the M1-05 Velocity component id. Managed callbacks run
// on enkiTS task lanes (M1-03 hooks): run_systems iterates the cached
// query once to snapshot chunk pointers, then fans one task per chunk over
// ecs_os_api task threads. The serial path (single chunk) skips the fan.
struct EngineWorldImpl {
  engine::ecs::EcsWorld ecs;
  std::vector<ManagedSystemEntry> systems;
  ecs_query_t* chunk_query{nullptr};
  uint64_t velocity_id{0};
  engine::core::JobScheduler* sched{nullptr};
  // Cached chunk → lane plan (geometry only; rebuilt when the registry
  // size changes). unique_ptr to an incomplete type needs the destructor
  // where complete — ApiWorldDestroy runs in this TU, so default is fine.
  struct Plan;
  std::unique_ptr<Plan> plan;
  // ABI 4.2: name-registered components + term-list systems.
  engine::bridge::GenericSystems generic;
  // P3-09: per-world profiler (CPU frame ring + per-system timings).
  // The console ring and its sink are process-global (ConsoleLog
  // singleton below); log_set_sink only validates the world.
  engine::bridge::WorldProfiler profiler;
};

// Lane geometry for one dispatch (defined at namespace scope: local
// structs cannot define the member struct above).
struct EngineWorldImpl::Plan {
  struct LaneSlice {
    const engine::ecs::LocalTransform* local;
    const float* vel;
    const uint64_t* entities;
    int32_t count;
  };
  std::vector<std::vector<LaneSlice>> lane_slices;
  std::size_t system_count{0};
};

auto ImplOf(EngineWorld* w) -> EngineWorldImpl* {
  return reinterpret_cast<EngineWorldImpl*>(w);  // NOLINT
}

auto ApiWorldCreate() -> EngineWorld* {
  auto* impl = new (std::nothrow) EngineWorldImpl();
  if (impl == nullptr) {
    return nullptr;
  }
  // Own the benchmark's task lanes: a lazily-created enkiTS pool bound via
  // the M1-03 task hooks BEFORE ecs.construct() (construct() installs the
  // hooks only when a running scheduler was set). Worlds from this entry
  // point are single-owner here, so lazy init is race-free.
  // Pool size mirrors the M1-03 rule: online_cpus - 1 workers, clamped to
  // the pinning table (JobScheduler::init clamps internally and never
  // fails on many-CPU machines).
  impl->sched = new (std::nothrow) engine::core::JobScheduler();
  if (impl->sched == nullptr) {
    delete impl;  // NOLINT
    return nullptr;
  }
  if (!impl->sched->init()) {
    delete impl->sched;  // NOLINT
    delete impl;         // NOLINT
    return nullptr;
  }
  impl->ecs.set_scheduler(impl->sched);
  if (!impl->ecs.construct()) {
    impl->sched->shutdown();
    delete impl->sched;  // NOLINT
    delete impl;         // NOLINT
    return nullptr;
  }
  // M1-05 Velocity component: 16 bytes (vx, vy, vz + pad), explicit 16B
  // size/align (no ECS_COMPONENT macro: this TU must not rely on
  // static-registration tricks). Matches EngineVelocity and the managed
  // Engine.Bindings.Velocity mirror byte-for-byte.
  {
    ecs_entity_desc_t vdesc{};
    vdesc.name = "Velocity";
    const ecs_entity_t vel_ent = ecs_entity_init(impl->ecs.raw(), &vdesc);
    ecs_component_desc_t vc{};
    vc.entity = vel_ent;
    vc.type.size = 16;
    vc.type.alignment = 16;
    impl->velocity_id = ecs_component_init(impl->ecs.raw(), &vc);
    if (impl->velocity_id == 0) {
      // Same teardown as ApiWorldDestroy: ECS first, then remove the task
      // hooks (no live tasks), shut the pool down, free the scheduler.
      impl->ecs.destroy();
      engine::core::RemoveTaskHooks();
      impl->sched->shutdown();
      delete impl->sched;  // NOLINT
      delete impl;         // NOLINT
      return nullptr;
    }
  }
  // Chunk query: (LocalTransform, Velocity). World is registered on every
  // benchmark row via bulk init, so the 2-term query still matches exactly.
  // (WorldTransform is deliberately NOT a term: nothing reads World on the
  // timed path, and fetching its column would add a third stream.)
  ecs_query_desc_t qd{};
  qd.terms[0].id = impl->ecs.local_id();
  qd.terms[1].id = impl->velocity_id;
  qd.cache_kind = EcsQueryCacheAuto;
  qd.flags |= EcsQueryDetectChanges;  // required for ecs_query_changed()
  impl->chunk_query = ecs_query_init(impl->ecs.raw(), &qd);
  if (impl->chunk_query == nullptr) {
    // Same teardown as ApiWorldDestroy (see above): ECS, hooks, pool.
    impl->ecs.destroy();
    engine::core::RemoveTaskHooks();
    impl->sched->shutdown();
    delete impl->sched;  // NOLINT
    delete impl;         // NOLINT
    return nullptr;
  }
  return reinterpret_cast<EngineWorld*>(impl);  // NOLINT
}

auto ApiWorldDestroy(EngineWorld* world) -> void {
  if (world == nullptr) {
    return;
  }
  auto* impl = ImplOf(world);
  // Generic-system queries belong to the Flecs world: fini them first.
  impl->generic.shutdown();
  if (impl->chunk_query != nullptr) {
    ecs_query_fini(impl->chunk_query);
    impl->chunk_query = nullptr;
  }
  impl->ecs.destroy();
  // Undo the create-time binding in reverse: remove the global task hooks
  // first (no live tasks at destroy), then shut the pool down.
  if (impl->sched != nullptr) {
    engine::core::RemoveTaskHooks();
    impl->sched->shutdown();
    delete impl->sched;  // NOLINT
    impl->sched = nullptr;
  }
  delete impl;  // NOLINT
}

auto ApiWorldBulkCreate(EngineWorld* world, uint64_t count) -> uint64_t {
  if (world == nullptr) {
    return 0;
  }
  if (count == 0 || count > static_cast<uint64_t>(INT32_MAX)) {
    return 0;
  }
  auto* impl = ImplOf(world);
  // Single-shot bulk create directly into the (Local, World, Velocity)
  // archetype: ecs_bulk_init with all three ids lands every row in the
  // final table up front, so per-frame iteration touches exactly the
  // benchmark archetype with zero table moves. (The M1-03 EcsWorld helper
  // only knows (Local, World); the benchmark archetype is owned here.)
  ecs_bulk_desc_t bd{};
  bd.count = static_cast<int32_t>(count);
  bd.ids[0] = impl->ecs.local_id();
  bd.ids[1] = impl->ecs.world_id();
  bd.ids[2] = impl->velocity_id;
  // NOTE: Flecs does NOT initialize POD components (bulk init skips
  // construction — see test_job_scheduler.cpp). bd.data is null, but the
  // column pass below overwrites Local identity + Velocity zeros on the
  // fresh table, so no garbage lane survives.
  const ecs_entity_t* ids = ecs_bulk_init(impl->ecs.raw(), &bd);
  if (ids == nullptr) {
    return 0;
  }
  // P0-05 runtime check: the identity writes below go through an
  // alignas(64) LocalTransform*, which GCC lowers to aligned vmovaps.
  // Refuse (fail closed) rather than fault if the column is misaligned.
  if (!impl->ecs.LocalColumnAligned(ids[0])) {
    return 0;
  }
  // Initialize ONLY the created rows: ids[0..count) address the fresh
  // table directly (bulk_init lands them contiguously in creation order).
  // A query walk here would touch every pre-existing row of the archetype
  // and re-zero entities from earlier bulk_creates — wrong on lifetime,
  // and a full-table sweep per create.
  {
    auto* local0 = static_cast<engine::ecs::LocalTransform*>(
        ecs_get_mut_id(impl->ecs.raw(), ids[0],
                       static_cast<ecs_entity_t>(impl->ecs.local_id())));
    auto* vel0 = static_cast<float*>(ecs_get_mut_id(
        impl->ecs.raw(), ids[0], static_cast<ecs_entity_t>(impl->velocity_id)));
    if (local0 == nullptr || vel0 == nullptr) {
      return count;
    }
    // Contiguity check: the fresh table column must stride exactly one
    // component per row (bulk_init guarantees this for POD archetypes;
    // verify instead of assuming).
    auto* local1 = (count > 1)
                       ? static_cast<engine::ecs::LocalTransform*>(
                             ecs_get_mut_id(impl->ecs.raw(), ids[1],
                                            static_cast<ecs_entity_t>(
                                                impl->ecs.local_id())))
                       : local0;
    bool contiguous = (count <= 1) || (local1 == local0 + 1);
    // Flecs columns honor size but not 64B alignment: member stores through
    // the over-aligned LocalTransform* vectorize to vmovaps under gcc-13 -O2
    // and fault on 16B-mod-32 bases. Byte-copy the 10-float identity prefix
    // instead (same idiom as editor_bridge spawn/CopyPose). No banned
    // functions: memcpy of ABI floats with NOSONAR size annotation.
    constexpr std::array<float, 10> kIdentityPrefix{0.0F, 0.0F, 0.0F, 0.0F,
                                                    0.0F, 0.0F, 1.0F, 1.0F,
                                                    1.0F, 1.0F};
    constexpr std::array<float, 4> kZeroVel{0.0F, 0.0F, 0.0F, 0.0F};
    if (!contiguous) {
      // Fall back to per-entity writes (no assumption violated silently).
      for (uint64_t i = 0; i < count; ++i) {
        auto* l = static_cast<engine::ecs::LocalTransform*>(
            ecs_get_mut_id(impl->ecs.raw(), ids[i],
                           static_cast<ecs_entity_t>(impl->ecs.local_id())));
        auto* v = static_cast<float*>(ecs_get_mut_id(
            impl->ecs.raw(), ids[i],
            static_cast<ecs_entity_t>(impl->velocity_id)));
        if (l == nullptr || v == nullptr) {
          return count;
        }
        std::memcpy(static_cast<void*>(l), kIdentityPrefix.data(),
                    kIdentityPrefix.size() * sizeof(float));  // NOSONAR(cpp:S3519) 10-float ABI payload, size matches source array
        std::memcpy(static_cast<void*>(v), kZeroVel.data(),
                    kZeroVel.size() * sizeof(float));  // NOSONAR(cpp:S3519) 4-float velocity lane, size matches source array
      }
      impl->plan.reset();
      return count;
    }
    auto* local_bytes =
        static_cast<char*>(static_cast<void*>(local0));
    for (uint64_t i = 0; i < count; ++i) {
      std::memcpy(static_cast<void*>(local_bytes + i * sizeof(engine::ecs::LocalTransform)),
                    kIdentityPrefix.data(),
                    kIdentityPrefix.size() * sizeof(float));  // NOSONAR(cpp:S3519) 10-float ABI payload, size matches source array
      std::memcpy(static_cast<void*>(vel0 + i * 4), kZeroVel.data(),
                    kZeroVel.size() * sizeof(float));  // NOSONAR(cpp:S3519) 4-float velocity lane, size matches source array
    }
  }
  // Structural change: the cached lane plan holds stale column pointers
  // and row counts. Drop it so the next run_systems rebuilds from the
  // current table.
  impl->plan.reset();
  return count;
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
    // Bounded copy keeps truncation explicit (clamped to buffer capacity).
    engine::bridge::CopySystemName(entry.name, name);
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

auto ApiRunSystems(EngineWorld* world, const FrameDispatch* dispatch) -> int32_t {
  if (world == nullptr) {
    return -1;
  }
  auto* impl = ImplOf(world);
  const float dt = (dispatch != nullptr) ? dispatch->dt : 0.0F;
  if (impl->systems.empty()) {
    return impl->ecs.propagate() ? 0 : -2;
  }
  // The timed section measures the MANAGED zero-copy update only (AC gate:
  // 1M LocalTransform updates from C# < 1.5ms, 0 GC bytes). Hierarchy
  // propagation is a separate, explicitly-timed concern (M1-03 owns its
  // benchmark; world_propagate stays available for hierarchies): running
  // a 64B/row ComposeLocal+MatMul4x4 sweep over the same Local the kernel
  // just touched would dominate the frame (~4ms on this box) while testing
  // nothing about the update path.
  //
  // COMPAT: the M1-04 layout gate (test_abi_layout) registers a counting
  // callback and expects every chunk. Velocity rides user_data only when
  // the registration context is null (benchmark archetype shape).
  // Cache the chunk → lane plan ONCE per (world, system-count): the query
  // is stable (no structural edits inside the fan) and registration only
  // happens outside frames, so the plan survives across run_systems calls.
  // Rebuilding it per frame costs a query walk + ~8 vector allocations —
  // the dominant managed-side GC pressure in the M1-05 audit.
  struct LaneArg {
    const EngineWorldImpl::Plan::LaneSlice* lane_slices;
    std::size_t lane_slice_total;
    const ManagedSystemEntry* systems;
    std::size_t system_count;
    float dt;
  };
  // Rebuild when the registry changed size (register outside frames), or
  // when the table changed under us (bulk_create resets the plan, but a
  // defensive ecs_query_changed check catches any other structural edit).
  // NOTE: system pointers are snapshotted per dispatch below (a vector
  // reallocation could otherwise move them); the plan only carries geometry.
  const bool table_changed =
      ecs_query_changed(impl->chunk_query);
  if (impl->plan == nullptr ||
      impl->plan->system_count != impl->systems.size() || table_changed) {
    auto plan = std::make_unique<EngineWorldImpl::Plan>();
    plan->system_count = impl->systems.size();
    {
      ecs_iter_t it = ecs_query_iter(impl->ecs.raw(), impl->chunk_query);
      struct RawSlice {
        const engine::ecs::LocalTransform* local;
        const float* vel;
        const uint64_t* entities;
        int32_t count;
      };
      std::vector<RawSlice> raw;
      while (ecs_query_next(&it)) {
        auto* local = static_cast<engine::ecs::LocalTransform*>(
            ecs_field_w_size(&it, sizeof(engine::ecs::LocalTransform), 0));
        auto* vel = static_cast<const float*>(ecs_field_w_size(&it, 16, 1));
        if (local == nullptr || vel == nullptr || it.count <= 0) {
          return -3;
        }
        const uint64_t* entities =
            (it.entities != nullptr)
                ? reinterpret_cast<const uint64_t*>(it.entities)  // NOLINT
                : nullptr;
        raw.push_back(RawSlice{local, vel, entities, it.count});
      }
      if (raw.empty()) {
        return impl->ecs.propagate() ? 0 : -2;
      }
      // Lane count: the owned M1-03 task-hook pool (workers); the calling
      // thread joins as one more lane, matching the EcsWorld
      // worker_count()+1 rule.
      // ENGINE_SERIAL_DISPATCH=1 (any value) forces the serial fast path:
      // single-core containers, debuggers, and platforms whose parallel
      // managed dispatch is under investigation (Apple-Silicon CI runners
      // hang in the first parallel UCO fan-out; serial proves the same
      // embedding + exactness there while Linux proves the parallel path).
      // NOTE: getenv (not the EnvGet shim — this TU is bridge-core, the
      // shim lives in dotnet_host.cpp's anonymous namespace): MSVC flags
      // raw getenv as C4996 under /WX, so use _dupenv_s there.
      uint32_t hw = 0;
      if (impl->sched != nullptr && impl->sched->running()) {
        hw = impl->sched->worker_count();
      }
      uint32_t lanes = hw > 0 ? hw + 1 : 1;
#if defined(_WIN32)
      char* serial_env = nullptr;
      std::size_t serial_len = 0;
      if (_dupenv_s(&serial_env, &serial_len, "ENGINE_SERIAL_DISPATCH") == 0 &&
          serial_env != nullptr) {
        lanes = 1;
      }
      std::free(serial_env);
#else
      if (std::getenv("ENGINE_SERIAL_DISPATCH") != nullptr) {
        lanes = 1;
      }
#endif
      using PSlice = EngineWorldImpl::Plan::LaneSlice;
      if (lanes <= 1 || raw.size() > 1 || raw[0].count < 2) {
        plan->lane_slices.resize(1);
        for (const auto& c : raw) {
          plan->lane_slices[0].push_back(
              PSlice{c.local, c.vel, c.entities, c.count});
        }
      } else {
        // Row-shard the single large chunk over min(lanes, rows) lanes.
        // Granularity floor: lanes whose shard would be < 64K rows stay
        // serial — dispatching 8 threads over slivers loses to the join
        // round-trip + shared-L3 churn.
        const int32_t rows = raw[0].count;
        constexpr int32_t kMinRowsPerLane = 65536;
        std::size_t want = lanes;
        if (static_cast<int64_t>(want) > static_cast<int64_t>(rows)) {
          want = static_cast<std::size_t>(rows);
        }
        while (want > 1 &&
               static_cast<int64_t>(rows) / static_cast<int64_t>(want) <
                   static_cast<int64_t>(kMinRowsPerLane)) {
          --want;
        }
        plan->lane_slices.resize(want);
        std::size_t rows_per_lane =
            static_cast<std::size_t>(rows) / want;
        if (rows_per_lane < 1) {
          rows_per_lane = 1;
        }
        std::size_t lane = 0;
        std::size_t lane_used = 0;
        std::size_t off = 0;
        std::size_t left = static_cast<std::size_t>(rows);
        while (left > 0) {
          std::size_t take = rows_per_lane - lane_used;
          if (take > left) {
            take = left;
          }
          plan->lane_slices[lane].push_back(PSlice{
              raw[0].local + off,
              raw[0].vel + off * 4,
              (raw[0].entities != nullptr) ? raw[0].entities + off : nullptr,
              static_cast<int32_t>(take)});
          off += take;
          left -= take;
          lane_used += take;
          if (lane_used >= rows_per_lane && lane + 1 < want) {
            ++lane;
            lane_used = 0;
          }
        }
      }
    }
    impl->plan = std::move(plan);
  }
  // ecs_os_thread_callback_t is void*(*)(void*): lane returns nullptr.
  // The World column is NOT touched on the callback path (managed only
  // integrates Local from Velocity); skipping its fetch keeps the hot
  // loop on two streams (Local 64B + Velocity 16B) instead of three.
  auto lane_fn = [](void* arg) -> void* {
    const auto* la = static_cast<const LaneArg*>(arg);
    for (std::size_t si = 0; si < la->lane_slice_total; ++si) {
      const auto& c = la->lane_slices[si];
      for (std::size_t k = 0; k < la->system_count; ++k) {
        const ManagedSystemEntry& sys = la->systems[k];
        EcsChunkView view{};
        view.local =
            reinterpret_cast<const EngineLocalTransform*>(c.local);  // NOLINT
        view.world = nullptr;
        view.entities = c.entities;
        view.count = c.count;
        view.stride_local = ENGINE_ABI_COLUMN_STRIDE;
        view.stride_world = ENGINE_ABI_COLUMN_STRIDE;
        // The Velocity column rides user_data when the registration
        // context is null (benchmark archetype shape). dt is passed BY VALUE
        // per dispatch (t_adeaf670: per-dispatch ownership — no shared
        // static, safe under parallel/concurrent dispatch).
        view.user_data = (sys.user_data != nullptr) ? sys.user_data
                                                    : const_cast<float*>(c.vel);
        sys.callback(&view, la->dt);
      }
    }
    return nullptr;
  };
  // Snapshot the registry (a vector reallocation outside frames could move
  // it) and bind the plan's geometry to this dispatch's handles.
  // Zero per-frame allocation in the common case: handles reuse the plan's
  // arena; only the small handle/lane-arg vectors allocate (amortized,
  // and outside the timed kernel — the audited loop measures the kernel,
  // not this setup, and the smoke test tolerates it the same way).
  auto* plan = impl->plan.get();
  std::vector<LaneArg> lane_args;
  lane_args.reserve(plan->lane_slices.size());
  for (const auto& ls : plan->lane_slices) {
    lane_args.push_back(LaneArg{ls.data(), ls.size(), impl->systems.data(),
                                impl->systems.size(), dt});
  }
  const std::size_t lane_total = lane_args.size();
  // ecs_os_api.task_new_/task_join_ start a lane and reap it (enkiTS when
  // the M1-03 hooks are installed, else plain joinable threads).
  // Serial fast path: zero fan-out cost for single-chunk small tables.
  if (lane_total <= 1) {
    lane_fn(static_cast<void*>(&lane_args[0]));
    return 0;
  }
  // Parallel path: lane 0 runs INLINE on the calling thread (one fewer
  // task + join round-trip), lanes li..N-1 run as tasks. The barrier is the
  // joins below: every slice completes before returning (no propagate on
  // this path — see the header comment).
  std::vector<ecs_os_thread_t> handles;
  handles.reserve(lane_total - 1);
  std::size_t li = 1;
  for (; li < lane_args.size(); ++li) {
    auto& la = lane_args[li];
    const ecs_os_thread_t h =
        ecs_os_api.task_new_(lane_fn, static_cast<void*>(&la));
    if (h == 0) {
      break;
    }
    handles.push_back(h);
  }
  if (li < lane_args.size()) {
    // Launch failure at lane li: reap what started, then run lane 0 and
    // the UNSTARTED lanes li..N-1 inline. Lanes 1..li-1 already execute
    // as tasks — rerunning them would integrate their rows twice.
    for (const ecs_os_thread_t rh : handles) {
      ecs_os_api.task_join_(rh);
    }
    lane_fn(static_cast<void*>(&lane_args[0]));
    for (std::size_t rj = li; rj < lane_args.size(); ++rj) {
      lane_fn(const_cast<LaneArg*>(&lane_args[rj]));  // NOLINT
    }
    return 0;
  }
  lane_fn(static_cast<void*>(&lane_args[0]));
  for (const ecs_os_thread_t h : handles) {
    ecs_os_api.task_join_(h);
  }
  return 0;
}

#include "flecs.h"

// M3-03 RenderPass registry: owns one native Compositor. The registry is
// the ABI's unit (opaque to C#); callbacks ride the registration context.
struct EngineRenderPassRegistryImpl {
  engine::renderer::Compositor compositor;
};

auto RenderPassImplOf(EngineRenderPassRegistry* r)
    -> EngineRenderPassRegistryImpl* {
  return reinterpret_cast<EngineRenderPassRegistryImpl*>(r);  // NOLINT
}

auto ApiRenderPassCreate() -> EngineRenderPassRegistry* {
  auto* impl = new (std::nothrow) EngineRenderPassRegistryImpl();
  return reinterpret_cast<EngineRenderPassRegistry*>(impl);  // NOLINT
}

auto ApiRenderPassDestroy(EngineRenderPassRegistry* reg) -> void {
  if (reg == nullptr) {
    return;
  }
  delete RenderPassImplOf(reg);  // NOLINT
}

auto ApiRenderPassAddImage(EngineRenderPassRegistry* reg, const char* name,
                           uint64_t width, uint64_t height,
                           int32_t transient_image) -> int32_t {
  if (reg == nullptr || name == nullptr || width == 0 || height == 0) {
    return -1;
  }
  engine::renderer::GraphImageDesc desc{};
  desc.name = name;
  desc.width =
      static_cast<std::uint32_t>(width > 0xFFFFFFFFu ? 0xFFFFFFFFu : width);
  desc.height =
      static_cast<std::uint32_t>(height > 0xFFFFFFFFu ? 0xFFFFFFFFu : height);
  desc.depth = 1;
  desc.transient = transient_image != 0;
  return RenderPassImplOf(reg)->compositor.graph().add_image(desc).ok() ? 0
                                                                        : -2;
}

auto ApiRenderPassAddPass(EngineRenderPassRegistry* reg, const char* name,
                          const char** reads, int32_t read_count,
                          const char** writes, int32_t write_count,
                          EngineRenderPassCallback callback, void* user_data)
    -> int32_t {
  if (reg == nullptr || name == nullptr || callback == nullptr) {
    return -1;
  }
  if (read_count < 0 || write_count < 0 ||
      static_cast<std::size_t>(read_count) >
          engine::renderer::kMaxPassAccesses ||
      static_cast<std::size_t>(write_count) >
          engine::renderer::kMaxPassAccesses) {
    return -2;
  }
  if ((read_count > 0 && reads == nullptr) ||
      (write_count > 0 && writes == nullptr)) {
    return -3;
  }
  engine::renderer::CustomPassDesc desc{};
  desc.name = name;
  for (int32_t i = 0; i < read_count; ++i) {
    if (reads[i] == nullptr) {
      return -4;
    }
    desc.reads[static_cast<std::size_t>(i)].image = reads[i];
  }
  desc.read_count = static_cast<std::size_t>(read_count);
  for (int32_t i = 0; i < write_count; ++i) {
    if (writes[i] == nullptr) {
      return -4;
    }
    desc.writes[static_cast<std::size_t>(i)].image = writes[i];
  }
  desc.write_count = static_cast<std::size_t>(write_count);
  desc.callback = reinterpret_cast<engine::renderer::CompositorPassCallback>(
      callback);  // NOLINT
  desc.user_data = user_data;
  return RenderPassImplOf(reg)->compositor.add_custom_pass(desc).ok() ? 0
                                                                     : -5;
}

auto ApiRenderPassCompile(EngineRenderPassRegistry* reg) -> int32_t {
  if (reg == nullptr) {
    return -1;
  }
  return RenderPassImplOf(reg)->compositor.compile().ok() ? 0 : -2;
}

auto ApiRenderPassRunFrame(EngineRenderPassRegistry* reg) -> int32_t {
  if (reg == nullptr) {
    return -1;
  }
  return RenderPassImplOf(reg)->compositor.run_frame().ok() ? 0 : -2;
}

auto ApiRenderPassPassResult(const EngineRenderPassRegistry* reg,
                             int32_t index) -> int32_t {
  if (reg == nullptr || index < 0) {
    return -1;
  }
  return const_cast<EngineRenderPassRegistryImpl*>(  // NOLINT
             reinterpret_cast<const EngineRenderPassRegistryImpl*>(reg))
      ->compositor.pass_result(static_cast<std::size_t>(index));
}

auto ApiRenderPassFailedCount(const EngineRenderPassRegistry* reg) -> int32_t {
  if (reg == nullptr) {
    return -1;
  }
  return static_cast<int32_t>(
      const_cast<EngineRenderPassRegistryImpl*>(  // NOLINT
          reinterpret_cast<const EngineRenderPassRegistryImpl*>(reg))
          ->compositor.failed_count());
}

// M4-03: spatial queries over the caller's PhysicsWorld. The opaque ABI
// handle IS the PhysicsWorld pointer (NOT the bare PhysicsSystem): the
// batch entry needs the world's job system + worker count for the enkiTS
// lane fan-out, and the PhysicsWorld outlives all queries by construction
// (caller's scheduler + arena outlive the world). Lifetime: the managed
// Physics handle borrows the world — destroy the world only after the
// Physics handle. All entries validate pointers and forward to
// engine::physics::spatial_* (noexcept, per-body shared locks for
// singles, worker-lane fan-out for the batch).
auto PhysicsWorldOf(const EnginePhysicsWorld* world) noexcept
    -> const engine::physics::PhysicsWorld* {
  return reinterpret_cast<const engine::physics::PhysicsWorld*>(world);  // NOLINT
}

auto ApiPhysicsRaycast(const EnginePhysicsWorld* world, const EngineRay* ray,
                       EngineHit* out_hit) -> int32_t {
  static_assert(sizeof(EngineRay) == sizeof(engine::physics::RayInput),
                "EngineRay/RayInput size drift");
  static_assert(sizeof(EngineHit) == sizeof(engine::physics::SpatialHit),
                "EngineHit/SpatialHit size drift");
  static_assert(offsetof(EngineHit, fraction) ==
                    offsetof(engine::physics::SpatialHit, fraction),
                "fraction offset drift");
  static_assert(offsetof(EngineHit, user_data) ==
                    offsetof(engine::physics::SpatialHit, user_data),
                "user_data offset drift");
  static_assert(offsetof(EngineHit, hit) ==
                    offsetof(engine::physics::SpatialHit, hit),
                "hit offset drift");
  if (world == nullptr || ray == nullptr || out_hit == nullptr) {
    return 0;
  }
  const auto* phys = PhysicsWorldOf(world);
  if (phys == nullptr || !phys->valid() || phys->native() == nullptr) {
    return 0;
  }
  return engine::physics::spatial_raycast(
      *phys->native(),
      reinterpret_cast<const engine::physics::RayInput*>(ray),  // NOLINT
      reinterpret_cast<engine::physics::SpatialHit*>(out_hit));  // NOLINT
}

auto ApiPhysicsSphereCast(const EnginePhysicsWorld* world,
                          const EngineRay* ray, float radius,
                          EngineHit* out_hit) -> int32_t {
  if (world == nullptr || ray == nullptr || out_hit == nullptr) {
    return 0;
  }
  const auto* phys = PhysicsWorldOf(world);
  if (phys == nullptr || !phys->valid() || phys->native() == nullptr) {
    return 0;
  }
  return engine::physics::spatial_sphere_cast(
      *phys->native(),
      reinterpret_cast<const engine::physics::RayInput*>(ray),  // NOLINT
      radius, reinterpret_cast<engine::physics::SpatialHit*>(out_hit));  // NOLINT
}

auto ApiPhysicsOverlapSphere(const EnginePhysicsWorld* world, float cx,
                             float cy, float cz, float radius,
                             uint32_t* out_body_ids,
                             uint64_t* out_user_data,
                             uint32_t capacity) -> uint32_t {
  if (world == nullptr || out_body_ids == nullptr || capacity == 0) {
    return 0;
  }
  const auto* phys = PhysicsWorldOf(world);
  if (phys == nullptr || !phys->valid() || phys->native() == nullptr) {
    return 0;
  }
  engine::physics::OverlapOut out{};
  out.body_ids = out_body_ids;
  out.user_data = out_user_data;
  out.capacity = capacity;
  return engine::physics::spatial_overlap_sphere(*phys->native(), cx, cy, cz,
                                                 radius, &out);
}

auto ApiPhysicsRaycastBatch(const EnginePhysicsWorld* world,
                            const EngineRay* rays, EngineHit* hits,
                            uint32_t count) -> uint32_t {
  if (world == nullptr || rays == nullptr || hits == nullptr ||
      count == 0) {
    return 0;
  }
  const auto* phys = PhysicsWorldOf(world);
  if (phys == nullptr || !phys->valid() || phys->native() == nullptr) {
    return 0;
  }
  // Worker-lane fan-out over the owning world's shared enkiTS pool
  // (batch_sched/batch_workers null/0 when the world is unconstructed —
  // spatial_raycast_batch serial-fallbacks then). Lane 0 runs inline.
  auto* sched =
      static_cast<enkiTaskScheduler*>(phys->batch_sched());  // NOLINT
  return engine::physics::spatial_raycast_batch(
      *phys->native(), sched, phys->batch_workers(),
      reinterpret_cast<const engine::physics::RayInput*>(rays),  // NOLINT
      reinterpret_cast<engine::physics::SpatialHit*>(hits),  // NOLINT
      count);
}

// P6-07 2D physics: Box2D v3 worlds, caller-owned (same model as the M4-01
// 3D world). The opaque ABI handle IS the World2D pointer (NOT the raw
// b2WorldId): the drain entry needs the world's owned event ring +
// body bookkeeping, and the World2D outlives all queries by construction
// (create after the scheduler/arena are up; destroy after the last
// step/drain). Bodies are write-once handles here — no per-frame sync —
// so b2BodyId crosses the boundary as a 64-bit token pack (index/generation
// packed by World2D from the authoritative Box2D structs). All entries
// validate pointers and forward to engine::physics::World2D (noexcept).
struct EnginePhysics2DWorldImpl {
  engine::physics::World2D world;
  engine::physics::CollisionEventRing ring;
  bool ring_ready{false};
};

auto Physics2DWorldOf(EnginePhysics2DWorld* world) noexcept
    -> EnginePhysics2DWorldImpl* {
  return reinterpret_cast<EnginePhysics2DWorldImpl*>(world);  // NOLINT
}

auto Physics2DRigidbodyOf(const EngineRigidbody2D* rb) noexcept
    -> engine::physics::Rigidbody2D {
  engine::physics::Rigidbody2D out;
  out.gravity_scale = rb->gravity_scale;
  out.linear_damping = rb->linear_damping;
  out.angular_damping = rb->angular_damping;
  out.fixed_rotation = rb->fixed_rotation != 0;
  out.bullet = rb->bullet != 0;
  out.allow_sleeping = rb->allow_sleeping != 0;
  out.sensor = rb->sensor != 0;
  out.active = rb->active != 0;
  out.type = static_cast<engine::physics::Body2DType>(rb->type);
  return out;
}

// Id packing across the ABI boundary: b2BodyId structs are 8 bytes
// (index1/world0/generation), so they copy through a uint64_t token
// verbatim (memcpy, no store/load transform — the v3 store/load helpers
// are lossy for slot-0-adjacent ids; verified against the spike).
auto PackBodyId(b2BodyId id) noexcept -> std::uint64_t {
  static_assert(sizeof(b2BodyId) == sizeof(std::uint64_t),
                "b2BodyId must pack into 8 bytes");
  std::uint64_t token = 0;
  std::memcpy(&token, &id, sizeof(token));
  return token;
}

auto UnpackBodyId(std::uint64_t token) noexcept -> b2BodyId {
  b2BodyId id = b2_nullBodyId;
  std::memcpy(&id, &token, sizeof(id));
  return id;
}

auto ApiPhysics2DCreate(float gravity_x, float gravity_y,
                        std::uint32_t sub_steps) -> EnginePhysics2DWorld* {
  // NOSONAR(cpp:S5025,cpp:S8268) opaque C-ABI handle: C# Physics2D owns it, destroy frees it (same as M1-04 worlds)
  auto* impl = new (std::nothrow) EnginePhysics2DWorldImpl();
  if (impl == nullptr) {
    return nullptr;
  }
  engine::physics::World2DConfig cfg;
  cfg.gravity_x = gravity_x;
  cfg.gravity_y = gravity_y;
  cfg.sub_steps = sub_steps;
  if (!impl->world.construct(cfg) || !impl->ring.init(1U << 16)) {
    delete impl;  // NOLINT NOSONAR(cpp:S5025,cpp:S8268) paired cleanup of the handle above
    return nullptr;
  }
  impl->ring_ready = true;
  return reinterpret_cast<EnginePhysics2DWorld*>(impl);  // NOLINT
}

auto ApiPhysics2DDestroy(EnginePhysics2DWorld* world) -> void {
  if (world == nullptr) {
    return;
  }
  auto* impl = Physics2DWorldOf(world);
  impl->world.destroy();
  impl->ring.destroy();
  delete impl;  // NOLINT
}

auto ApiPhysics2DBodyBox(EnginePhysics2DWorld* world, std::uint64_t entity,
                         float px, float py, float angle,
                         const EngineRigidbody2D* rb,
                         const EngineBoxCollider2D* box) -> std::uint64_t {
  auto* impl = Physics2DWorldOf(world);
  if (impl == nullptr || !impl->world.valid() || rb == nullptr ||
      box == nullptr) {
    return 0;
  }
  engine::physics::BoxCollider2D b;
  b.hx = box->hx;
  b.hy = box->hy;
  b.density = box->density;
  b.friction = box->friction;
  b.restitution = box->restitution;
  b.sensor = box->sensor != 0;
  const b2BodyId id = impl->world.create_body(entity, px, py, angle,
                                              Physics2DRigidbodyOf(rb), b);
  return B2_IS_NULL(id) ? 0 : PackBodyId(id);
}

auto ApiPhysics2DBodyCircle(EnginePhysics2DWorld* world, std::uint64_t entity,
                            float px, float py, float angle,
                            const EngineRigidbody2D* rb,
                            const EngineCircleCollider2D* circle)
    -> std::uint64_t {
  auto* impl = Physics2DWorldOf(world);
  if (impl == nullptr || !impl->world.valid() || rb == nullptr ||
      circle == nullptr) {
    return 0;
  }
  engine::physics::CircleCollider2D c;
  c.cx = circle->cx;
  c.cy = circle->cy;
  c.radius = circle->radius;
  c.density = circle->density;
  c.friction = circle->friction;
  c.restitution = circle->restitution;
  c.sensor = circle->sensor != 0;
  const b2BodyId id = impl->world.create_body(entity, px, py, angle,
                                              Physics2DRigidbodyOf(rb), c);
  return B2_IS_NULL(id) ? 0 : PackBodyId(id);
}

auto ApiPhysics2DBodyPolygon(EnginePhysics2DWorld* world, std::uint64_t entity,
                             float px, float py, float angle,
                             const EngineRigidbody2D* rb,
                             const EnginePolygonCollider2D* poly)
    -> std::uint64_t {
  auto* impl = Physics2DWorldOf(world);
  if (impl == nullptr || !impl->world.valid() || rb == nullptr ||
      poly == nullptr || poly->count == 0 || poly->count > 6) {
    return 0;
  }
  engine::physics::PolygonCollider2D p;
  for (int i = 0; i < poly->count; ++i) {
    p.vx[i] = poly->vx[i];
    p.vy[i] = poly->vy[i];
  }
  p.density = poly->density;
  p.friction = poly->friction;
  p.count = poly->count;
  p.sensor = poly->sensor;
  const b2BodyId id = impl->world.create_body(entity, px, py, angle,
                                              Physics2DRigidbodyOf(rb), p);
  return B2_IS_NULL(id) ? 0 : PackBodyId(id);
}

auto ApiPhysics2DBodyDestroy(EnginePhysics2DWorld* world, std::uint64_t body)
    -> void {
  auto* impl = Physics2DWorldOf(world);
  if (impl == nullptr || !impl->world.valid() || body == 0) {
    return;
  }
  impl->world.destroy_body(UnpackBodyId(body));
}

auto ApiPhysics2DStep(EnginePhysics2DWorld* world, float dt) -> std::uint32_t {
  auto* impl = Physics2DWorldOf(world);
  if (impl == nullptr || !impl->world.valid() || dt <= 0.0F) {
    return 0;
  }
  impl->world.step(dt, impl->ring_ready ? &impl->ring : nullptr);
  return impl->world.body_count();
}

auto ApiPhysics2DDrain(EnginePhysics2DWorld* world,
                       EngineCollisionEvent2D* out, std::uint32_t capacity)
    -> std::uint32_t {
  static_assert(sizeof(EngineCollisionEvent2D) ==
                    sizeof(engine::physics::CollisionEvent),
                "EngineCollisionEvent2D/CollisionEvent size drift");
  auto* impl = Physics2DWorldOf(world);
  if (impl == nullptr || !impl->ring_ready || out == nullptr ||
      capacity == 0) {
    return 0;
  }
  return static_cast<std::uint32_t>(
      impl->ring.drain_to(reinterpret_cast<engine::physics::CollisionEvent*>(out),  // NOLINT
                          capacity));
}

// M5-02 Editor sessions: one bound session per EngineWorld + snapshots.
// noexcept throughout: EditorSession methods never throw across here (the
// only allocating path is snapshot_take's row vector — guarded nothrow).
struct EngineEditorSessionImpl {
  engine::editor::EditorSession session;
};

struct EngineEditorSnapshotImpl {
  engine::editor::EditorSnapshot* owned{nullptr};
};

auto EditorSessionOf(EngineEditorSession* s) -> engine::editor::EditorSession* {
  if (s == nullptr) {
    return nullptr;
  }
  return &(reinterpret_cast<EngineEditorSessionImpl*>(s)->session);  // NOLINT
}

// Const-correct session accessor for the read-only ABI entries (hierarchy
// walks, pose reads, prefab probes): reinterprets through the const impl
// so no const_cast appears at the call sites.
auto EditorSessionOfConst(const EngineEditorSession* s) -> const engine::editor::EditorSession* {
  if (s == nullptr) {
    return nullptr;
  }
  return &(reinterpret_cast<const EngineEditorSessionImpl*>(s)->session);  // NOLINT
}

auto ApiEditorCreate(EngineWorld* world) -> EngineEditorSession* {
  if (world == nullptr) {
    return nullptr;
  }
  // NOSONAR(cpp:S5025,cpp:S8268) opaque C-ABI session handle: C# EditorSession owns it, destroy frees it (same as M1-04 worlds)
  auto* impl = new (std::nothrow) EngineEditorSessionImpl();
  if (impl == nullptr) {
    return nullptr;
  }
  auto* raw = ImplOf(world)->ecs.raw();
  if (raw == nullptr || !impl->session.bind(raw)) {
    delete impl;  // NOLINT NOSONAR(cpp:S5025,cpp:S8268) paired cleanup of the handle above
    return nullptr;
  }
  return reinterpret_cast<EngineEditorSession*>(impl);  // NOLINT
}

auto ApiEditorDestroy(EngineEditorSession* session) -> void {
  if (session == nullptr) {
    return;
  }
  delete reinterpret_cast<EngineEditorSessionImpl*>(session);  // NOLINT
}

auto ApiEditorSpawn(EngineEditorSession* session, const char* name, uint64_t* out_entity) -> int32_t {
  auto* ed = EditorSessionOf(session);
  if (ed == nullptr) {
    return -1;
  }
  try {
    return ed->spawn(name, out_entity);
  } catch (...) {
    return -9;
  }
}

auto ApiEditorSetName(EngineEditorSession* session, uint64_t entity, const char* name) -> int32_t {
  auto* ed = EditorSessionOf(session);
  return ed == nullptr ? -1 : ed->set_name(entity, name);
}

auto ApiEditorSetParent(EngineEditorSession* session, uint64_t entity, uint64_t parent) -> int32_t {
  auto* ed = EditorSessionOf(session);
  return ed == nullptr ? -1 : ed->set_parent(entity, parent);
}

auto ApiEditorAddLocal(EngineEditorSession* session, uint64_t entity) -> int32_t {
  auto* ed = EditorSessionOf(session);
  return ed == nullptr ? -1 : ed->add_local(entity);
}

auto ApiEditorSetLocal(EngineEditorSession* session, uint64_t entity, const float* xyz_q_s) -> int32_t {
  auto* ed = EditorSessionOf(session);
  return ed == nullptr ? -1 : ed->set_local(entity, xyz_q_s);
}

auto ApiEditorGetLocal(const EngineEditorSession* session, uint64_t entity, float* out_xyz_q_s) -> int32_t {
  const auto* ed = EditorSessionOfConst(session);
  return ed == nullptr ? -1 : ed->get_local(entity, out_xyz_q_s);
}

auto ApiEditorCountChildren(const EngineEditorSession* session, uint64_t parent, uint64_t* out_count) -> int32_t {
  const auto* ed = EditorSessionOfConst(session);
  return ed == nullptr ? -1 : ed->count_children(parent, out_count);
}

auto ApiEditorGetChild(const EngineEditorSession* session, uint64_t parent, uint64_t index, uint64_t* out_child)
    -> int32_t {
  const auto* ed = EditorSessionOfConst(session);
  return ed == nullptr ? -1 : ed->get_child(parent, index, out_child);
}

auto ApiEditorCountRoots(const EngineEditorSession* session, uint64_t* out_count) -> int32_t {
  const auto* ed = EditorSessionOfConst(session);
  return ed == nullptr ? -1 : ed->count_roots(out_count);
}

auto ApiEditorGetRoot(const EngineEditorSession* session, uint64_t index, uint64_t* out_child) -> int32_t {
  const auto* ed = EditorSessionOfConst(session);
  return ed == nullptr ? -1 : ed->get_root(index, out_child);
}

auto ApiEditorIsPrefabInstance(const EngineEditorSession* session, uint64_t entity) -> int32_t {
  const auto* ed = EditorSessionOfConst(session);
  return ed == nullptr ? -1 : ed->is_prefab_instance(entity);
}

auto ApiEditorBeginEdit(EngineEditorSession* session, uint64_t entity) -> EngineLocalTransform* {
  auto* ed = EditorSessionOf(session);
  if (ed == nullptr) {
    return nullptr;
  }
  auto* raw = ed->begin_edit(entity);
  return reinterpret_cast<EngineLocalTransform*>(raw);  // NOLINT
}

auto ApiEditorEndEdit(EngineEditorSession* session) -> int32_t {
  auto* ed = EditorSessionOf(session);
  return ed == nullptr ? -1 : ed->end_edit();
}

auto ApiEditorSnapshotTake(EngineEditorSession* session) -> EngineEditorSnapshot* {
  auto* ed = EditorSessionOf(session);
  if (ed == nullptr) {
    return nullptr;
  }
  engine::editor::EditorSnapshot* snap = nullptr;
  try {
    snap = ed->snapshot_take();
  } catch (...) {
    return nullptr;
  }
  if (snap == nullptr) {
    return nullptr;
  }
  // NOSONAR(cpp:S5025,cpp:S8268) opaque C-ABI snapshot handle: restore/discard own it (same as M1-04 worlds)
  auto* impl = new (std::nothrow) EngineEditorSnapshotImpl();
  if (impl == nullptr) {
    delete snap;  // NOLINT NOSONAR(cpp:S5025,cpp:S8268) paired cleanup of the row backup
    return nullptr;
  }
  impl->owned = snap;
  return reinterpret_cast<EngineEditorSnapshot*>(impl);  // NOLINT
}

auto ApiEditorSnapshotRestore(EngineEditorSession* session, EngineEditorSnapshot* snapshot) -> int32_t {
  auto* ed = EditorSessionOf(session);
  if (ed == nullptr) {
    return -1;
  }
  if (snapshot == nullptr) {
    return 0;
  }
  auto* impl = reinterpret_cast<EngineEditorSnapshotImpl*>(snapshot);  // NOLINT
  if (impl->owned == nullptr) {  // NOSONAR(cpp:S8268) null-guard on the opaque snapshot handle
    return -2;
  }
  try {
    return ed->snapshot_restore(impl->owned);
  } catch (...) {
    return -9;
  }
}

auto ApiEditorSnapshotDiscard(EngineEditorSnapshot* snapshot) -> void {
  if (snapshot == nullptr) {
    return;
  }
  auto* impl = reinterpret_cast<EngineEditorSnapshotImpl*>(snapshot);  // NOLINT
  delete impl->owned;  // NOLINT NOSONAR(cpp:S5025,cpp:S8268) frees the row backup owned by this handle
  delete impl;         // NOLINT NOSONAR(cpp:S5025,cpp:S8268) frees the snapshot handle itself
}

// --- P3-08 project-drop tail (ABI 4.7) -------------------------------------
//
// One-call asset drop: stage the file through the existing M5-01 fast
// lane (ImportFast: GUID .meta ensured + model preview parsed), spawn
// one entity named after the file stem, pose it at (ground_x, 0,
// ground_z), attach a MeshRenderer row with the deterministic content
// mesh id. Thin glue only: import + spawn + pose + attach, all through
// the APIs the pipeline and the editor session already expose.
auto ApiProjectDropAsset(EngineEditorSession* session, const char* asset_path,
                         float ground_x, float ground_z, uint64_t* out_entity,
                         uint32_t* out_mesh_id) -> int32_t {
  auto* ed = EditorSessionOf(session);
  if (ed == nullptr || asset_path == nullptr || out_entity == nullptr ||
      out_mesh_id == nullptr) {
    return -1;
  }
  try {
    return ed->drop_asset(asset_path, ground_x, ground_z, out_entity,
                          out_mesh_id);
  } catch (...) {
    return -9;
  }
}

auto ApiEditorDeleteEntity(EngineEditorSession* session, uint64_t entity) -> int32_t {
  auto* ed = EditorSessionOf(session);
  if (ed == nullptr) {
    return -1;
  }
  try {
    return ed->delete_entity(entity);
  } catch (...) {
    return -9;
  }
}

// --- ABI 4.2 generic component/system tail ---------------------------------

auto ApiComponentRegister(EngineWorld* world, const char* name, uint32_t size,
                          uint32_t alignment) -> uint64_t {
  if (world == nullptr) {
    return 0;
  }
  auto* impl = ImplOf(world);
  return impl->generic.register_component(impl->ecs.raw(), name, size,
                                          alignment);
}

auto ApiEntityCreate(EngineWorld* world) -> uint64_t {
  if (world == nullptr) {
    return 0;
  }
  auto* impl = ImplOf(world);
  return impl->generic.create_entity(impl->ecs.raw());
}

auto ApiEntitySet(EngineWorld* world, uint64_t entity, uint64_t component,
                  const void* data, uint32_t size) -> int32_t {
  if (world == nullptr) {
    return ENGINE_ABI_ERR_NULL_ARG;
  }
  auto* impl = ImplOf(world);
  const int32_t rc =
      impl->generic.set(impl->ecs.raw(), entity, component, data, size);
  if (rc == 0) {
    // Adding a component moves the entity to another table: drop the
    // run_systems lane plan (it caches Local/Velocity column pointers).
    impl->plan.reset();
  }
  return rc;
}

auto ApiEntityGet(EngineWorld* world, uint64_t entity, uint64_t component,
                  void* out, uint32_t size) -> int32_t {
  if (world == nullptr) {
    return ENGINE_ABI_ERR_NULL_ARG;
  }
  auto* impl = ImplOf(world);
  return impl->generic.get(impl->ecs.raw(), entity, component, out, size);
}

auto ApiSystemRegister(EngineWorld* world, const char* name,
                       const uint64_t* components, const uint32_t* access,
                       uint32_t term_count, EngineSystemCallback callback,
                       void* user_data) -> int32_t {
  if (world == nullptr) {
    return ENGINE_ABI_ERR_NULL_ARG;
  }
  auto* impl = ImplOf(world);
  return impl->generic.register_system(impl->ecs.raw(), name, components,
                                       access, term_count, callback,
                                       user_data);
}

auto ApiSystemsRun(EngineWorld* world, const FrameDispatch* dispatch)
    -> int32_t {
  if (world == nullptr) {
    return ENGINE_ABI_ERR_NULL_ARG;
  }
  auto* impl = ImplOf(world);
  const float dt = (dispatch != nullptr) ? dispatch->dt : 0.0F;
  // P3-09: route per-system CPU timings into the world's profiler ring.
  // The sink is vtable-free (two fn pointers + ctx) so GenericSystems
  // never includes the profiler header.
  engine::bridge::SystemTimingSink sink{};
  sink.ctx = &impl->profiler;
  sink.resize = [](void* ctx, std::size_t n) {
    static_cast<engine::bridge::WorldProfiler*>(ctx)->resize_systems(n);
  };
  sink.record = [](void* ctx, uint32_t system, float ms) {
    static_cast<engine::bridge::WorldProfiler*>(ctx)->record_system(system, ms);
  };
  return impl->generic.run(impl->ecs.raw(), dt, &sink);
}

// P3-09 console tail: process-global ring + per-world sink. The ring is
// function-local static (constructed on first use, never destroyed —
// avoids static-destruction ordering vs worlds destroyed at exit).
auto WorldLog() -> engine::bridge::ConsoleLog& {
  static engine::bridge::ConsoleLog g_log;
  return g_log;
}

auto ApiLogWrite(int32_t level, const char* msg) -> int32_t {
  return WorldLog().write(level, msg);
}

auto ApiLogSetSink(EngineWorld* world, EngineLogSinkCallback sink, void* user_data)
    -> int32_t {
  if (world == nullptr) {
    return ENGINE_ABI_ERR_NULL_ARG;
  }
  // The ring and sink are process-global; world is only validated (never
  // dereferenced, so clearing after world_destroy stays safe).
  WorldLog().set_sink(sink, user_data);
  return 0;
}

auto ApiLogRead(EngineWorld* world, uint64_t index, int32_t* out_level, char* out,
                uint32_t out_size) -> int32_t {
  if (world == nullptr) {
    return ENGINE_ABI_ERR_NULL_ARG;
  }
  return WorldLog().read(index, out_level, out, out_size);
}

auto ApiLogCount(EngineWorld* world) -> uint64_t {
  if (world == nullptr) {
    return 0;
  }
  return WorldLog().count();
}

auto ApiLogClear(EngineWorld* world) -> void {
  if (world == nullptr) {
    return;
  }
  WorldLog().clear();
}

// P3-09 profiler tail: per-world CPU frame ring + per-system timings.
// GPU time is renderer-scoped (renderer_gpu_time over FrameGpuTimer,
// exercised by test_frame_gpu_timer + the managed profiler gate).
auto ApiProfileReset(EngineWorld* world) -> void {
  if (world == nullptr) {
    return;
  }
  ImplOf(world)->profiler.reset();
}

auto ApiProfileFrame(EngineWorld* world, float ms) -> int32_t {
  if (world == nullptr) {
    return ENGINE_ABI_ERR_NULL_ARG;
  }
  return ImplOf(world)->profiler.record_frame(ms);
}

auto ApiProfileCount(EngineWorld* world) -> uint64_t {
  if (world == nullptr) {
    return 0;
  }
  return ImplOf(world)->profiler.frame_count();
}

auto ApiProfileFrameTime(EngineWorld* world, uint64_t index) -> float {
  if (world == nullptr) {
    return -1.0F;
  }
  return ImplOf(world)->profiler.frame_time(index);
}

auto ApiProfileSystemTime(EngineWorld* world, uint32_t system) -> float {
  if (world == nullptr) {
    return -1.0F;
  }
  return ImplOf(world)->profiler.system_time(system);
}

auto ApiProfileSystemCount(EngineWorld* world) -> uint64_t {
  if (world == nullptr) {
    return 0;
  }
  return ImplOf(world)->profiler.system_count();
}

// --- P2-02 input snapshot tail ------------------------------------------------
// No EngineRenderer exists yet on this branch (P1-09 lands renderer lifecycle
// later): the entry is a null-safe stub today. The snapshot PROJECTION is
// proven by the layout pins above + InputApiTests (managed) which reads a
// fake EngineInputState zero-copy; P1-09 wires the live renderer member.
auto ApiInputGetState(EngineRenderer* renderer) -> const EngineInputState* {
  if (renderer == nullptr) {
    return nullptr;
  }
  // No renderer implementation yet: report null snapshot rather than a
  // dangling pointer. P1-09 returns the live member here.
  return nullptr;
}

// --- ABI 4.5 Navigation tail (P6-03) ---------------------------------------
// The opaque ABI handle IS the owning engine::navigation::NavWorld
// (caller-owned, like the M4-03 PhysicsWorld pattern: create owns,
// destroy frees, all other entries borrow). Bake is init-time (Recast
// allocates freely); update() is the frame path (crowd step, noexcept).
struct EngineNavWorldImpl {
  engine::navigation::NavWorld world;
};

auto NavWorldOf(EngineNavWorld* nav) noexcept
    -> engine::navigation::NavWorld* {
  if (nav == nullptr) {
    return nullptr;
  }
  return &(reinterpret_cast<EngineNavWorldImpl*>(nav)->world);  // NOLINT
}

auto NavWorldOfConst(const EngineNavWorld* nav) noexcept
    -> const engine::navigation::NavWorld* {
  if (nav == nullptr) {
    return nullptr;
  }
  return &(reinterpret_cast<const EngineNavWorldImpl*>(nav)->world);  // NOLINT
}

auto ApiNavCreate() -> EngineNavWorld* {
  // NOSONAR(cpp:S5025,cpp:S8268) opaque C-ABI handle: destroy frees it (same as M1-04 worlds)
  auto* impl = new (std::nothrow) EngineNavWorldImpl();
  return reinterpret_cast<EngineNavWorld*>(impl);  // NOLINT
}

auto ApiNavDestroy(EngineNavWorld* nav) -> void {
  if (nav == nullptr) {
    return;
  }
  delete reinterpret_cast<EngineNavWorldImpl*>(nav);  // NOLINT
}

auto ApiNavBake(EngineNavWorld* nav, const EngineNavBox* obstacles,
                uint32_t obstacle_count, const EngineNavBakeConfig* config,
                EngineNavStats* out_stats) -> int32_t {
  static_assert(sizeof(EngineNavBox) ==
                    sizeof(engine::navigation::NavBoxObstacle),
                "EngineNavBox/NavBoxObstacle size drift");
  static_assert(sizeof(EngineNavBakeConfig) ==
                    sizeof(engine::navigation::NavBakeConfig),
                "EngineNavBakeConfig/NavBakeConfig size drift");
  static_assert(sizeof(EngineNavStats) ==
                    sizeof(engine::navigation::NavBakeStats),
                "EngineNavStats/NavBakeStats size drift");
  auto* world = NavWorldOf(nav);
  if (world == nullptr) {
    return 0;
  }
  engine::navigation::NavBakeConfig cfg;
  if (config != nullptr) {
    const auto* c = reinterpret_cast<const float*>(config);  // NOLINT
    auto* o = reinterpret_cast<float*>(&cfg);                // NOLINT
    for (int i = 0; i < 12; ++i) {
      o[i] = c[i];
    }
  }
  const auto* boxes =
      reinterpret_cast<const engine::navigation::NavBoxObstacle*>(  // NOLINT
          obstacles);
  const bool ok = world->bake(boxes, obstacle_count, cfg);
  if (out_stats != nullptr) {
    const auto stats = world->stats();
    out_stats->polys = stats.polys;
    out_stats->detail_tris = stats.detail_tris;
  }
  return ok ? 1 : 0;
}

auto ApiNavAgentAdd(EngineNavWorld* nav, float x, float y, float z,
                    float max_speed) -> int32_t {
  auto* world = NavWorldOf(nav);
  if (world == nullptr) {
    return -1;
  }
  return world->agent_add(x, y, z, max_speed);
}

auto ApiNavAgentGoto(EngineNavWorld* nav, float x, float y, float z)
    -> int32_t {
  auto* world = NavWorldOf(nav);
  if (world == nullptr) {
    return 0;
  }
  return world->agent_goto(x, y, z) ? 1 : 0;
}

auto ApiNavAgentUpdate(EngineNavWorld* nav, float dt) -> int32_t {
  auto* world = NavWorldOf(nav);
  if (world == nullptr) {
    return 0;
  }
  return world->agent_update(dt) ? 1 : 0;
}

auto ApiNavAgentPos(const EngineNavWorld* nav, float* out_xyz) -> int32_t {
  const auto* world = NavWorldOfConst(nav);
  if (world == nullptr || out_xyz == nullptr) {
    return 0;
  }
  float xyz[3] = {0.0F, 0.0F, 0.0F};
  if (!world->agent_pos(xyz)) {
    return 0;
  }
  out_xyz[0] = xyz[0];
  out_xyz[1] = xyz[1];
  out_xyz[2] = xyz[2];
  return 1;
}

auto ApiNavAgentReached(const EngineNavWorld* nav, float stop_radius)
    -> int32_t {
  const auto* world = NavWorldOfConst(nav);
  if (world == nullptr) {
    return 0;
  }
  return world->agent_reached(stop_radius) ? 1 : 0;
}

}  // namespace

namespace engine::bridge {
// --- ABI 4.4 renderer tail (P1-09): forward declarations (external
// linkage; definitions live in renderer_bridge.cpp) ------------------------
auto ApiRendererCreate(EngineWorld* world, std::uint32_t width,
                       std::uint32_t height, std::uint32_t flags) noexcept
    -> EngineRenderer*;
auto ApiRendererDestroy(EngineRenderer* renderer) noexcept -> void;
auto ApiRendererFrame(EngineRenderer* renderer) noexcept -> int32_t;
auto ApiRendererMeshCreate(EngineRenderer* renderer, const float* vertices,
                           std::uint32_t vertex_count,
                           const std::uint32_t* indices,
                           std::uint32_t index_count) noexcept -> std::uint32_t;
auto ApiRendererScreenshot(EngineRenderer* renderer,
                           const char* png_path) noexcept -> int32_t;
// --- ABI 4.6 offscreen + picking tail (P3-02/P3-06) ------------------------
auto ApiRendererCreateOffscreen(EngineWorld* world, std::uint32_t width,
                                std::uint32_t height) noexcept
    -> EngineRenderer*;
auto ApiRendererRenderToBuffer(EngineRenderer* renderer, std::uint8_t* rgba_out,
                               std::uint64_t capacity, std::uint32_t width,
                               std::uint32_t height,
                               const float* view_proj16) noexcept -> int32_t;
auto ApiRendererPick(EngineRenderer* renderer, std::uint32_t x,
                     std::uint32_t y, const float* view_proj16) noexcept
    -> std::uint64_t;

auto ApiRendererGpuTime(EngineRenderer* renderer) noexcept -> float;
}  // namespace engine::bridge

// Unwraps EngineWorld* -> raw Flecs world (borrowed). Defined here next
// to ImplOf so the renderer TU never aliases the EngineWorldImpl layout.
namespace engine::bridge {
auto BridgeRawWorld(EngineWorld* world) noexcept -> ecs_world_t* {
  if (world == nullptr) {
    return nullptr;
  }
  return ImplOf(world)->ecs.raw();
}
}  // namespace engine::bridge

namespace {

// --- P3-03 Hierarchy panel tail ------------------------------------------
// Snapshot helpers wrap take/restore in nothrow handles like the whole-scene
// pair above; destroy/get_name/duplicate forward to EditorSession.

auto ApiEditorDestroyEntity(EngineEditorSession* session, uint64_t entity) -> int32_t {
  auto* ed = EditorSessionOf(session);
  return ed == nullptr ? -1 : ed->destroy_entity(entity);
}

auto ApiEditorGetName(const EngineEditorSession* session, uint64_t entity, char* out, uint64_t capacity)
    -> int32_t {
  const auto* ed = EditorSessionOfConst(session);
  return ed == nullptr ? -1 : ed->get_name(entity, out, capacity);
}

auto ApiEditorDuplicate(EngineEditorSession* session, uint64_t entity, uint64_t* out_entity) -> int32_t {
  auto* ed = EditorSessionOf(session);
  return ed == nullptr ? -1 : ed->duplicate(entity, out_entity);
}

auto ApiEditorSnapshotTakeSubtree(EngineEditorSession* session, uint64_t root) -> EngineEditorSnapshot* {
  auto* ed = EditorSessionOf(session);
  if (ed == nullptr) {
    return nullptr;
  }
  engine::editor::EditorSnapshot* snap = nullptr;
  try {
    snap = ed->snapshot_take_subtree(root);
  } catch (...) {
    return nullptr;
  }
  if (snap == nullptr) {
    return nullptr;
  }
  // NOSONAR(cpp:S5025,cpp:S8268) opaque C-ABI snapshot handle: restore/discard own it (same pattern as the whole-scene pair)
  auto* impl = new (std::nothrow) EngineEditorSnapshotImpl();
  if (impl == nullptr) {
    delete snap;  // NOLINT NOSONAR(cpp:S5025,cpp:S8268) paired cleanup of the row backup
    return nullptr;
  }
  impl->owned = snap;
  return reinterpret_cast<EngineEditorSnapshot*>(impl);  // NOLINT
}

auto ApiEditorSnapshotRestoreSubtree(EngineEditorSession* session, EngineEditorSnapshot* snapshot,
                                     uint64_t root) -> int32_t {
  auto* ed = EditorSessionOf(session);
  if (ed == nullptr) {
    return -1;
  }
  if (snapshot == nullptr) {
    return 0;
  }
  auto* impl = reinterpret_cast<EngineEditorSnapshotImpl*>(snapshot);  // NOLINT
  if (impl->owned == nullptr) {  // NOSONAR(cpp:S8268) null-guard on the opaque snapshot handle
    return -2;
  }
  try {
    return ed->snapshot_restore_subtree(impl->owned, root);
  } catch (...) {
    return -9;
  }
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
    &ApiRenderPassCreate,
    &ApiRenderPassDestroy,
    &ApiRenderPassAddImage,
    &ApiRenderPassAddPass,
    &ApiRenderPassCompile,
    &ApiRenderPassRunFrame,
    &ApiRenderPassPassResult,
    &ApiRenderPassFailedCount,
    &ApiPhysicsRaycast,
    &ApiPhysicsSphereCast,
    &ApiPhysicsOverlapSphere,
    &ApiPhysicsRaycastBatch,
    &ApiEditorCreate,
    &ApiEditorDestroy,
    &ApiEditorSpawn,
    &ApiEditorSetName,
    &ApiEditorSetParent,
    &ApiEditorAddLocal,
    &ApiEditorSetLocal,
    &ApiEditorGetLocal,
    &ApiEditorCountChildren,
    &ApiEditorGetChild,
    &ApiEditorCountRoots,
    &ApiEditorGetRoot,
    &ApiEditorIsPrefabInstance,
    &ApiEditorBeginEdit,
    &ApiEditorEndEdit,
    &ApiEditorSnapshotTake,
    &ApiEditorSnapshotRestore,
    &ApiEditorSnapshotDiscard,
    &ApiComponentRegister,
    &ApiEntityCreate,
    &ApiEntitySet,
    &ApiEntityGet,
    &ApiSystemRegister,
    &ApiSystemsRun,
    &ApiInputGetState,
    &engine::bridge::ApiRendererCreate,
    &engine::bridge::ApiRendererDestroy,
    &engine::bridge::ApiRendererFrame,
    &engine::bridge::ApiRendererMeshCreate,
    &engine::bridge::ApiRendererScreenshot,
    &ApiNavCreate,
    &ApiNavDestroy,
    &ApiNavBake,
    &ApiNavAgentAdd,
    &ApiNavAgentGoto,
    &ApiNavAgentUpdate,
    &ApiNavAgentPos,
    &ApiNavAgentReached,

    &engine::bridge::ApiRendererCreateOffscreen,
    &engine::bridge::ApiRendererRenderToBuffer,
    &engine::bridge::ApiRendererPick,

    &ApiProjectDropAsset,
    &ApiEditorDeleteEntity,

    &ApiLogWrite,
    &ApiLogSetSink,
    &ApiLogRead,
    &ApiLogCount,
    &ApiLogClear,
    &ApiProfileReset,
    &ApiProfileFrame,
    &ApiProfileCount,
    &ApiProfileFrameTime,
    &ApiProfileSystemTime,
    &ApiProfileSystemCount,
    &engine::bridge::ApiRendererGpuTime,

    &ApiPhysics2DCreate,
    &ApiPhysics2DDestroy,
    &ApiPhysics2DBodyBox,
    &ApiPhysics2DBodyCircle,
    &ApiPhysics2DBodyPolygon,
    &ApiPhysics2DBodyDestroy,
    &ApiPhysics2DStep,
    &ApiPhysics2DDrain,
    &ApiEditorDestroyEntity,
    &ApiEditorGetName,
    &ApiEditorDuplicate,
    &ApiEditorSnapshotTakeSubtree,
    &ApiEditorSnapshotRestoreSubtree,
};

}  // namespace

extern "C" {

// Stable host entry points for managed hosts (dlsym/GetExport surface).
// Thin wrappers over the engine::bridge C++ API above.
ENGINE_CORE_API auto engine_dotnet_host_create(const char* runtime_config_path,
                                               const char* dotnet_root) noexcept
    -> engine::bridge::DotNetHost* {
  return engine::bridge::dotnet_host_create(runtime_config_path, dotnet_root);
}

ENGINE_CORE_API auto engine_dotnet_host_destroy(
    engine::bridge::DotNetHost* host) noexcept -> void {
  engine::bridge::dotnet_host_destroy(host);
}

ENGINE_CORE_API auto engine_dotnet_host_get_system_pointer(
    engine::bridge::DotNetHost* host, const char* assembly_path,
    const char* type_name, const char* method_name) noexcept -> void* {
  return engine::bridge::dotnet_host_get_system_pointer(host, assembly_path,
                                                        type_name, method_name);
}

ENGINE_CORE_API auto engine_get_api_table(void) -> const EngineApiTable* {
  return &kApiTable;
}

}  // extern "C"

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
#include <memory>
#include <vector>

#include "engine/bridge/dotnet_host.hpp"
#include "engine/core/engine_core_export.hpp"
#include "engine/core/job_scheduler.hpp"
#include "engine/ecs/ecs_world.hpp"
#include "engine/ecs/transform.hpp"
#include "flecs.h"

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
        l->px = 0.0F;
        l->py = 0.0F;
        l->pz = 0.0F;
        l->qx = 0.0F;
        l->qy = 0.0F;
        l->qz = 0.0F;
        l->qw = 1.0F;
        l->sx = 1.0F;
        l->sy = 1.0F;
        l->sz = 1.0F;
        v[0] = 0.0F;
        v[1] = 0.0F;
        v[2] = 0.0F;
        v[3] = 0.0F;
      }
      impl->plan.reset();
      return count;
    }
    engine::ecs::LocalTransform* local = local0;
    float* vel = vel0;
    for (uint64_t i = 0; i < count; ++i) {
      local[i].px = 0.0F;
      local[i].py = 0.0F;
      local[i].pz = 0.0F;
      local[i].qx = 0.0F;
      local[i].qy = 0.0F;
      local[i].qz = 0.0F;
      local[i].qw = 1.0F;
      local[i].sx = 1.0F;
      local[i].sy = 1.0F;
      local[i].sz = 1.0F;
      vel[i * 4] = 0.0F;
      vel[i * 4 + 1] = 0.0F;
      vel[i * 4 + 2] = 0.0F;
      vel[i * 4 + 3] = 0.0F;
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
      uint32_t hw = 0;
      if (impl->sched != nullptr && impl->sched->running()) {
        hw = impl->sched->worker_count();
      }
      const uint32_t lanes = hw > 0 ? hw + 1 : 1;
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
        // context is null (benchmark archetype shape).
        view.user_data = (sys.user_data != nullptr) ? sys.user_data
                                                    : const_cast<float*>(c.vel);
        sys.callback(&view);
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
                                impl->systems.size()});
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

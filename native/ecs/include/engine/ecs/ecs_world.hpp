// ecs_world.hpp — M1-03: Flecs world wrapper with enkiTS task threads,
// LocalTransform/WorldTransform components, and the breadth-first
// hierarchy propagation pipeline.
//
// Usage:
//   EcsWorld w;
//   w.set_scheduler(&sched);   // before construct(); may be null
//   w.construct();             // ecs_init + component registration
//   w.set_task_threads(n);     // n>=2 routes stages via enkiTS
//   w.propagate();             // EcsCascade over EcsChildOf, SIMD
//   w.destroy();
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"
#include "engine/core/job_scheduler.hpp"
#include "engine/ecs/transform.hpp"

struct ecs_world_t;
struct ecs_query_t;

namespace engine::ecs {

class ENGINE_CORE_API EcsWorld {
 public:
  EcsWorld() noexcept = default;
  EcsWorld(const EcsWorld&) = delete;
  auto operator=(const EcsWorld&) -> EcsWorld& = delete;
  ~EcsWorld() noexcept;

  // Scheduler backing Flecs task threads. Must outlive this world.
  // May be null (plain os-thread staging). Set before construct().
  auto set_scheduler(core::JobScheduler* sched) noexcept -> void {
    sched_ = sched;
  }

  // Create the Flecs world and register LocalTransform/WorldTransform.
  // Installs task hooks when a running scheduler was set. Returns false
  // on ecs_init failure or double-construct.
  auto construct() noexcept -> bool;

  // Tear down (ecs_fini). Task-threaded worlds are first returned to
  // single-stage so enkiTS tasks join before the world dies.
  auto destroy() noexcept -> void;

  [[nodiscard]] auto valid() const noexcept -> bool { return world_ != nullptr; }
  [[nodiscard]] auto raw() const noexcept -> ecs_world_t* { return world_; }

  // Stage count for Flecs' pipeline. threads==1 (default) is single
  // threaded. threads>=2 routes worker stages through the installed
  // task hooks (requires a running scheduler). Flecs task workers are
  // persistent threads that occupy their execution slot for the world's
  // whole lifetime, so stages are clamped to worker_count()+1 (the pool
  // workers plus the calling thread) — oversubscribing a fixed pool
  // would leave the extra worker never started and deadlock the sync.
  auto set_task_threads(std::int32_t threads) noexcept -> bool;

  // One-shot hierarchy propagation (safe to call every frame), in
  // row-vector order: World = Local * ParentWorld (roots: World = Local).
  // ComposeLocal emits row-vector matrices (translation in row 3, the
  // rotation block is R-transpose), so the local matrix left-multiplies
  // the parent world matrix. Runs via a breadth-first EcsCascade query
  // over EcsChildOf. The component handles come from construct(); ids
  // stable for the world's lifetime.
  auto propagate() noexcept -> bool;

  // Bulk-create `count` entities with identity LocalTransform +
  // WorldTransform. Returns entities created.
  auto bulk_create(std::size_t count) noexcept -> std::size_t;

  // Component ids (valid after construct()).
  [[nodiscard]] auto local_id() const noexcept -> std::uint64_t {
    return local_id_;
  }
  [[nodiscard]] auto world_id() const noexcept -> std::uint64_t {
    return world_id_;
  }

 private:
  ecs_world_t* world_{nullptr};
  ecs_query_t* cascade_query_{nullptr};
  core::JobScheduler* sched_{nullptr};
  std::uint64_t local_id_{0};
  std::uint64_t world_id_{0};
};

}  // namespace engine::ecs

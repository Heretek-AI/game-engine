// job_system_enki.hpp — M4-01: Jolt::JobSystem backed by enkiTS.
//
// Routes Jolt physics jobs onto the engine's shared enkiTS scheduler so ECS
// and Physics share the same worker threads (no second thread pool).
// Built on JobSystemWithBarrier: barrier bookkeeping is inherited; only
// GetMaxConcurrency / CreateJob / FreeJob / QueueJob(s) route to enkiTS.
//
// Lifetime: construct with a running scheduler, Init(max_barriers,
// max_jobs_per_barrier) once before use (inherited barrier Init + fixed
// job-slot pool + pending-set list reservations; Init-time only). Each
// queued ready Jolt job takes one pooled slot (placement-new) and one
// enkiTS TaskSet of size 1. QueueJob never mallocs/news on the frame path:
// job slots come from the fixed pool, enkiTS sets from enkiTS' internal
// pool, the pending list is reserved at Init. Reclaim in WaitForJobs waits
// each set (enkiWaitForTaskSet) before deleting it — delete-without-wait
// is a use-after-free against in-flight worker TUs.
//
// Ref-counting (upstream ThreadPool contract, see JobSystemThreadPool.cpp):
// CreateJob's initial ref belongs to the returned JobHandle; QueueJob takes
// one extra ref for the queued execution, released after Execute returns
// (or when the barrier pump executes the job inline on the waiting thread).
// FreeJob returns the slot to the pool (Jolt calls it when the last ref
// drops: handle-destruction or barrier-drain release).
#pragma once

#include "engine/core/engine_core_export.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#include "engine/core/job_scheduler.hpp"

// WARNING: Jolt headers are included ONLY here and in the sibling .cpp TUs
// (job_system_enki.cpp, physics_world.cpp, temp_allocator_arena.cpp) — the
// only TUs that textually include <Jolt/...>. They arrive via the
// engine_physics_jolt_wrapper interface (SYSTEM include + -Wno-conversion),
// so -Wconversion -Werror never fires inside Jolt headers. Never include
// <Jolt/...> from any other engine header.
#include <Jolt/Jolt.h>
#include <Jolt/Core/Color.h>
#include <Jolt/Core/JobSystemWithBarrier.h>

struct enkiTaskSet;

namespace engine::physics {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251 4275)  // STL members + Jolt non-dll bases: same-toolchain, OBJECT lib linked into engine_core.
#endif

// Pool sizing: worst case per PhysicsSystem::Update is one barrier with
// jobs for every island + broadphase + narrowphase batch. Upstream
// BarrierImpl caps cMaxJobs=2048 per barrier; cap our pool at
// max_barriers * 2048 worst case is unbounded memory, so Init takes an
// explicit per-barrier job cap (default 2048 matches upstream) and fails
// closed when exceeded — QueueJob drops the job (Jolt treats a missing
// queue as "run inline at Wait", same as its zero-worker-thread path)
// instead of allocating.
inline constexpr JPH::uint kDefaultMaxJobsPerBarrier = 2048;

class ENGINE_CORE_API EnkiJobSystem final : public JPH::JobSystemWithBarrier {
 public:
  explicit EnkiJobSystem(core::JobScheduler* sched) noexcept : sched_(sched) {}
  ~EnkiJobSystem() override;

  // Reserve barrier count + job-slot pool + pending-set list up front
  // (Init-time only; never on the frame path). Returns false when the
  // scheduler is null/stopped or sizes are 0. Calls the inherited
  // JobSystemWithBarrier::Init(max_barriers) — do NOT call that directly.
  auto Init(JPH::uint max_barriers,
            JPH::uint max_jobs_per_barrier = kDefaultMaxJobsPerBarrier) noexcept
      -> bool;

  auto GetMaxConcurrency() const -> int override;

  auto CreateJob(const char* inName, JPH::ColorArg inColor,
                 const JobFunction& inJobFunction,
                 JPH::uint32 inNumDependencies = 0) -> JobHandle override;
  auto FreeJob(Job* inJob) -> void override;
  auto QueueJob(Job* inJob) -> void override;
  auto QueueJobs(Job** inJobs, JPH::uint inNumJobs) -> void override;
  auto WaitForJobs(Barrier* inBarrier) -> void override;

  [[nodiscard]] auto scheduler() const noexcept -> core::JobScheduler* {
    return sched_;
  }

 private:
  // Concrete Jolt job: names the protected JPH::JobSystem::Job base so the
  // ctor can forward (name, color, owning system, function, dependencies).
  struct EnkiJob final : public Job {
    EnkiJob(const char* name, JPH::ColorArg color, JPH::JobSystem* sys,
            const JobFunction& fn, JPH::uint32 deps)
        : Job(name, color, sys, fn, deps) {}
  };

  // Fixed slot pool: one EnkiJob footprint + free-stack, all reserved at
  // Init. Index-tagged free list, lock-protected (CreateJob/FreeJob race
  // across enkiTS workers + barrier pump thread).
  struct Slot {
    alignas(EnkiJob) std::array<std::byte, sizeof(EnkiJob)> storage{};
    enkiTaskSet* task_set{nullptr};
  };

  // Reclaim every enkiTS set created since the last wait: WAIT each set
  // first (completion guaranteed only after the barrier's Wait drains AND
  // our launched sets finish — WaitForJobs callers must not assume the
  // barrier pump alone joined enkiTS work), then delete. All complete once
  // both drain.
  auto ReclaimSets() noexcept -> void;

  // enkiTS entry point: runs one Jolt job then releases the queued ref.
  // Static member (not a free function) so the cast can name the protected
  // JPH::JobSystem::Job base — free functions cannot refer to it.
  static auto RunJob(std::uint32_t start, std::uint32_t end,
                     std::uint32_t threadnum, void* args) -> void;

  core::JobScheduler* sched_{nullptr};
  std::mutex mutex_;
  std::vector<Slot> slots_;              // fixed pool, sized at Init
  std::vector<JPH::uint32> free_stack_;  // slot free list (LIFO)
  std::vector<enkiTaskSet*> pending_;    // launched sets, reserved at Init
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace engine::physics

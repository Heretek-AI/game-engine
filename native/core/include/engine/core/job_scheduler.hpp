// job_scheduler.hpp — M1-03: enkiTS work-stealing thread pool + Flecs task hooks.
//
// A process-wide JobScheduler owns one enkiTS task scheduler with
// (N - 1) worker threads (N = online logical CPUs), each pinned to a
// distinct physical CPU core. The Flecs multi-threaded task API
// (ecs_os_api.task_new_/task_join_) is bound to enkiTS, so
// ecs_set_task_threads() stages run on our pinned workers.
//
// Threading model: InstallTaskHooks() patches ONLY task_new_/task_join_
// into the live global ecs_os_api table (direct struct assignment —
// ecs_os_set_api() is one-shot and must not be re-called after M1-02's
// InstallAlignedFlecsHooks ran). ecs_os_api itself is written once before
// any world exists in practice; concurrent task_new_/task_join_ calls are
// served by enkiTS (lock-free) and the atomic g_owner below.
//
// Lifetime: init() once at startup (before ecs_init), shutdown() at
// teardown after all worlds using the hooks are destroyed. Not
// thread-safe for init/shutdown races — same contract as the OS API.
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"

struct enkiTaskScheduler;

namespace engine::core {

inline constexpr std::uint32_t kInvalidCpu = 0xFFFFFFFFu;

class ENGINE_CORE_API JobScheduler {
 public:
  JobScheduler() noexcept = default;
  JobScheduler(const JobScheduler&) = delete;
  auto operator=(const JobScheduler&) -> JobScheduler& = delete;
  ~JobScheduler() noexcept;

  // Create the enkiTS scheduler with (online_cpus - 1) worker threads and
  // pin each worker to a distinct physical core. Returns false when enkiTS
  // allocation fails. May be called at most once; a second call fails.
  // `pinned_out` (optional, capacity >= worker_count()) receives the OS
  // cpu id each worker was pinned to, in enkiTS thread-number order.
  auto init(std::uint32_t* pinned_out = nullptr,
            std::size_t pinned_cap = 0) noexcept -> bool;

  // Shut down and free the enkiTS scheduler. Worlds bound to the task
  // hooks must be destroyed BEFORE calling this. Idempotent.
  auto shutdown() noexcept -> void;

  [[nodiscard]] auto running() const noexcept -> bool { return sched_ != nullptr; }

  // Worker threads created by init() (0 before init). Total threads
  // participating in work (incl. the calling thread) is worker_count()+1.
  [[nodiscard]] auto worker_count() const noexcept -> std::uint32_t {
    return worker_count_;
  }

  // Physical cpu each worker is pinned to (size worker_count()).
  [[nodiscard]] auto pinned_cpu(std::uint32_t worker) const noexcept
      -> std::uint32_t;

  [[nodiscard]] auto native() const noexcept -> enkiTaskScheduler* {
    return sched_;
  }

  // Maximum worker threads (size of the pinning table).
  static constexpr std::size_t kMaxWorkersForInit = 256;

 private:
  enkiTaskScheduler* sched_{nullptr};
  std::uint32_t worker_count_{0};
  static constexpr std::size_t kMaxWorkers = 256;
  std::uint32_t pinned_[kMaxWorkers]{};
};

// Install task_new_/task_join_ hooks that run each Flecs worker as one
// enkiTS task-set of size 1 on `sched` (which must be running). The
// malloc/calloc/realloc/free hooks installed by M1-02 are preserved.
// Call once before ecs_init; uninstall with RemoveTaskHooks().
ENGINE_CORE_API auto InstallTaskHooks(JobScheduler* sched) noexcept -> bool;

// Restore stock Flecs task hooks (plain os-thread tasks). The scheduler
// may be shut down after this returns. Worlds using task threads must be
// finished (ecs_set_threads(world, 1) / ecs_fini) before calling.
ENGINE_CORE_API auto RemoveTaskHooks() noexcept -> void;

// Scheduler currently serving the task hooks (null when uninstalled).
ENGINE_CORE_API auto TaskHooksScheduler() noexcept -> JobScheduler*;

// Online logical CPU count (sched_getaffinity/online cpus). Never 0.
ENGINE_CORE_API auto OnlineCpuCount() noexcept -> std::uint32_t;

// Distinct physical core ids for this machine, first-seen order, capped
// at `cap`. Returns the count written. Linux parses
// /sys/devices/system/cpu/cpu*/topology/thread_siblings_list (falls back
// to 0..N-1); other OSes return 0..N-1 directly.
ENGINE_CORE_API auto PhysicalCores(std::uint32_t* out,
                                   std::size_t cap) noexcept -> std::size_t;

}  // namespace engine::core

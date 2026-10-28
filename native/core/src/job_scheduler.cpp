// job_scheduler.cpp — enkiTS pool + Flecs task-hook binding; see header.
//
// Design notes:
//  - CPU pinning happens inside a profiler threadStart callback, which
//    enkiTS invokes once per worker on the worker thread itself, so no
//    cross-thread affinity races are possible.
//  - Flecs task_new_: each worker becomes one enkiTS TaskSet of size 1.
//    task_join_ waits on that set and frees the wrapper. enkiTS signatures
//    bind at runtime (no new/delete on the frame path — only TaskSet
//    control blocks, allocated from enkiTS' internal pool).
//  - InstallTaskHooks() writes ecs_os_api.task_new_/task_join_ directly
//    (NOT via ecs_os_set_api, which is one-shot and already consumed by
//    InstallAlignedFlecsHooks). Workers read those two fields only while
//    task threads are active; install/uninstall happen while no world
//    has task threads, matching Flecs' own usage contract.
#include "engine/core/job_scheduler.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#elif defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

#include "TaskScheduler_c.h"
#include "flecs.h"

namespace engine::core {
namespace {

// Scheduler currently installed; atomic so task_new_ (any thread) never
// races with install/uninstall (which happen while no tasks are live).
std::atomic<JobScheduler*> g_owner{nullptr};

// Default (pre-install) task hooks: plain short-lived os threads, same
// shape as Flecs' own examples (bake/stdcpp).
auto DefaultTaskNew(ecs_os_thread_callback_t cb, void* param)
    -> ecs_os_thread_t {
  // Flecs' posix thread_new_ creates a detached thread and returns 1.
  // task_new_ needs a joinable handle, so implement it here with pthreads
  // (Windows/macOS paths mirror their platform thread APIs) rather than
  // delegating to thread_new_.
#if defined(_WIN32)
  struct Pack {
    ecs_os_thread_callback_t cb;
    void* param;
  };
  Pack* pack = static_cast<Pack*>(std::malloc(sizeof(Pack)));  // NOLINT
  if (pack == nullptr) {
    return 0;
  }
  pack->cb = cb;
  pack->param = param;
  HANDLE h = CreateThread(
      nullptr, 0,
      [](LPVOID p) -> DWORD {
        Pack* q = static_cast<Pack*>(p);  // NOLINT
        q->cb(q->param);
        std::free(q);
        return 0;
      },
      pack, 0, nullptr);
  if (h == nullptr) {
    std::free(pack);
    return 0;
  }
  return static_cast<ecs_os_thread_t>(reinterpret_cast<std::uintptr_t>(h));
#else
  pthread_t* th = static_cast<pthread_t*>(std::malloc(sizeof(pthread_t)));  // NOLINT
  if (th == nullptr) {
    return 0;
  }
  struct Pack {
    ecs_os_thread_callback_t cb;
    void* param;
  };
  Pack* pack = static_cast<Pack*>(std::malloc(sizeof(Pack)));  // NOLINT
  if (pack == nullptr) {
    std::free(th);
    return 0;
  }
  pack->cb = cb;
  pack->param = param;
  auto entry = [](void* p) -> void* {
    Pack* q = static_cast<Pack*>(p);  // NOLINT
    void* r = q->cb(q->param);
    std::free(q);
    return r;
  };
  if (pthread_create(th, nullptr, entry, pack) != 0) {
    std::free(pack);
    std::free(th);
    return 0;
  }
  return static_cast<ecs_os_thread_t>(reinterpret_cast<std::uintptr_t>(th));
#endif
}

auto DefaultTaskJoin(ecs_os_thread_t thread) -> void* {
  if (thread == 0) {
    return nullptr;
  }
#if defined(_WIN32)
  HANDLE h = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(thread));  // NOLINT
  WaitForSingleObject(h, INFINITE);
  CloseHandle(h);
  return nullptr;
#else
  pthread_t* th = reinterpret_cast<pthread_t*>(static_cast<std::uintptr_t>(thread));  // NOLINT
  void* ret = nullptr;
  pthread_join(*th, &ret);
  std::free(th);
  return ret;
#endif
}

// One live Flecs worker task: exactly one enkiTS TaskSet range slot.
// (Task threads are occupied for the world's whole lifetime, so callers
// must not stage more workers than the pool holds — EcsWorld clamps
// stages to worker_count()+1. Task creation is rare (once per worker per
// world), never frame-path, so one small malloc per handle is fine.)
struct FlecsTask {
  ecs_os_thread_callback_t cb;
  void* param;
  enkiTaskSet* set;
};

auto FlecsTaskRun(std::uint32_t start, std::uint32_t end,
                  std::uint32_t threadnum, void* args) -> void {
  (void)start;
  (void)end;
  (void)threadnum;
  auto* ft = static_cast<FlecsTask*>(args);  // NOLINT
  (void)ft->cb(ft->param);
}

auto EnkiTaskNew(ecs_os_thread_callback_t cb, void* param)
    -> ecs_os_thread_t {
  JobScheduler* owner = g_owner.load(std::memory_order_acquire);
  if (owner == nullptr || !owner->running()) {
    return DefaultTaskNew(cb, param);
  }
  auto* ft = static_cast<FlecsTask*>(std::malloc(sizeof(FlecsTask)));  // NOLINT
  if (ft == nullptr) {
    return 0;
  }
  ft->cb = cb;
  ft->param = param;
  ft->set = enkiCreateTaskSet(owner->native(), FlecsTaskRun);
  if (ft->set == nullptr) {
    std::free(ft);
    return 0;
  }
  enkiAddTaskSetArgs(owner->native(), ft->set, ft, 1);
  return static_cast<ecs_os_thread_t>(reinterpret_cast<std::uintptr_t>(ft));
}

auto EnkiTaskJoin(ecs_os_thread_t thread) -> void* {
  JobScheduler* owner = g_owner.load(std::memory_order_acquire);
  auto* ft = reinterpret_cast<FlecsTask*>(static_cast<std::uintptr_t>(thread));  // NOLINT
  if (ft == nullptr) {
    return nullptr;
  }
  // Foreign handle (DefaultTaskNew path taken when owner was null):
  // recognizable because EnkiTaskNew always sets g_owner before
  // allocating. A null owner now means the scheduler went away, so fall
  // back to joining as a plain thread handle.
  if (owner == nullptr || !owner->running()) {
    return DefaultTaskJoin(thread);
  }
  enkiWaitForTaskSet(owner->native(), ft->set);
  enkiDeleteTaskSet(owner->native(), ft->set);
  std::free(ft);
  return nullptr;
}

// ---- CPU pinning (threadStart runs ON the fresh worker thread) ----

// True when `cpu` is usable by this process (intersects the process
// affinity mask where the OS exposes one).
auto CpuAllowed(std::uint32_t cpu) noexcept -> bool {
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof(set), &set) != 0) {
    return true;  // No mask to consult; assume allowed.
  }
  if (cpu >= static_cast<std::uint32_t>(CPU_SETSIZE)) {
    return false;
  }
  return CPU_ISSET(static_cast<int>(cpu), &set) != 0;
#elif defined(_WIN32)
  DWORD_PTR proc_mask = 0;
  DWORD_PTR sys_mask = 0;
  if (GetProcessAffinityMask(GetCurrentProcess(), &proc_mask, &sys_mask) ==
      0) {
    return true;
  }
  if (cpu >= 64) {
    return false;
  }
  return (proc_mask & (static_cast<DWORD_PTR>(1ULL) << (cpu % 64))) != 0;
#else
  (void)cpu;
  return true;  // No public affinity API; assume allowed.
#endif
}

auto PinSelfTo(std::uint32_t cpu) noexcept -> bool {
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#elif defined(_WIN32)
  return SetThreadAffinityMask(GetCurrentThread(),
                               static_cast<DWORD_PTR>(1ULL << (cpu % 64))) != 0;
#elif defined(__APPLE__)
  (void)cpu;  // No public thread-affinity API; QoS left to the OS.
  return true;
#else
  (void)cpu;
  return false;
#endif
}

// Physical cpu ids usable by this process, first-seen order: the Linux
// topology enumeration is intersected with the process affinity mask, so
// containers/taskset-constrained runs never pin workers to CPUs they may
// not run on. Live status table: written by init(), and updated by the
// worker thread itself when its pin fails — hence atomic (relaxed is
// enough: single writer per slot, readers need only the latest value).
std::atomic<std::uint32_t> g_pinned[JobScheduler::kMaxWorkersForInit];
static_assert(JobScheduler::kMaxWorkersForInit == 256,
              "worker table size drifted");

void ThreadStartPin(std::uint32_t threadnum) {
  // enkiTS numbers the primary (initializing) thread 0 and launches
  // internal workers numbered 1..N (see TaskScheduler::Initialize: thread
  // 0 is PRIMARY_REGISTERED, worker std::threads start at
  // numExternalTaskThreads + GetNumFirstExternalTaskThread() == 1 when no
  // external threads are registered). g_pinned[0..workers-1] is filled by
  // init(), so worker k reads slot k-1. Thread 0 never reaches here;
  // anything outside 1..workers, unset slots, and CPUs outside the
  // process affinity mask are left alone (success is per-worker, checked
  // by the caller only through the pinned_cpu table).
  if (threadnum == 0 ||
      threadnum - 1 >= JobScheduler::kMaxWorkersForInit) {
    return;
  }
  const std::uint32_t cpu = g_pinned[threadnum - 1].load(std::memory_order_relaxed);
  if (cpu == kInvalidCpu || !CpuAllowed(cpu)) {
    return;
  }
  if (!PinSelfTo(cpu)) {
    // Affinity rejected at pin time: mark the miss visible instead of
    // leaving a CPU the worker is not actually on.
    g_pinned[threadnum - 1].store(kInvalidCpu, std::memory_order_relaxed);
  }
}

}  // namespace

auto OnlineCpuCount() noexcept -> std::uint32_t {
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof(set), &set) == 0) {
    long n = 0;
    for (int i = 0; i < CPU_SETSIZE; ++i) {
      n += CPU_ISSET(i, &set) ? 1 : 0;
    }
    if (n > 0) {
      return static_cast<std::uint32_t>(n);
    }
  }
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? static_cast<std::uint32_t>(n) : 1U;
#elif defined(_WIN32)
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  return si.dwNumberOfProcessors > 0 ? si.dwNumberOfProcessors : 1U;
#elif defined(__APPLE__)
  int ncpu = 0;
  std::size_t len = sizeof(ncpu);
  if (sysctlbyname("hw.logicalcpu", &ncpu, &len, nullptr, 0) == 0 && ncpu > 0) {
    return static_cast<std::uint32_t>(ncpu);
  }
  return 1U;
#else
  return 1U;
#endif
}

auto PhysicalCores(std::uint32_t* out, std::size_t cap) noexcept
    -> std::size_t {
  if (out == nullptr || cap == 0) {
    return 0;
  }
#if defined(__linux__)
  std::size_t n = 0;
  // this_cpu -> first sibling id; siblings already reported map to it.
  std::uint32_t sibling_of[4096];
  for (std::size_t i = 0; i < 4096; ++i) {
    sibling_of[i] = 0xFFFFFFFFu;
  }
  for (std::uint32_t cpu = 0; cpu < 4096 && n < cap; ++cpu) {
    // Skip CPUs the process may not run on (affinity mask) BEFORE the
    // sibling-group check, so a group whose first thread is disallowed
    // still contributes its allowed siblings.
    if (!CpuAllowed(cpu)) {
      continue;
    }
    char path[128];
    std::snprintf(path, sizeof(path),
                  "/sys/devices/system/cpu/cpu%u/topology/thread_siblings_list",
                  cpu);
    FILE* f = std::fopen(path, "r");
    if (f == nullptr) {
      // No more cpu dirs (or offline cpu): keep scanning a little, then
      // fall back to identity mapping below if nothing was found.
      continue;
    }
    unsigned first = 0;
    int rc = std::fscanf(f, "%u", &first);
    std::fclose(f);
    if (rc != 1) {
      continue;
    }
    if (first < 4096 && sibling_of[first] == 0xFFFFFFFFu) {
      sibling_of[first] = cpu;
      out[n++] = cpu;
    }
  }
  if (n > 0) {
    return n;
  }
#endif
  const auto online = OnlineCpuCount();
  for (std::uint32_t i = 0; i < online && static_cast<std::size_t>(i) < cap;
       ++i) {
    out[i] = i;
  }
  return static_cast<std::size_t>(online < cap ? online : cap);
}

JobScheduler::~JobScheduler() noexcept { shutdown(); }

auto JobScheduler::init(std::uint32_t* pinned_out,
                        std::size_t pinned_cap) noexcept -> bool {
  if (sched_ != nullptr) {
    return false;
  }
  const std::uint32_t online = OnlineCpuCount();
  // Clamp, never fail: on many-logical-CPU machines the pool is capped at
  // the pinning table size instead of refusing to start.
  const std::uint32_t workers =
      (online > 1 ? online - 1 : 1) > static_cast<std::uint32_t>(kMaxWorkers)
          ? static_cast<std::uint32_t>(kMaxWorkers)
          : (online > 1 ? online - 1 : 1);

  std::uint32_t cores[kMaxWorkers];
  const std::size_t ncores = PhysicalCores(cores, kMaxWorkers);
  // Unique assignment: first-sibling cores first (skipping index 0 when the
  // pool is smaller than the machine so the main thread keeps its own
  // core), then remaining allowed logical CPUs. Never reuse a CPU while an
  // unused allowed one remains; unfilled slots stay kInvalidCpu (visible).
  for (std::uint32_t i = 0; i < workers; ++i) {
    pinned_[i] = kInvalidCpu;
    g_pinned[i].store(kInvalidCpu, std::memory_order_relaxed);
  }
  std::uint32_t assigned = 0;
  auto try_assign = [&](std::uint32_t cpu) {
    if (assigned >= workers || !CpuAllowed(cpu)) {
      return;
    }
    for (std::uint32_t k = 0; k < assigned; ++k) {
      if (pinned_[k] == cpu) {
        return;
      }
    }
    pinned_[assigned] = cpu;
    g_pinned[assigned].store(cpu, std::memory_order_relaxed);
    ++assigned;
  };
  const std::size_t first = (ncores > 1 && workers < ncores) ? 1 : 0;
  for (std::size_t k = first; k < ncores; ++k) {
    try_assign(cores[k]);
  }
  if (first == 1 && ncores > 0) {
    try_assign(cores[0]);
  }
  for (std::uint32_t cpu = 0; assigned < workers && cpu < 4096; ++cpu) {
    try_assign(cpu);
  }

  sched_ = enkiNewTaskScheduler();
  if (sched_ == nullptr) {
    return false;
  }
  enkiTaskSchedulerConfig cfg = enkiGetTaskSchedulerConfig(sched_);
  cfg.numTaskThreadsToCreate = workers;
  cfg.profilerCallbacks.threadStart = ThreadStartPin;
  enkiInitTaskSchedulerWithConfig(sched_, cfg);
  worker_count_ = workers;

  for (std::uint32_t i = 0; i < workers && i < pinned_cap; ++i) {
    pinned_out[i] = pinned_[i];
  }
  return true;
}

auto JobScheduler::shutdown() noexcept -> void {
  if (sched_ == nullptr) {
    return;
  }
  // Caller must have removed the hooks + destroyed worlds first; guard
  // anyway so a stray task_new_ degrades to plain threads, never UAF.
  JobScheduler* expected = this;
  g_owner.compare_exchange_strong(expected, nullptr,
                                  std::memory_order_acq_rel);
  enkiWaitforAllAndShutdown(sched_);
  enkiDeleteTaskScheduler(sched_);
  sched_ = nullptr;
  worker_count_ = 0;
}

auto JobScheduler::pinned_cpu(std::uint32_t worker) const noexcept
    -> std::uint32_t {
  if (worker >= worker_count_) {
    return kInvalidCpu;
  }
  // Live status, not the init-time request: reflects a worker marking its
  // own slot kInvalidCpu when its pin fails.
  return g_pinned[worker].load(std::memory_order_relaxed);
}

auto InstallTaskHooks(JobScheduler* sched) noexcept -> bool {
  if (sched == nullptr || !sched->running()) {
    return false;
  }
  g_owner.store(sched, std::memory_order_release);
  ecs_os_api.task_new_ = EnkiTaskNew;
  ecs_os_api.task_join_ = EnkiTaskJoin;
  return true;
}

auto RemoveTaskHooks() noexcept -> void {
  g_owner.store(nullptr, std::memory_order_release);
  ecs_os_api.task_new_ = DefaultTaskNew;
  ecs_os_api.task_join_ = DefaultTaskJoin;
}

auto TaskHooksScheduler() noexcept -> JobScheduler* {
  return g_owner.load(std::memory_order_acquire);
}

}  // namespace engine::core

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

auto PinSelfTo(std::uint32_t cpu) noexcept -> void {
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#elif defined(_WIN32)
  DWORD_PTR mask = static_cast<DWORD_PTR>(1ULL << (cpu % 64));
  SetThreadAffinityMask(GetCurrentThread(), mask);
#elif defined(__APPLE__)
  (void)cpu;  // No public thread-affinity API; QoS left to the OS.
#endif
}

// Physical cpu ids, first-seen order. Written once at init from the
// constructing thread; workers only read their own slot.
std::uint32_t g_pinned[JobScheduler::kMaxWorkersForInit] = {};
static_assert(JobScheduler::kMaxWorkersForInit == 256,
              "worker table size drifted");

void ThreadStartPin(std::uint32_t threadnum) {
  if (threadnum < JobScheduler::kMaxWorkersForInit) {
    PinSelfTo(g_pinned[threadnum]);
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
  const std::uint32_t workers = online > 1 ? online - 1 : 1;
  if (workers > kMaxWorkers) {
    return false;
  }

  std::uint32_t cores[kMaxWorkers];
  const std::size_t ncores = PhysicalCores(cores, kMaxWorkers);
  for (std::uint32_t i = 0; i < workers; ++i) {
    // Stride across physical cores; skip thread-0's core when the pool
    // is smaller than the machine so the main thread keeps its own.
    std::uint32_t cpu = 0;
    if (ncores > 0) {
      cpu = cores[(i + 1) % ncores];
    }
    pinned_[i] = cpu;
    g_pinned[i] = cpu;
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
  return pinned_[worker];
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

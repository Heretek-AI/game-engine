// job_system_enki.cpp — M4-01: Jolt jobs on the shared enkiTS pool.
//
// Each ready Jolt job becomes one enkiTS TaskSet of size 1 whose ExecuteRange
// calls Job::Execute() (runs the function, flips done, fires the barrier
// callback) then releases the queued-execution ref. Ref-counting follows the
// upstream ThreadPool contract: CreateJob's initial ref belongs to the
// returned JobHandle (released when Jolt drops it); QueueJob takes one extra
// ref for the queued execution; FreeJob returns the slot to the fixed pool.
//
// WaitForJobs drains via the inherited barrier pump (the calling thread
// executes barrier jobs inline — this is what runs jobs when the pool is
// stopped: QueueJob drops them, same as upstream's zero-thread path), then
// ReclaimSets enkiWaitForTaskSet's each launched set before deleting it
// (delete-without-wait is a use-after-free against in-flight worker TUs).
// No malloc/new on the frame path after Init: job slots come from the fixed
// pool, the pending list is reserved at Init, enkiTS sets come from enkiTS'
// internal pool. Pool exhaustion fails closed (drop + inline at Wait).
#include "engine/physics/job_system_enki.hpp"

#include <cstdint>
#include <new>
#include <utility>

#include "TaskScheduler_c.h"

namespace engine::physics {

EnkiJobSystem::~EnkiJobSystem() {
  if (sched_ != nullptr && sched_->native() != nullptr) {
    for (auto& slot : slots_) {
      if (slot.task_set != nullptr) {
        enkiDeleteTaskSet(sched_->native(), slot.task_set);
        slot.task_set = nullptr;
      }
    }
  }
}

auto EnkiJobSystem::RunJob(std::uint32_t start, std::uint32_t end,
                           std::uint32_t threadnum, void* args) -> void {
  (void)start;
  (void)end;
  (void)threadnum;
  auto* job = static_cast<Job*>(args);  // NOLINT: member can name protected Job
  if (job == nullptr) {
    return;
  }
  job->Execute();
  job->Release();  // queued-execution ref (see QueueJob)
}

auto EnkiJobSystem::Init(JPH::uint max_barriers,
                         JPH::uint max_jobs_per_barrier) noexcept -> bool {
  if (sched_ == nullptr || !sched_->running()) {
    return false;
  }
  if (max_barriers == 0 || max_jobs_per_barrier == 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (!slots_.empty()) {
    return false;  // already initialized; single-shot like PageArena::init
  }
  const std::uint64_t total =
      static_cast<std::uint64_t>(max_barriers) * max_jobs_per_barrier;
  if (total > static_cast<std::uint64_t>(SIZE_MAX / sizeof(Slot)) ||
      total > static_cast<std::uint64_t>(0xFFFFFFFU)) {
    return false;
  }
  JobSystemWithBarrier::Init(max_barriers);
  slots_.resize(static_cast<std::size_t>(total));
  for (std::size_t i = 0; i < total; ++i) {
    slots_[i].task_set = enkiCreateTaskSet(sched_->native(), RunJob);
  }
  free_stack_.reserve(static_cast<std::size_t>(total));
  for (JPH::uint32 i = static_cast<JPH::uint32>(total); i > 0; --i) {
    free_stack_.push_back(i - 1);
  }
  pending_.reserve(static_cast<std::size_t>(total));
  return true;
}

auto EnkiJobSystem::GetMaxConcurrency() const -> int {
  if (sched_ == nullptr || !sched_->running()) {
    return 1;
  }
  return static_cast<int>(sched_->worker_count()) + 1;
}

auto EnkiJobSystem::CreateJob(const char* inName, JPH::ColorArg inColor,
                              const JobFunction& inJobFunction,
                              JPH::uint32 inNumDependencies) -> JobHandle {
  std::lock_guard<std::mutex> lock(mutex_);
  if (free_stack_.empty()) {
    return JobHandle();  // pool exhausted: Jolt treats invalid handle as
                         // inline-at-Wait (documented fail-closed path)
  }
  const JPH::uint32 idx = free_stack_.back();
  free_stack_.pop_back();
  auto* job =
      new (&slots_[idx].storage) EnkiJob(inName, inColor, this, inJobFunction,
                                         inNumDependencies);  // NOLINT: pool
  static_cast<void>(idx);
  return JobHandle(job);
}

auto EnkiJobSystem::FreeJob(Job* inJob) -> void {
  if (inJob == nullptr) {
    return;
  }
  // Recover the slot index from the placement address (pool is contiguous:
  // storage is Slot[N], so every EnkiJob* from CreateJob sits at a
  // slot base). Guard the arithmetic — a foreign pointer must never
  // recycle a live slot.
  std::lock_guard<std::mutex> lock(mutex_);
  // Slot lookup in address units (CodeQL cpp/suspicious-add-sizeof:
  // never add `count * sizeof(T)` to a byte pointer — convert to
  // uintptr_t first, then do integer arithmetic).
  const auto base_addr = reinterpret_cast<std::uintptr_t>(slots_.data());
  const auto ptr_addr = reinterpret_cast<std::uintptr_t>(inJob);
  if (ptr_addr < base_addr) {
    return;
  }
  const std::size_t diff = ptr_addr - base_addr;
  const std::size_t span = slots_.size() * sizeof(Slot);
  if (diff >= span) {
    return;
  }
  if (diff % sizeof(Slot) != 0) {
    return;
  }
  const auto idx_size = diff / sizeof(Slot);
  if (idx_size > static_cast<std::size_t>(0xFFFFFFFEU)) {
    return;
  }
  const JPH::uint32 idx = static_cast<JPH::uint32>(idx_size);
  inJob->~Job();
  free_stack_.push_back(idx);
}

auto EnkiJobSystem::QueueJob(Job* inJob) -> void {
  if (inJob == nullptr || sched_ == nullptr || !sched_->running()) {
    return;  // stopped pool: job stays on the barrier, Wait() runs it inline
  }
  std::lock_guard<std::mutex> lock(mutex_);
  const auto base_addr = reinterpret_cast<std::uintptr_t>(slots_.data());
  const auto ptr_addr = reinterpret_cast<std::uintptr_t>(inJob);
  if (ptr_addr < base_addr) {
    return;
  }
  const std::size_t diff = ptr_addr - base_addr;
  const std::size_t span = slots_.size() * sizeof(Slot);
  if (diff >= span || (diff % sizeof(Slot) != 0)) {
    return;
  }
  const auto idx_size = diff / sizeof(Slot);
  if (idx_size > static_cast<std::size_t>(0xFFFFFFFEU)) {
    return;
  }
  const JPH::uint32 idx = static_cast<JPH::uint32>(idx_size);
  enkiTaskSet* set = slots_[idx].task_set;
  if (set == nullptr) {
    return;
  }
  inJob->AddRef();  // queued-execution ref, released in RunJob
  enkiAddTaskSetArgs(sched_->native(), set, inJob, 1);
  pending_.push_back(set);  // reserved at Init; no frame-path allocation
}

auto EnkiJobSystem::QueueJobs(Job** inJobs, JPH::uint inNumJobs) -> void {
  if (inJobs == nullptr || inNumJobs == 0) {
    return;
  }
  for (JPH::uint i = 0; i < inNumJobs; ++i) {
    QueueJob(inJobs[i]);
  }
}

auto EnkiJobSystem::WaitForJobs(Barrier* inBarrier) -> void {
  JobSystemWithBarrier::WaitForJobs(inBarrier);
  ReclaimSets();  // wait each launched set
}

auto EnkiJobSystem::ReclaimSets() noexcept -> void {
  std::lock_guard<std::mutex> lock(mutex_);
  if (sched_ != nullptr && sched_->running()) {
    for (enkiTaskSet* set : pending_) {
      enkiWaitForTaskSet(sched_->native(), set);  // join before barrier returns
    }
  }
  pending_.clear();
}

}  // namespace engine::physics

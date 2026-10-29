// physics_world.cpp — M4-01: PhysicsSystem facade implementation.
#include "engine/physics/physics_world.hpp"

#include <cstdlib>
#include <mutex>
#include <new>

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

namespace engine::physics {

namespace {

// Jolt's Factory::sInstance is process-global (RegisterTypes registers
// types into it). Refcount it across PhysicsWorld lifetimes so sequential
// construct/destroy cycles in one process (tests, reload) never
// double-alloc or leak, and overlapping worlds stay safe. Protected by a
// mutex so concurrent world creation never races.
std::mutex g_factory_mutex;
int g_factory_refs = 0;

auto acquire_factory() noexcept -> bool {
  std::lock_guard<std::mutex> lock(g_factory_mutex);
  if (g_factory_refs > 0) {
    ++g_factory_refs;
    return true;
  }
  JPH::RegisterDefaultAllocator();
  void* mem = std::malloc(sizeof(JPH::Factory));
  if (mem == nullptr) {
    return false;
  }
  JPH::Factory::sInstance = new (mem) JPH::Factory();  // NOLINT: init-time
  JPH::RegisterTypes();
  g_factory_refs = 1;
  return true;
}

auto release_factory() noexcept -> void {
  std::lock_guard<std::mutex> lock(g_factory_mutex);
  if (g_factory_refs <= 0) {
    return;
  }
  if (--g_factory_refs == 0 && JPH::Factory::sInstance != nullptr) {
    JPH::UnregisterTypes();
    JPH::Factory::sInstance->~Factory();
    std::free(JPH::Factory::sInstance);
    JPH::Factory::sInstance = nullptr;
  }
}

}  // namespace

PhysicsWorld::~PhysicsWorld() noexcept { destroy(); }

auto PhysicsWorld::construct(core::JobScheduler* sched,
                             core::PageArenaAllocator* arena,
                             const PhysicsWorldConfig& cfg) noexcept -> bool {
  if (system_ != nullptr || sched == nullptr || !sched->running() ||
      arena == nullptr) {
    return false;
  }
  // Jolt requires Factory::sInstance set before RegisterTypes. Refcounted
  // process-global; balanced release_factory() in destroy() + failure paths.
  if (!acquire_factory()) {
    return false;
  }

  // Jolt classes override operator new (JPH_OVERRIDE_NEW_DELETE) with no
  // nothrow form, and this TU builds -fno-exceptions — so malloc +
  // placement new, keeping the noexcept/bool contract.
  void* jobs_mem = std::malloc(sizeof(EnkiJobSystem));
  if (jobs_mem == nullptr) {
    release_factory();
    return false;
  }
  jobs_ = new (jobs_mem) EnkiJobSystem(sched);  // NOLINT: init-time only
  if (!jobs_->Init(cfg.max_barriers, cfg.max_jobs_per_barrier)) {
    jobs_->~EnkiJobSystem();
    std::free(jobs_mem);
    jobs_ = nullptr;
    release_factory();
    return false;
  }

  if (!temp_.init(arena, cfg.temp_allocator_bytes)) {
    jobs_->~EnkiJobSystem();
    std::free(jobs_);
    jobs_ = nullptr;
    release_factory();
    return false;
  }

  void* sys_mem = std::malloc(sizeof(JPH::PhysicsSystem));
  if (sys_mem == nullptr) {
    jobs_->~EnkiJobSystem();
    std::free(jobs_);
    jobs_ = nullptr;
    release_factory();
    return false;
  }
  auto* sys = new (sys_mem) JPH::PhysicsSystem();  // NOLINT: init-time
  const JPH::uint bodies = cfg.max_bodies == 0 ? 65536 : cfg.max_bodies;
  const JPH::uint mutexes =
      cfg.num_body_mutexes == 0
          ? static_cast<JPH::uint>(jobs_->GetMaxConcurrency())
          : cfg.num_body_mutexes;
  sys->Init(bodies, mutexes, cfg.max_body_pairs, cfg.max_contact_constraints,
            bp_layers_, obj_vs_bp_, obj_pair_);
  sched_ = sched;
  system_ = sys;
  return true;
}

auto PhysicsWorld::destroy() noexcept -> void {
  // Reverse-construction order: the PhysicsSystem dtor waits on in-flight
  // Jolt jobs, so tear it down before the job system it references; the temp
  // scratch belongs to the arena (forget-only, never freed here) and is
  // released last so no in-flight job can touch a forgotten region.
  // temp_.destroy() is also safe when construct never carved (no-op) and on
  // double-destroy, so a partial construct (temp carved, system malloc
  // failed) cannot leak the carve-out.
  if (system_ != nullptr) {
    if (jobs_ != nullptr) {
      jobs_->ReclaimSets();
    }
    system_->~PhysicsSystem();
    std::free(system_);  // NOLINT: teardown only, matches malloc+placement
    system_ = nullptr;
  }
  if (jobs_ != nullptr) {
    jobs_->~EnkiJobSystem();
    std::free(jobs_);  // NOLINT: teardown only, matches malloc+placement
    jobs_ = nullptr;
  }
  temp_.destroy();  // forget the arena carve-out (arena frees wholesale)
  sched_ = nullptr;
  release_factory();
}

auto PhysicsWorld::step(float delta_time, int collision_steps) noexcept
    -> bool {
  if (system_ == nullptr || jobs_ == nullptr) {
    return false;
  }
  const JPH::EPhysicsUpdateError err =
      system_->Update(delta_time, collision_steps, &temp_, jobs_);
  return err == JPH::EPhysicsUpdateError::None;
}

auto PhysicsWorld::body_count() const noexcept -> std::uint32_t {
  if (system_ == nullptr) {
    return 0;
  }
  return static_cast<std::uint32_t>(system_->GetNumBodies());
}

auto PhysicsWorld::batch_sched() const noexcept -> void* {
  if (system_ == nullptr || sched_ == nullptr || !sched_->running()) {
    return nullptr;
  }
  return sched_->native();
}

auto PhysicsWorld::batch_workers() const noexcept -> std::uint32_t {
  if (system_ == nullptr || sched_ == nullptr || !sched_->running()) {
    return 0;
  }
  return sched_->worker_count();
}

}  // namespace engine::physics

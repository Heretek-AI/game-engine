// physics_world.cpp — M4-01: PhysicsSystem facade implementation.
#include "engine/physics/physics_world.hpp"

#include <cstdlib>
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
// double-alloc or leak, and overlapping worlds (not currently created,
// but cheap to support) stay safe. malloc+placement throughout — this TU
// builds -fno-exceptions, and JPH::Factory overrides operator new with no
// nothrow form.
int g_factory_refs = 0;

auto acquire_factory() noexcept -> bool {
  if (g_factory_refs > 0) {
    ++g_factory_refs;
    return true;
  }
  void* mem = std::malloc(sizeof(JPH::Factory));
  if (mem == nullptr) {
    return false;
  }
  JPH::Factory::sInstance = new (mem) JPH::Factory();  // NOLINT: init-time
  g_factory_refs = 1;
  return true;
}

auto release_factory() noexcept -> void {
  if (g_factory_refs <= 0) {
    return;
  }
  if (--g_factory_refs == 0 && JPH::Factory::sInstance != nullptr) {
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
  JPH::RegisterDefaultAllocator();
  // Jolt requires Factory::sInstance set before RegisterTypes: types
  // self-register with the factory singleton (PhysicsSystem::Init faults
  // without it — observed segfault). Refcounted process-global; balanced
  // release_factory() in destroy() + every construct() failure path below.
  if (!acquire_factory()) {
    return false;
  }
  JPH::RegisterTypes();

  // Jolt classes override operator new (JPH_OVERRIDE_NEW_DELETE) with no
  // nothrow form, and this TU builds -fno-exceptions — so malloc +
  // placement new (M1-03 FlecsTask pattern), keeping the noexcept/bool
  // contract. Matches explicit-dtor + free in destroy().
  void* jobs_mem = std::malloc(sizeof(EnkiJobSystem));
  if (jobs_mem == nullptr) {
    JPH::UnregisterTypes();
    release_factory();
    return false;
  }
  jobs_ = new (jobs_mem) EnkiJobSystem(sched);  // NOLINT: init-time only
  if (!jobs_->Init(cfg.max_barriers, cfg.max_jobs_per_barrier)) {
    jobs_->~EnkiJobSystem();
    std::free(jobs_mem);
    jobs_ = nullptr;
    JPH::UnregisterTypes();
    release_factory();
    return false;
  }

  if (!temp_.init(arena, cfg.temp_allocator_bytes)) {
    jobs_->~EnkiJobSystem();
    std::free(jobs_);
    jobs_ = nullptr;
    JPH::UnregisterTypes();
    release_factory();
    return false;
  }

  void* sys_mem = std::malloc(sizeof(JPH::PhysicsSystem));
  if (sys_mem == nullptr) {
    jobs_->~EnkiJobSystem();
    std::free(jobs_);
    jobs_ = nullptr;
    JPH::UnregisterTypes();
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
  if (system_ != nullptr) {
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
  JPH::UnregisterTypes();
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

}  // namespace engine::physics

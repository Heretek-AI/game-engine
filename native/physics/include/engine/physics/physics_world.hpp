// physics_world.hpp — M4-01: PhysicsSystem facade (Jolt init/step/teardown).
//
// Owns: Jolt RegisterTypes + Factory, EnkiJobSystem (shared enkiTS pool),
// ArenaTempAllocator (fixed bump region carved from the caller's
// PageArenaAllocator — the arena must outlive this world), BPLayer/filter
// singletons, and the JPH::PhysicsSystem itself. Frame path: step(dt) ->
// PhysicsSystem::Update. No malloc/new on the frame path after construct
// (fixed pools, bump temp; the two init-time news are documented in the
// .cpp).
#pragma once

#include "engine/core/engine_core_export.hpp"

#include <cstddef>
#include <cstdint>

#include "engine/core/job_scheduler.hpp"
#include "engine/core/page_arena_allocator.hpp"
#include "engine/physics/components.hpp"
#include "engine/physics/job_system_enki.hpp"
#include "engine/physics/layers.hpp"
#include "engine/physics/temp_allocator_arena.hpp"

namespace JPH {
class PhysicsSystem;
}  // namespace JPH

namespace engine::physics {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251 4275)  // STL members + Jolt non-dll bases: same-toolchain, OBJECT lib linked into engine_core.
#endif

struct PhysicsWorldConfig {
  std::uint32_t max_bodies{65536};
  std::uint32_t num_body_mutexes{0};  // 0 => job-system concurrency
  std::uint32_t max_body_pairs{65536};
  std::uint32_t max_contact_constraints{10240};
  std::size_t temp_allocator_bytes{32U << 20};  // 32 MiB bump region
  std::uint32_t max_barriers{8};
  std::uint32_t max_jobs_per_barrier{kDefaultMaxJobsPerBarrier};
};

class ENGINE_CORE_API PhysicsWorld {
 public:
  PhysicsWorld() noexcept = default;
  PhysicsWorld(const PhysicsWorld&) = delete;
  auto operator=(const PhysicsWorld&) -> PhysicsWorld& = delete;
  ~PhysicsWorld() noexcept;

  // Scheduler must be running and arena must be initialized; both must
  // outlive this world. Registers Jolt types, builds job system (shared
  // enkiTS pool) + temp allocator (carve-out of `arena`) +
  // PhysicsSystem::Init.
  auto construct(core::JobScheduler* sched, core::PageArenaAllocator* arena,
                 const PhysicsWorldConfig& cfg = {}) noexcept -> bool;
  auto destroy() noexcept -> void;

  // Fixed-step update (called from the M4-02 sync pipeline).
  auto step(float delta_time, int collision_steps = 1) noexcept -> bool;

  [[nodiscard]] auto valid() const noexcept -> bool { return system_ != nullptr; }
  [[nodiscard]] auto native() const noexcept -> JPH::PhysicsSystem* {
    return system_;
  }
  [[nodiscard]] auto jobs() noexcept -> EnkiJobSystem* { return jobs_; }
  [[nodiscard]] auto temp() noexcept -> ArenaTempAllocator* { return &temp_; }
  // Raw enkiTS scheduler + worker count for the M4-03 batch fan-out
  // (spatial_raycast_batch shards over these lanes; null/0 when invalid).
  [[nodiscard]] auto batch_sched() const noexcept -> void*;
  [[nodiscard]] auto batch_workers() const noexcept -> std::uint32_t;
  [[nodiscard]] auto body_count() const noexcept -> std::uint32_t;

 private:
  core::JobScheduler* sched_{nullptr};
  EnkiJobSystem* jobs_{nullptr};
  ArenaTempAllocator temp_;
  BPLayerInterfaceImpl bp_layers_;
  ObjectVsBroadPhaseLayerFilterImpl obj_vs_bp_;
  ObjectLayerPairFilterImpl obj_pair_;
  JPH::PhysicsSystem* system_{nullptr};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace engine::physics

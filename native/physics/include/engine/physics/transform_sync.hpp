// transform_sync.hpp — M4-02: parallel Flecs <-> Jolt transform sync.
//
// Zero-glue design: entities that want physics carry a PhysicsBody link
// component holding the Jolt BodyID (UserData on the Jolt side carries the
// Flecs entity, so both directions are plain index math — no virtual calls,
// no hash maps, no per-object allocations).
//
// Pipeline (called around PhysicsWorld::step):
//   push_to_physics()  — before step: Flecs LocalTransform -> Jolt body
//                        via SetPositionAndRotationWhenChanged (kinematic/
//                        dynamic only; statics never move).
//   pull_from_physics()— after step: active Jolt bodies -> Flecs
//                        LocalTransform columns, sharded over enkiTS.
//   ContactListenerImpl — Jolt ContactListener pushing Enter/Exit records
//                        into the CollisionEventRing from worker threads.
//
// Parallelism: both directions shard over contiguous BodyID ranges via
// enkiTS TaskSets (min-range grain keeps small worlds single-task). Body
// reads use the locking interface (per-body shared locks — required with a
// shared enkiTS pool where the waiting thread also pumps tasks); writes go
// through BodyInterface (internally locked, thread-safe by Jolt contract).
//
// Frame path: no malloc, no exceptions, noexcept throughout.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"
#include "engine/physics/collision_events.hpp"

struct ecs_world_t;
struct ecs_query_t;

namespace JPH {
class PhysicsSystem;
class BodyInterface;
class ContactListener;
}  // namespace JPH

namespace engine::physics {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251)  // raw-pointer members (private, same-toolchain OBJECT lib).
#endif

// Flecs link: which Jolt body drives this entity's LocalTransform.
// Blittable + 64-byte aligned (Flecs column rule — matches RigidBody et al).
struct alignas(64) PhysicsBody {
  std::uint32_t body_id{0xFFFFFFFFu};  // full Jolt BodyID (index+sequence via
                                       // GetIndexAndSequenceNumber), or INVALID
  std::uint8_t kinematic{0};           // 1: push Flecs->Jolt every frame
  std::uint8_t sensor{0};              // 1: trigger volume (no response)
  std::uint8_t reserved[54]{};
};

static_assert(sizeof(PhysicsBody) == 64, "PhysicsBody must be one cache line");
static_assert(alignof(PhysicsBody) == 64, "PhysicsBody alignas(64)");

// Contact listener: Jolt calls these on physics worker threads during
// Update(). Bodies are NOT safely dereferenceable in OnContactRemoved, so
// both paths cache only UserData (entity) + contact point into the ring.
class ENGINE_CORE_API ContactListenerImpl {
 public:
  ContactListenerImpl() noexcept = default;
  ContactListenerImpl(const ContactListenerImpl&) = delete;
  auto operator=(const ContactListenerImpl&) -> ContactListenerImpl& = delete;

  // Bind before PhysicsWorld init; the world registers it with Jolt.
  // Ring + body iface must outlive the listener's use inside Update().
  auto bind(CollisionEventRing* ring, const JPH::BodyInterface* iface) noexcept
      -> void;

  // Owned Jolt listener (registered via PhysicsSystem::SetContactListener).
  // Null until bind() is called.
  [[nodiscard]] auto native() const noexcept -> JPH::ContactListener* {
    return listener_;
  }

  [[nodiscard]] auto ring() const noexcept -> CollisionEventRing* {
    return ring_;
  }

 private:
  JPH::ContactListener* listener_{nullptr};
  CollisionEventRing* ring_{nullptr};
  const JPH::BodyInterface* iface_{nullptr};
};

// Options for a full sync step.
struct TransformSyncConfig {
  std::uint32_t min_range{256};  // enkiTS min grain per task (bodies)
  std::size_t event_capacity{1U << 16};  // ring slots (64k default)
};

// Sync pipeline state: owns the ring + contact listener, borrows the
// Flecs world + Jolt system + scheduler. init() BEFORE first Update;
// the PhysicsSystem must have the listener installed at ITS init.
class ENGINE_CORE_API TransformSync {
 public:
  TransformSync() noexcept = default;
  TransformSync(const TransformSync&) = delete;
  auto operator=(const TransformSync&) -> TransformSync& = delete;
  ~TransformSync() noexcept;

  // Register PhysicsBody with the Flecs world (idempotent per world) and
  // allocate the event ring. Returns false on double-init or OOM.
  auto init(ecs_world_t* world, const TransformSyncConfig& cfg = {}) noexcept
      -> bool;
  auto destroy() noexcept -> void;

  [[nodiscard]] auto valid() const noexcept -> bool {
    return world_ != nullptr;
  }
  [[nodiscard]] auto body_id() const noexcept -> std::uint64_t {
    return body_id_;
  }
  [[nodiscard]] auto ring() noexcept -> CollisionEventRing* {
    return &ring_;
  }
  [[nodiscard]] auto listener() noexcept -> ContactListenerImpl* {
    return &listener_;
  }

  // AC1 — before step: push kinematic/dynamic LocalTransform edits to Jolt
  // via BodyInterface::SetPositionAndRotationWhenChanged. Iterates the
  // (LocalTransform, PhysicsBody) query serially (Flecs iters are cheap;
  // each push is one locked Jolt call — typically a handful of bodies).
  auto push_to_physics(JPH::PhysicsSystem& system) noexcept -> std::uint32_t;

  // AC2 — after step: pull active-body transforms into Flecs
  // LocalTransform columns in parallel over enkiTS. Returns bodies synced.
  // `sched_native` is the enkiTaskScheduler* (type-erased to avoid pulling
  // enkiTS headers into this header); null => serial fallback.
  auto pull_from_physics(JPH::PhysicsSystem& system, void* sched_native,
                         std::uint32_t worker_count) noexcept -> std::uint32_t;

  // AC3 — drain pending collision events into `out` (contiguous batch).
  // Returns events written; drops are counted in ring dropped().
  auto drain_events(CollisionEvent* out, std::size_t max_out) noexcept
      -> std::size_t;

 private:
  ecs_world_t* world_{nullptr};
  std::uint64_t body_id_{0};
  TransformSyncConfig cfg_{};
  ecs_query_t* query_{nullptr};        // cached (LocalTransform, PhysicsBody)
  void* active_vec_{nullptr};        // JPH::BodyIDVector* (created in init,
                                     // reused every pull — stable capacity,
                                     // no per-frame malloc; type-erased so
                                     // this header never includes Jolt)
  void* task_set_{nullptr};          // reused enkiTaskSet* (type-erased).
                                     // Owned by US (created via
                                     // enkiCreateTaskSet): must be deleted
                                     // via enkiDeleteTaskSet BEFORE the
                                     // scheduler shuts down — but this TU
                                     // can't call it without the scheduler
                                     // alive, so destroy() takes the
                                     // scheduler for ordered teardown. If
                                     // you destroy the scheduler first, the
                                     // set leaks (one small object) but
                                     // never use-after-frees.
  CollisionEventRing ring_{};
  ContactListenerImpl listener_{};
};

#if defined(_MSC_VER)
#pragma warning(pop)  // C4251 scoped to ContactListenerImpl/TransformSync.
#endif

}  // namespace engine::physics

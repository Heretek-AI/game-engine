// joints.hpp — P6-05: physics joints as components backed by Jolt constraints.
//
// Blittable, 64-byte aligned (Flecs column rule — matches RigidBody et al).
// Joint entities reference the connected bodies by Flecs entity id; the
// JointManager resolves them to Jolt BodyIDs via the PhysicsBody link
// component (UserData on the Jolt side carries the Flecs entity).
//
// Constraint mapping (all world-space at creation):
//   FixedJoint    -> JPH::FixedConstraint      (auto-detect point)
//   HingeJoint    -> JPH::HingeConstraint      (Y hinge axis, X normal axis)
//   SliderJoint   -> JPH::SliderConstraint     (X slide axis, auto-detect)
//   DistanceJoint -> JPH::DistanceConstraint   (min/max range, hard limits)
//   SpringJoint   -> JPH::DistanceConstraint + SpringSettings (frequency/damping)
//
// JointManager lifecycle: init(flecs_world) once, then per fixed step
// sync(physics_system) creates Jolt constraints for new joint entities and
// removes constraints for dead, disabled or re-linked ones. destroy()
// removes every owned constraint from the last synced PhysicsSystem, so it
// must run while that system is still alive.
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"

struct ecs_world_t;
struct ecs_query_t;

namespace JPH {
class PhysicsSystem;
class Constraint;
template <class T>
class Ref;
}  // namespace JPH

namespace engine::physics {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)  // alignas(64) padding by design.
#endif

// Which bodies a joint connects. 0 = invalid / world anchor (static).
// Both entities must carry a PhysicsBody link with a valid body_id.
enum class JointType : std::uint32_t { kFixed = 0, kHinge = 1, kSlider = 2, kDistance = 3, kSpring = 4 };

// Fixed: rigidly welds two bodies in their current relative pose.
struct alignas(64) FixedJoint {
  std::uint64_t body_a{0};  // Flecs entity of first body (0 => invalid)
  std::uint64_t body_b{0};  // Flecs entity of second body (0 => world/static anchor)
  std::uint8_t enabled{1};
  std::uint8_t reserved[47];
};

// Hinge: single-axis rotation (door hinge) about local Y, with angle limits.
struct alignas(64) HingeJoint {
  std::uint64_t body_a{0};
  std::uint64_t body_b{0};
  float anchor_x{0.0F};  // world-space hinge point at creation
  float anchor_y{0.0F};
  float anchor_z{0.0F};
  float limits_min{-1.5707964F};  // rad, [-pi, 0]
  float limits_max{1.5707964F};   // rad, [0, pi]
  float max_friction_torque{0.0F};  // N m (0 = frictionless)
  std::uint8_t enabled{1};
  std::uint8_t reserved[23];
};

// Slider: translation along local X within [limits_min, limits_max] (meters).
struct alignas(64) SliderJoint {
  std::uint64_t body_a{0};
  std::uint64_t body_b{0};
  float anchor_x{0.0F};
  float anchor_y{0.0F};
  float anchor_z{0.0F};
  float limits_min{-1.0F};
  float limits_max{1.0F};
  std::uint8_t enabled{1};
  std::uint8_t reserved[27];
};

// Distance: keeps two anchor points within [min_distance, max_distance].
struct alignas(64) DistanceJoint {
  std::uint64_t body_a{0};
  std::uint64_t body_b{0};
  float anchor_a_x{0.0F};
  float anchor_a_y{0.0F};
  float anchor_a_z{0.0F};
  float anchor_b_x{0.0F};
  float anchor_b_y{0.0F};
  float anchor_b_z{0.0F};
  float min_distance{-1.0F};  // < 0 => use creation-time distance
  float max_distance{-1.0F};   // < 0 => use creation-time distance
  std::uint8_t enabled{1};
  std::uint8_t reserved[15];
};

// Spring: distance joint with soft spring limits (frequency + damping).
struct alignas(64) SpringJoint {
  std::uint64_t body_a{0};
  std::uint64_t body_b{0};
  float anchor_a_x{0.0F};
  float anchor_a_y{0.0F};
  float anchor_a_z{0.0F};
  float anchor_b_x{0.0F};
  float anchor_b_y{0.0F};
  float anchor_b_z{0.0F};
  float frequency{2.0F};  // Hz (> 0 enables the spring)
  float damping{0.5F};    // ratio (0 = undamped, 1 = critical)
  std::uint8_t enabled{1};
  std::uint8_t reserved[15];
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

static_assert(sizeof(FixedJoint) == 64, "FixedJoint must be one cache line");
static_assert(sizeof(HingeJoint) == 64, "HingeJoint must be one cache line");
static_assert(sizeof(SliderJoint) == 64, "SliderJoint must be one cache line");
static_assert(sizeof(DistanceJoint) == 64, "DistanceJoint must be one cache line");
static_assert(sizeof(SpringJoint) == 64, "SpringJoint must be one cache line");
static_assert(alignof(FixedJoint) == 64, "FixedJoint alignas(64)");
static_assert(alignof(HingeJoint) == 64, "HingeJoint alignas(64)");
static_assert(alignof(SliderJoint) == 64, "SliderJoint alignas(64)");
static_assert(alignof(DistanceJoint) == 64, "DistanceJoint alignas(64)");
static_assert(alignof(SpringJoint) == 64, "SpringJoint alignas(64)");

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251)  // vector<Constraint*> member, same-toolchain OBJECT lib.
#endif

// JointManager: owns the Flecs->Jolt constraint lifecycle for P6-05 joints.
//
// Model: each joint *entity* carries exactly one of the five joint
// components. sync() scans the five joint tables, creates a Jolt
// TwoBodyConstraint for every (entity, body_a, body_b) triple it has not
// seen, and removes Jolt constraints whose joint entity died or whose
// component was removed, disabled, or now links different bodies.
// Ownership: constraints are reference counted (JPH::Ref). The manager keeps
// one Ref per entry and the PhysicsSystem keeps its own after AddConstraint;
// removal drops both and Jolt frees the object. Never delete one directly.
//
// Frame path: sync() runs once per fixed step (serial; constraint counts
// are small). Component ids and the five per-type queries are created once
// in init(). Storage is a malloc'd entry array plus a parallel Ref array,
// reserved at init and doubled with realloc when full (amortized, not per
// frame in steady state).
class ENGINE_CORE_API JointManager {
 public:
  JointManager() noexcept = default;
  JointManager(const JointManager&) = delete;
  auto operator=(const JointManager&) -> JointManager& = delete;
  ~JointManager() noexcept;

  // Register the five joint components with the Flecs world (explicit
  // size/align, same pattern as TransformSync::init). Returns false if
  // already initialized or on failure; call destroy() before re-init.
  auto init(ecs_world_t* world) noexcept -> bool;
  auto destroy() noexcept -> void;

  [[nodiscard]] auto valid() const noexcept -> bool { return world_ != nullptr; }
  [[nodiscard]] auto constraint_count() const noexcept -> std::uint32_t {
    return count_;
  }

  // Reconcile Jolt constraints with the joint tables. Returns live
  // constraint count. Safe to call every fixed step.
  auto sync(JPH::PhysicsSystem& system) noexcept -> std::uint32_t;

  // Query a hinge's current angle (radians) by joint entity. Returns
  // false when the entity has no live hinge constraint.
  auto hinge_angle(JPH::PhysicsSystem& system, std::uint64_t joint_entity,
                   float* out_angle) noexcept -> bool;

 private:
  struct Entry {
    std::uint64_t joint_entity{0};
    JointType type{JointType::kFixed};
    // Ref-counted Jolt constraint handle. TwoBodyConstraintSettings::Create
    // returns a raw pointer with refcount 0; AddConstraint AddRefs into the
    // ConstraintManager list (count 1). The manager keeps its Ref on the
    // side in a parallel array (see below) — never a raw owning pointer.
    // remove_entry Removes from the system (dropping the manager's Ref,
    // freeing at zero) then clears the manager-side Ref. NO raw delete
    // EVER (raw delete races the manager's Ref: double-free on remove,
    // dangling walk in GetActiveConstraints).
    JPH::Constraint* constraint{nullptr};
    // Jolt body ids the constraint was built against; a link change
    // rebuilds it.
    std::uint32_t body_a{0xFFFFFFFFu};
    std::uint32_t body_b{0xFFFFFFFFu};
  };
  // Manager-side Refs, parallel to entries_: Ref<Constraint> is non-trivial
  // (needs Jolt headers for the full type) while Entry must stay a POD for
  // the malloc/memset/realloc array. refs_[i] holds OUR Ref for
  // entries_[i].constraint; both arrays swap-remove together.
  JPH::Ref<JPH::Constraint>* refs_{nullptr};

  auto remove_entry(JPH::PhysicsSystem& system, std::size_t idx) noexcept -> void;
  auto find_entry(std::uint64_t joint_entity) noexcept -> std::int64_t;

  ecs_world_t* world_{nullptr};
  // Last system passed to sync(); destroy() removes constraints from it.
  JPH::PhysicsSystem* system_{nullptr};
  // Created once in init(): component ids by JointType, and one query per
  // joint type (same order).
  std::uint64_t comp_ids_[5]{};
  ecs_query_t* queries_[5]{};
  std::uint64_t physics_body_id_{0};
  Entry* entries_{nullptr};  // malloc'd array (init-time only), capacity below
  std::uint32_t capacity_{0};
  std::uint32_t count_{0};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace engine::physics

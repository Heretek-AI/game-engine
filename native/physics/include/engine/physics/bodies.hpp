// bodies.hpp — P2-04: native Flecs -> Jolt body lifecycle.
//
// Creates Jolt bodies for entities carrying a P2-04 RigidBody plus exactly
// one collider (box / sphere / capsule), destroys them on removal, and
// leaves steady-state stepping + pose writeback to the existing M4-02
// TransformSync pipeline (push_to_physics / pull_from_physics around
// PhysicsWorld::step).
//
// Component contract (mirrors managed/Engine.Runtime/Physics.cs, P2-04):
//   RigidBody     { float mass; uint32 motion_type; /*0 static,1 kinematic,2
//                   dynamic*/ float linear_damping; float angular_damping; }
//                   16 bytes, align 4.
//   BoxCollider   { float half_extents[3]; }  12 bytes, align 4.
//   SphereCollider{ float radius; }            4 bytes, align 4.
//   CapsuleCollider{ float radius; float half_height; }  8 bytes, align 4.
//
// Registration rule (spec section 6 gotcha 20): every shared component is
// registered ONLY through GenericSystems::register_component under the C#
// full name ("Engine.Runtime.RigidBody" etc.) with the same name + size +
// align. register_component is idempotent, so calling BodySystem::init
// twice (or after a C# registration of the same layout) returns the same
// id instead of aborting. This TU never calls ecs_component_init for these
// names (Flecs aborts the process on a size-mismatched re-register).
//
// Frame path: sync() runs once per fixed step (serial; body counts are
// small). It reconciles Jolt bodies with the component tables and then
// runs the fixed-step pipeline: push (kinematic pose edits) -> step ->
// pull (active-body pose writeback). No malloc on the frame path in steady
// state: the entry list doubles amortized via realloc and Jolt body ids are
// tracked in a parallel array.
//
// Lifetime: entries hold the full Jolt BodyID (index+sequence). Removal
// calls BodyInterface::RemoveBody + DestroyBody. destroy() releases only
// the entry list + query; owned Jolt bodies die with their PhysicsSystem,
// so destroy the PhysicsWorld before re-initializing a BodySystem on it.
//
// Threading: sync() runs on the fixed-update thread only. Jolt body
// creation/removal goes through BodyInterface (internally locked,
// thread-safe by Jolt contract). No enkiTS sharding here (counts are
// small); the pull phase fans out internally via TransformSync.
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"

struct ecs_world_t;
struct ecs_query_t;

namespace JPH {
class PhysicsSystem;
}  // namespace JPH

namespace engine::physics {

// P2-04 motion encoding (matches the C# RigidBody.motion_type contract).
// 0 = static (non-moving layer, never sleeps off), 1 = kinematic
// (moves via push, no gravity response), 2 = dynamic (full simulation).
enum class BodyMotion : std::uint32_t { kStatic = 0, kKinematic = 1, kDynamic = 2 };

// Native wire mirror of the C# RigidBody (16 bytes, align 4). Field order
// matches the managed struct exactly so the whole-struct Set path is a
// plain copy. Default member initializers only document intent: Flecs
// storage is raw and components added without Set carry whatever the
// writer stored (Set path) or zeros.
struct BodyInterop {
  float mass{1.0F};
  std::uint32_t motion_type{2};
  float linear_damping{0.05F};
  float angular_damping{0.05F};
};

// Native wire mirror of the C# BoxCollider (12 bytes, align 4).
struct BoxInterop {
  float half_extents[3]{0.5F, 0.5F, 0.5F};
};

// Native wire mirror of the C# SphereCollider (4 bytes, align 4).
struct SphereInterop {
  float radius{0.5F};
};

// Native wire mirror of the C# CapsuleCollider (8 bytes, align 4).
struct CapsuleInterop {
  float radius{0.3F};
  float half_height{0.5F};
};

static_assert(sizeof(BodyInterop) == 16, "RigidBody wire size drift vs C#");
static_assert(alignof(BodyInterop) == 4, "RigidBody wire align drift vs C#");
static_assert(sizeof(BoxInterop) == 12, "BoxCollider wire size drift vs C#");
static_assert(alignof(BoxInterop) == 4, "BoxCollider wire align drift vs C#");
static_assert(sizeof(SphereInterop) == 4,
              "SphereCollider wire size drift vs C#");
static_assert(alignof(SphereInterop) == 4,
              "SphereCollider wire align drift vs C#");
static_assert(sizeof(CapsuleInterop) == 8,
              "CapsuleCollider wire size drift vs C#");
static_assert(alignof(CapsuleInterop) == 4,
              "CapsuleCollider wire align drift vs C#");

// BodySystem: owns the Flecs -> Jolt body lifecycle for P2-04 bodies.
//
// Model: each body *entity* carries exactly one RigidBody plus exactly
// one of the three collider components. sync() reconciles: it destroys
// Jolt bodies whose entity died, lost RigidBody, lost its collider, or
// changed collider kind, then creates Jolt bodies for entities not yet
// tracked. Creation reads the spawn pose from the entity's LocalTransform
// (when registered; otherwise origin/identity) and tags the body UserData
// with the Flecs entity id so TransformSync::pull maps it back.
class ENGINE_CORE_API BodySystem {
 public:
  BodySystem() noexcept = default;
  BodySystem(const BodySystem&) = delete;
  auto operator=(const BodySystem&) -> BodySystem& = delete;
  ~BodySystem() noexcept;

  // Register the four P2-04 components via GenericSystems::
  // register_component under their C# full names (idempotent) and cache
  // the PhysicsBody link id + per-shape queries. Returns false when the
  // world is null, already initialized, or any registration fails.
  // Safe to call when C# already registered the same layouts: the helper
  // returns the existing ids (gotcha 20).
  auto init(ecs_world_t* world) noexcept -> bool;
  auto destroy() noexcept -> void;

  [[nodiscard]] auto valid() const noexcept -> bool { return world_ != nullptr; }
  [[nodiscard]] auto body_count() const noexcept -> std::uint32_t {
    return count_;
  }

  // Reconcile Jolt bodies with the component tables, then run the
  // fixed-step pipeline (push -> step -> pull) when `advance` is true.
  // `fixed_dt` is the P2-03 fixed step (1/50 s). `world` owns the Jolt
  // system (step runs through it so temp allocators + job lanes stay
  // correct); `sync` is the world's TransformSync (borrowed, must outlive
  // use in this call). Returns the live body count. Safe to call every
  // fixed step.
  auto sync(class PhysicsWorld& world, class TransformSync& sync,
            float fixed_dt, bool advance) noexcept -> std::uint32_t;

 private:
  struct Entry {
    std::uint64_t entity{0};    // Flecs entity owning the body
    std::uint32_t body_id{0xFFFFFFFFu};  // full Jolt BodyID, INVALID = none
    std::uint32_t shape_kind{0};  // 1 = box, 2 = sphere, 3 = capsule
    std::uint8_t kinematic{0};  // 1 = kinematic motion type (link flag)
  };

  static constexpr std::uint32_t kInvalidBodyId = 0xFFFFFFFFu;

  auto find_entry(std::uint64_t entity) const noexcept -> std::int32_t;
  auto remove_entry(JPH::PhysicsSystem& system, std::uint32_t index) noexcept
      -> void;
  auto create_entry(JPH::PhysicsSystem& system, std::uint64_t entity,
                    const BodyInterop& body, std::uint32_t shape_kind,
                    const void* shape_data) noexcept -> bool;

  ecs_world_t* world_{nullptr};
  std::uint64_t rigidbody_id_{0};
  std::uint64_t box_id_{0};
  std::uint64_t sphere_id_{0};
  std::uint64_t capsule_id_{0};
  std::uint64_t physics_body_id_{0};
  ecs_query_t* rigidbody_query_{nullptr};
  Entry* entries_{nullptr};
  std::uint32_t count_{0};
  std::uint32_t capacity_{0};
};

}  // namespace engine::physics

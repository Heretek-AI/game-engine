// bodies.cpp — P2-04: native Flecs -> Jolt body lifecycle; see header.
//
// Registration: the four P2-04 components go through
// GenericSystems::register_component (idempotent, gotcha 20) — this TU
// never calls ecs_component_init for the shared names.
//
// Creation: one RigidBody + exactly one collider per entity. Shape choice
// prefers box, then sphere, then capsule when several colliders are
// present (C# authors exactly one; the preference only keeps malformed
// entities deterministic). Density 1000 kg/m^3 with the C# mass applied
// as an override keeps the acceptance drop (r=0.5 sphere from y=5)
// mass-independent while still honoring per-body mass.
//
// Motion mapping: 0 static -> EMotionType::Static on the NON_MOVING
// layer (never sleeps off, cheapest broadphase); 1 kinematic ->
// EMotionType::Kinematic on MOVING with gravity factor 0 (pose driven by
// push_to_physics via the PhysicsBody.kinematic link flag); 2 dynamic ->
// EMotionType::Dynamic on MOVING (full simulation). Statics are created
// inactive; non-statics are activated on creation so the acceptance drop
// starts falling on the first step.
//
// Removal: entity death, RigidBody loss, collider loss, or collider-kind
// change all drop the entry (RemoveBody + DestroyBody). Flecs component
// removal without entity death is covered because the tracked entity no
// longer matches the expected (RigidBody, collider) pair on the next
// sync — the kind check compares against the live tables each call.
//
// Jolt headers ONLY in this TU (wrapper interface include: SYSTEM +
// -Wno-conversion), so -Wconversion -Werror never fires inside Jolt.
#include "engine/physics/bodies.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

// IWYU: Jolt kernel headers stay in this TU (see file comment).
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/EActivation.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include "engine/ecs/ecs_render_components.hpp"
#include "engine/ecs/transform.hpp"
#include "engine/physics/layers.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/physics/transform_sync.hpp"
#include "flecs.h"

namespace engine::physics {

namespace {

// Initial entry capacity: init-time malloc, doubled amortized (same model
// as JointManager — not per-frame in steady state).
constexpr std::uint32_t kInitialCapacity = 64;

// Shape kinds tracked per entry (match the header comment).
constexpr std::uint32_t kShapeBox = 1;
constexpr std::uint32_t kShapeSphere = 2;
constexpr std::uint32_t kShapeCapsule = 3;

// Clamp helper: keeps malformed C# values (negative/NaN extents, zero
// mass) from reaching Jolt shape ctors (which assert on degenerate
// input). Falls back to the component default.
auto ClampPositive(float v, float fallback) noexcept -> float {
  if (!(v > 0.0F)) {
    return fallback;
  }
  return v;
}

// Resolve the collider kind + payload for an entity from the live tables.
// Prefers box, then sphere, then capsule. Returns 0 when no collider is
// present (not a body entity).
auto ResolveShape(ecs_world_t* world, std::uint64_t entity,
                  std::uint64_t box_id, std::uint64_t sphere_id,
                  std::uint64_t capsule_id, const void** out_data) noexcept
    -> std::uint32_t {
  const auto eid = static_cast<ecs_entity_t>(entity);
  if (box_id != 0) {
    const void* data = ecs_get_id(world, eid, static_cast<ecs_id_t>(box_id));
    if (data != nullptr) {
      *out_data = data;
      return kShapeBox;
    }
  }
  if (sphere_id != 0) {
    const void* data =
        ecs_get_id(world, eid, static_cast<ecs_id_t>(sphere_id));
    if (data != nullptr) {
      *out_data = data;
      return kShapeSphere;
    }
  }
  if (capsule_id != 0) {
    const void* data =
        ecs_get_id(world, eid, static_cast<ecs_id_t>(capsule_id));
    if (data != nullptr) {
      *out_data = data;
      return kShapeCapsule;
    }
  }
  return 0;
}

// Read the spawn pose from LocalTransform (identity fallback when the
// component is absent — e.g. TestFixture registers builtins lazily).
// Rotation passes through too: a tilted static ramp or pre-rotated
// dynamic body keeps its authored orientation (CodeRabbit review).
auto SpawnPose(ecs_world_t* world, std::uint64_t entity, float* px, float* py,
               float* pz, float* q) noexcept -> void {
  *px = 0.0F;
  *py = 0.0F;
  *pz = 0.0F;
  q[0] = 0.0F;
  q[1] = 0.0F;
  q[2] = 0.0F;
  q[3] = 1.0F;
  const ecs_entity_t local_id = ecs_lookup(world, "LocalTransform");
  if (local_id == 0) {
    return;
  }
  const auto* t = static_cast<const engine::ecs::LocalTransform*>(
      ecs_get_id(world, static_cast<ecs_entity_t>(entity), local_id));
  if (t != nullptr) {
    *px = t->px;
    *py = t->py;
    *pz = t->pz;
    q[0] = t->qx;
    q[1] = t->qy;
    q[2] = t->qz;
    q[3] = t->qw;
  }
}

}  // namespace

BodySystem::~BodySystem() noexcept { destroy(); }

auto BodySystem::init(ecs_world_t* world) noexcept -> bool {
  if (world_ != nullptr || world == nullptr) {
    return false;
  }
  // Idempotent shared registration (gotcha 20): same name + size + align
  // returns the existing id when C# (or a second init) got here first.
  // RegisterRenderComponent is the shared helper GenericSystems::
  // register_component delegates to (same lookup-or-init semantics).
  const std::uint64_t rigid = engine::ecs::RegisterRenderComponent(
      world, "Engine.Runtime.RigidBody", sizeof(BodyInterop),
      alignof(BodyInterop));
  const std::uint64_t box = engine::ecs::RegisterRenderComponent(
      world, "Engine.Runtime.BoxCollider", sizeof(BoxInterop),
      alignof(BoxInterop));
  const std::uint64_t sphere = engine::ecs::RegisterRenderComponent(
      world, "Engine.Runtime.SphereCollider", sizeof(SphereInterop),
      alignof(SphereInterop));
  const std::uint64_t capsule = engine::ecs::RegisterRenderComponent(
      world, "Engine.Runtime.CapsuleCollider", sizeof(CapsuleInterop),
      alignof(CapsuleInterop));
  if (rigid == 0 || box == 0 || sphere == 0 || capsule == 0) {
    return false;
  }
  rigidbody_id_ = rigid;
  box_id_ = box;
  sphere_id_ = sphere;
  capsule_id_ = capsule;
  // The PhysicsBody link is owned by TransformSync (M4-02): resolve when
  // present, retry lazily in sync() when sync.init ran after us.
  physics_body_id_ =
      static_cast<std::uint64_t>(ecs_lookup(world, "PhysicsBody"));

  ecs_query_desc_t qd{};
  qd.terms[0].id = static_cast<ecs_entity_t>(rigidbody_id_);
  qd.cache_kind = EcsQueryCacheAuto;
  rigidbody_query_ = ecs_query_init(world, &qd);
  if (rigidbody_query_ == nullptr) {
    rigidbody_id_ = 0;
    box_id_ = 0;
    sphere_id_ = 0;
    capsule_id_ = 0;
    physics_body_id_ = 0;
    return false;
  }
  void* mem = std::malloc(sizeof(Entry) * kInitialCapacity);
  if (mem == nullptr) {
    ecs_query_fini(rigidbody_query_);
    rigidbody_query_ = nullptr;
    rigidbody_id_ = 0;
    box_id_ = 0;
    sphere_id_ = 0;
    capsule_id_ = 0;
    physics_body_id_ = 0;
    return false;
  }
  entries_ = static_cast<Entry*>(mem);
  for (std::uint32_t i = 0; i < kInitialCapacity; ++i) {
    new (&entries_[i]) Entry{};
  }
  capacity_ = kInitialCapacity;
  count_ = 0;
  world_ = world;
  return true;
}

auto BodySystem::destroy() noexcept -> void {
  // Bodies are removed by the caller via sync teardown while the system
  // is alive; here we only release the entry list + query (same split as
  // JointManager: owned Jolt objects die with their PhysicsSystem).
  if (rigidbody_query_ != nullptr) {
    ecs_query_fini(rigidbody_query_);
    rigidbody_query_ = nullptr;
  }
  if (entries_ != nullptr) {
    // Entries are POD (no Jolt Refs held — bodies die with the system),
    // so plain free suffices (no element dtors to run).
    std::free(entries_);  // NOLINT: teardown only
    entries_ = nullptr;
  }
  count_ = 0;
  capacity_ = 0;
  world_ = nullptr;
  rigidbody_id_ = 0;
  box_id_ = 0;
  sphere_id_ = 0;
  capsule_id_ = 0;
  physics_body_id_ = 0;
}

auto BodySystem::find_entry(std::uint64_t entity) const noexcept
    -> std::int32_t {
  for (std::uint32_t i = 0; i < count_; ++i) {
    if (entries_[i].entity == entity) {
      return static_cast<std::int32_t>(i);
    }
  }
  return -1;
}

auto BodySystem::remove_entry(JPH::PhysicsSystem& system,
                              std::uint32_t index) noexcept -> void {
  if (index >= count_) {
    return;
  }
  Entry& e = entries_[index];
  if (e.body_id != kInvalidBodyId) {
    const JPH::BodyID bid(e.body_id);
    JPH::BodyInterface& iface = system.GetBodyInterface();
    iface.RemoveBody(bid);
    iface.DestroyBody(bid);
  }
  // Clear the PhysicsBody link so a re-created body never reads a stale
  // id (the link component is owned by TransformSync — only mutate the
  // row, never the registration).
  if (physics_body_id_ != 0 && e.entity != 0 &&
      ecs_is_alive(world_, static_cast<ecs_entity_t>(e.entity))) {
    auto* link = static_cast<PhysicsBody*>(ecs_get_mut_id(
        world_, static_cast<ecs_entity_t>(e.entity),
        static_cast<ecs_entity_t>(physics_body_id_)));
    if (link != nullptr) {
      link->body_id = kInvalidBodyId;
      link->kinematic = 0;
    }
  }
  entries_[index] = entries_[count_ - 1];
  entries_[count_ - 1] = Entry{};
  --count_;
}

auto BodySystem::create_entry(JPH::PhysicsSystem& system,
                              std::uint64_t entity, const BodyInterop& body,
                              std::uint32_t shape_kind,
                              const void* shape_data) noexcept -> bool {
  if (shape_data == nullptr || shape_kind == 0) {
    return false;
  }
  if (count_ >= capacity_) {
    const std::uint32_t ncap = capacity_ * 2U;
    if (ncap <= capacity_) {
      return false;
    }
    void* mem = std::realloc(entries_, sizeof(Entry) * ncap);
    if (mem == nullptr) {
      return false;
    }
    entries_ = static_cast<Entry*>(mem);
    for (std::uint32_t i = capacity_; i < ncap; ++i) {
      new (&entries_[i]) Entry{};
    }
    capacity_ = ncap;
  }

  // Build the shape from the wire payload (clamped: Jolt asserts on
  // degenerate extents). Box uses the C# half extents directly; sphere
  // the radius; capsule (radius, cylinder half height).
  JPH::Ref<JPH::Shape> shape;
  if (shape_kind == kShapeBox) {
    const auto* box = static_cast<const BoxInterop*>(shape_data);
    const float hx = ClampPositive(box->half_extents[0], 0.5F);
    const float hy = ClampPositive(box->half_extents[1], 0.5F);
    const float hz = ClampPositive(box->half_extents[2], 0.5F);
    const JPH::BoxShapeSettings settings(JPH::Vec3(hx, hy, hz));
    const JPH::Shape::ShapeResult res = settings.Create();
    if (!res.IsValid()) {
      return false;
    }
    shape = res.Get();
  } else if (shape_kind == kShapeSphere) {
    const auto* sphere = static_cast<const SphereInterop*>(shape_data);
    const float radius = ClampPositive(sphere->radius, 0.5F);
    const JPH::SphereShapeSettings settings(radius);
    const JPH::Shape::ShapeResult res = settings.Create();
    if (!res.IsValid()) {
      return false;
    }
    shape = res.Get();
  } else if (shape_kind == kShapeCapsule) {
    const auto* capsule = static_cast<const CapsuleInterop*>(shape_data);
    const float radius = ClampPositive(capsule->radius, 0.3F);
    const float half_height = ClampPositive(capsule->half_height, 0.5F);
    const JPH::CapsuleShapeSettings settings(half_height, radius);
    const JPH::Shape::ShapeResult res = settings.Create();
    if (!res.IsValid()) {
      return false;
    }
    shape = res.Get();
  } else {
    return false;
  }

  const std::uint32_t motion = body.motion_type > 2U ? 2U : body.motion_type;
  const bool is_static = motion == 0U;
  const bool is_kinematic = motion == 1U;
  const JPH::EMotionType mt = is_static        ? JPH::EMotionType::Static
                              : is_kinematic   ? JPH::EMotionType::Kinematic
                                               : JPH::EMotionType::Dynamic;
  const JPH::ObjectLayer layer = is_static ? ObjectLayers::kNonMoving
                                           : ObjectLayers::kMoving;
  float sx = 0.0F;
  float sy = 0.0F;
  float sz = 0.0F;
  float q[4]{0.0F, 0.0F, 0.0F, 1.0F};
  SpawnPose(world_, entity, &sx, &sy, &sz, q);
  JPH::BodyCreationSettings bcs(shape, JPH::RVec3(sx, sy, sz),
                                JPH::Quat(q[0], q[1], q[2], q[3]).Normalized(),
                                mt, layer);
  bcs.mUserData = entity;
  // P2-05: trigger volume flag rides the RigidBody channel (body-level,
  // matching Jolt mIsSensor semantics). Zeroed wire bytes keep missing
  // payloads non-sensor; any nonzero byte reads as sensor.
  bcs.mIsSensor = body.is_sensor != 0U;
  bcs.mLinearDamping = body.linear_damping >= 0.0F ? body.linear_damping : 0.0F;
  bcs.mAngularDamping =
      body.angular_damping >= 0.0F ? body.angular_damping : 0.0F;
  if (is_kinematic) {
    bcs.mGravityFactor = 0.0F;
  }
  // Mass override: shapes carry density 1000 kg/m^3, so Jolt computes
  // inertia from the volume and scales to the C# mass (CalculateMassAnd
  // Inertia keeps the acceptance drop mass-independent).
  const float mass = ClampPositive(body.mass, 1.0F);
  bcs.mOverrideMassProperties =
      JPH::EOverrideMassProperties::CalculateMassAndInertia;
  bcs.mMassPropertiesOverride.mMass = mass;
  bcs.mAllowSleeping = !is_static;
  const JPH::EActivation activation =
      is_static ? JPH::EActivation::DontActivate : JPH::EActivation::Activate;
  JPH::BodyInterface& iface = system.GetBodyInterface();
  const JPH::BodyID bid = iface.CreateAndAddBody(bcs, activation);
  if (bid.IsInvalid()) {
    return false;
  }

  Entry& e = entries_[count_];
  e.entity = entity;
  e.body_id = bid.GetIndexAndSequenceNumber();
  e.shape_kind = shape_kind;
  ++count_;

  // Publish the link for TransformSync::push/pull. ecs_get_mut_id does NOT
  // add the component (CodeRabbit review): a brand-new entity has no
  // PhysicsBody row, so mutate only when present here and let sync()
  // install the link after the query via ecs_ensure_id (structural adds
  // are unsafe while iterating a query). The kinematic flag rides in the
  // entry either way so the deferred install is exact.
  e.kinematic = is_kinematic ? 1 : 0;
  if (physics_body_id_ != 0 &&
      ecs_is_alive(world_, static_cast<ecs_entity_t>(entity))) {
    auto* link = static_cast<PhysicsBody*>(const_cast<void*>(ecs_get_id(
        world_, static_cast<ecs_entity_t>(entity),
        static_cast<ecs_entity_t>(physics_body_id_))));
    if (link != nullptr) {
      link->body_id = e.body_id;
      link->kinematic = e.kinematic;
    }
  }
  return true;
}

auto BodySystem::sync(PhysicsWorld& world, TransformSync& sync, float fixed_dt,
                      bool advance) noexcept -> std::uint32_t {
  if (world_ == nullptr || entries_ == nullptr) {
    return 0;
  }
  if (!world.valid() || world.native() == nullptr) {
    return count_;
  }
  JPH::PhysicsSystem& system = *world.native();
  if (physics_body_id_ == 0) {
    physics_body_id_ =
        static_cast<std::uint64_t>(ecs_lookup(world_, "PhysicsBody"));
  }

  // Pass 1: drop entries whose entity died, lost RigidBody, lost its
  // collider, or changed collider kind (Pass 2 rebuilds changed ones).
  for (std::uint32_t i = 0; i < count_;) {
    const std::uint64_t entity = entries_[i].entity;
    bool keep = false;
    if (entity != 0 &&
        ecs_is_alive(world_, static_cast<ecs_entity_t>(entity))) {
      const void* rigid =
          rigidbody_id_ != 0
              ? ecs_get_id(world_, static_cast<ecs_entity_t>(entity),
                           static_cast<ecs_entity_t>(rigidbody_id_))
              : nullptr;
      if (rigid != nullptr) {
        const void* shape_data = nullptr;
        const std::uint32_t kind = ResolveShape(world_, entity, box_id_,
                                                sphere_id_, capsule_id_,
                                                &shape_data);
        keep = kind != 0 && kind == entries_[i].shape_kind;
      }
    }
    if (!keep) {
      remove_entry(system, i);  // swap-removes; do not advance
    } else {
      ++i;
    }
  }

  // Pass 2: create bodies for untracked (RigidBody + collider) entities.
  // Links for the newly created bodies install AFTER the query closes:
  // ecs_ensure_id adds the PhysicsBody row (ecs_get_mut_id inside
  // create_entry does not add it), and structural adds during a live
  // query iteration are unsafe. count_ only grows here, so index into the
  // created slice directly.
  const std::uint32_t created_begin = count_;
  if (rigidbody_query_ != nullptr) {
    ecs_iter_t it = ecs_query_iter(world_, rigidbody_query_);
    while (ecs_query_next(&it)) {
      const auto* bodies = static_cast<const BodyInterop*>(
          ecs_field_w_size(&it, sizeof(BodyInterop), 0));
      if (bodies == nullptr) {
        continue;
      }
      for (int r = 0; r < it.count; ++r) {
        const std::uint64_t entity =
            static_cast<std::uint64_t>(it.entities[r]);
        if (find_entry(entity) >= 0) {
          continue;  // already tracked
        }
        const void* shape_data = nullptr;
        const std::uint32_t kind = ResolveShape(world_, entity, box_id_,
                                                sphere_id_, capsule_id_,
                                                &shape_data);
        if (kind == 0) {
          continue;  // RigidBody without a collider: not a body yet
        }
        // Malformed rows (zero-size) still create with clamped shapes;
        // creation failure (OOM/shape reject) retries next sync.
        create_entry(system, entity, bodies[r], kind, shape_data);
      }
    }
  }

  // Install links for bodies created above (query is closed now).
  if (physics_body_id_ != 0) {
    for (std::uint32_t i = created_begin; i < count_; ++i) {
      const Entry& created = entries_[i];
      const auto ent = static_cast<ecs_entity_t>(created.entity);
      if (!ecs_is_alive(world_, ent)) {
        continue;
      }
      auto* link = static_cast<PhysicsBody*>(ecs_ensure_id(
          world_, ent, static_cast<ecs_entity_t>(physics_body_id_),
          sizeof(PhysicsBody)));
      if (link != nullptr) {
        link->body_id = created.body_id;
        link->kinematic = created.kinematic;
      }
    }
  }

  if (!advance || fixed_dt <= 0.0F) {
    return count_;
  }
  // Fixed-step pipeline (P2-03 loop): kinematic pose edits -> step ->
  // active-body pose writeback. Step runs through the owning PhysicsWorld
  // (temp allocator + job lanes); the pull shards internally via
  // TransformSync. Large dt is clamped to a single step to avoid
  // spiral-of-death inside the solver.
  sync.push_to_physics(system);
  const float dt = fixed_dt > 0.05F ? 0.05F : fixed_dt;
  world.step(dt, 1);
  sync.pull_from_physics(system, world.batch_sched(), world.batch_workers());
  return count_;
}

}  // namespace engine::physics

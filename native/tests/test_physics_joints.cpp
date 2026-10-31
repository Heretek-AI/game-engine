// test_physics_joints.cpp — P6-05 acceptance: fixed, hinge, slider,
// distance and spring joints as components backed by Jolt constraints.
//
// AC: a hinged door pushed with an impulse swings and stops at its angle
// limit, within +/- 1 deg. The rest pins each joint type's basic behavior.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include <Jolt/Jolt.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/EActivation.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include "engine/core/job_scheduler.hpp"
#include "engine/core/page_arena_allocator.hpp"
#include "engine/ecs/transform.hpp"
#include "engine/physics/components.hpp"
#include "engine/physics/joints.hpp"
#include "engine/physics/layers.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/physics/transform_sync.hpp"
#include "flecs.h"

namespace {

auto check(bool ok, const char* name) -> int {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  return ok ? 0 : 1;
}

struct Fixture {
  engine::core::JobScheduler sched;
  engine::core::PageArenaAllocator arena;
  engine::physics::PhysicsWorld world;
  engine::physics::TransformSync sync;
  engine::physics::JointManager joints;
  ecs_world_t* ecs{nullptr};

  auto init() noexcept -> bool {
    if (!sched.init()) {
      return false;
    }
    if (!arena.init(128U << 20, 16U << 20)) {
      return false;
    }
    engine::physics::PhysicsWorldConfig cfg;
    cfg.temp_allocator_bytes = 16U << 20;
    if (!world.construct(&sched, &arena, cfg)) {
      return false;
    }
    world.native()->SetContactListener(sync.listener()->native() == nullptr
                                           ? nullptr
                                           : sync.listener()->native());
    // LocalTransform must exist before TransformSync::init binds its query.
    ecs = ecs_init();
    if (ecs == nullptr) {
      return false;
    }
    ecs_entity_desc_t lent{};
    lent.name = "LocalTransform";
    const ecs_entity_t local_ent = ecs_entity_init(ecs, &lent);
    ecs_component_desc_t ld{};
    ld.entity = local_ent;
    ld.type.size = static_cast<ecs_size_t>(sizeof(engine::ecs::LocalTransform));
    ld.type.alignment = static_cast<ecs_size_t>(alignof(engine::ecs::LocalTransform));
    if (ecs_component_init(ecs, &ld) == 0) {
      return false;
    }
    if (!sync.init(ecs)) {
      return false;
    }
    if (!joints.init(ecs)) {
      return false;
    }
    return true;
  }

  ~Fixture() noexcept {
    joints.destroy();
    sync.destroy();
    if (ecs != nullptr) {
      ecs_fini(ecs);
    }
    world.destroy();
    arena.destroy();
    sched.shutdown();
  }
};

// Create a dynamic box body via BodyInterface (same pattern as
// test_physics_sync's fixture helper). Returns full BodyID.
// gravity_factor 1 = normal, 0 = weightless (door test: isolate the hinge).
auto make_body(JPH::PhysicsSystem& sys, float hx, float hy, float hz, float x,
               float y, float z, std::uint64_t user_data, bool dynamic = true,
               bool allow_sleep = false, float gravity_factor = 1.0F) -> std::uint32_t {
  JPH::BoxShapeSettings shape_settings(JPH::Vec3(hx, hy, hz));
  JPH::Shape::ShapeResult res = shape_settings.Create();
  JPH::Ref<JPH::Shape> shape = res.Get();
  const JPH::EMotionType mt =
      dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static;
  JPH::BodyCreationSettings bcs(shape, JPH::RVec3(x, y, z),
                                JPH::Quat::sIdentity(), mt,
                                engine::physics::ObjectLayers::kMoving);
  bcs.mUserData = user_data;
  bcs.mAllowSleeping = allow_sleep;
  bcs.mLinearDamping = 0.05F;
  bcs.mAngularDamping = 0.05F;
  bcs.mGravityFactor = gravity_factor;
  const JPH::BodyID bid =
      sys.GetBodyInterface().CreateAndAddBody(bcs, JPH::EActivation::Activate);
  return bid.GetIndexAndSequenceNumber();
}

// Spawn a Flecs entity carrying (LocalTransform, PhysicsBody) with the
// given body id — the shape the JointManager resolves joints through.
auto link_entity(ecs_world_t* ecs, std::uint32_t body_id, float px, float py,
                  float pz) -> std::uint64_t {
  const ecs_entity_t e = ecs_new(ecs);
  const ecs_entity_t local_id = ecs_lookup(ecs, "LocalTransform");
  const ecs_entity_t link_id = ecs_lookup(ecs, "PhysicsBody");
  ecs_add_id(ecs, e, local_id);
  ecs_add_id(ecs, e, link_id);
  auto* lt = static_cast<engine::ecs::LocalTransform*>(
      ecs_get_mut_id(ecs, e, local_id));
  lt->px = px;
  lt->py = py;
  lt->pz = pz;
  lt->qx = 0.0F;
  lt->qy = 0.0F;
  lt->qz = 0.0F;
  lt->qw = 1.0F;
  lt->sx = 1.0F;
  lt->sy = 1.0F;
  lt->sz = 1.0F;
  auto* link = static_cast<engine::physics::PhysicsBody*>(
      ecs_get_mut_id(ecs, e, link_id));
  link->body_id = body_id;
  link->kinematic = 0;
  return static_cast<std::uint64_t>(e);
}

}  // namespace

auto main() -> int {
  setvbuf(stdout, nullptr, _IONBF, 0);
  // Windows DLL-duplication guard (same as test_physics_sync).
  JPH::RegisterDefaultAllocator();
  int failures = 0;

  failures += check(sizeof(engine::physics::FixedJoint) == 64, "joints: FixedJoint 64B");
  failures += check(sizeof(engine::physics::HingeJoint) == 64, "joints: HingeJoint 64B");
  failures += check(sizeof(engine::physics::SliderJoint) == 64, "joints: SliderJoint 64B");
  failures += check(sizeof(engine::physics::DistanceJoint) == 64, "joints: DistanceJoint 64B");
  failures += check(sizeof(engine::physics::SpringJoint) == 64, "joints: SpringJoint 64B");

  Fixture fx;
  failures += check(fx.init(), "joints: fixture init (scheduler/arena/world/ecs/sync/manager)");

  JPH::PhysicsSystem& sys = *fx.world.native();
  constexpr float kDt = 1.0F / 60.0F;

  // --- AC: hinged door --------------------------------------------------
  // Frame (static post) + door (dynamic 1m x 2m x 0.05m panel, hinge on its
  // left edge). Impulse about +Y spins the door open; the 90-degree limit
  // must catch it within +/- 1 deg. Geometry is gap-free but non-touching:
  // frame spans x in [-0.1, 0.1], hinge at x = 0.2, door spans
  // [0.2, 1.2] so no contact fights the hinge. (Hinge at 0.12 left only a
  // 0.02 gap while the door's inner corner sweeps a 0.025 radius about the
  // hinge — the corner clipped the frame past ~53 deg and bounced back.)
  {
    const std::uint32_t frame_bid = make_body(sys, 0.1F, 1.0F, 0.1F, 0.0F, 1.0F, 0.0F,
                                              1001, false /* static */);
    const std::uint32_t door_bid =
        make_body(sys, 0.5F, 1.0F, 0.025F, 0.7F, 1.0F, 0.0F, 1002, true,
                  false /* no sleep */, 0.0F /* weightless: isolate hinge */);
    // Mild angular damping on the door: the Jolt angle limit kills velocity
    // into the stop, but the position solver's projection leaves a small
    // residual kick (~0.5 deg/s observed); with zero damping that coasts
    // forever and the door never rests. c = 0.3 settles it into the stop
    // while still arriving with authority (theta_inf = w0/c = 4 rad).
    // (No BodyInterface damping setter exists, so patch creation damping
    // here via MotionProperties before the swing.)
    {
      JPH::BodyLockWrite dlock(sys.GetBodyLockInterface(), JPH::BodyID(door_bid));
      if (dlock.Succeeded()) {
        dlock.GetBody().GetMotionProperties()->SetAngularDamping(0.3F);
      }
    }
    const std::uint64_t frame_e = link_entity(fx.ecs, frame_bid, 0.0F, 1.0F, 0.0F);
    const std::uint64_t door_e = link_entity(fx.ecs, door_bid, 0.7F, 1.0F, 0.0F);

    const ecs_entity_t hinge_comp = ecs_lookup(fx.ecs, "HingeJoint");
    failures += check(hinge_comp != 0, "door: HingeJoint component registered");
    const ecs_entity_t je = ecs_new(fx.ecs);
    ecs_add_id(fx.ecs, je, hinge_comp);
    auto* hj = static_cast<engine::physics::HingeJoint*>(
        ecs_get_mut_id(fx.ecs, je, hinge_comp));
    hj->body_a = frame_e;
    hj->body_b = door_e;
    hj->anchor_x = 0.2F;
    hj->anchor_y = 1.0F;
    hj->anchor_z = 0.0F;
    hj->limits_min = 0.0F;               // closed
    hj->limits_max = 1.5707964F;        // 90 deg open
    hj->max_friction_torque = 0.0F;
    hj->enabled = 1;

    failures += check(fx.joints.sync(sys) == 1, "door: one hinge constraint created");

    // Push the door open: set an initial angular velocity about +Y (hinge
    // axis). w0 = 1.0 rad/s reaches the 90-degree stop in ~2 s. The door
    // body runs mild angular damping (0.3: the Jolt stop leaves a small
    // residual kick that zero damping never kills, while the 0.05 default
    // bled a gentle swing to a 40-degree stall) so arrival is clean and it
    // genuinely rests at the stop. 12 s at 60 Hz: swing + settle.
    sys.GetBodyInterface().SetAngularVelocity(JPH::BodyID(door_bid),
                                              JPH::Vec3(0.0F, 1.0F, 0.0F));
    for (int i = 0; i < 720; ++i) {  // 12 s at 60 Hz
      fx.world.step(kDt, 1);
      if ((i % 60) == 59) {
        float dbg = -99.0F;
        fx.joints.hinge_angle(sys, static_cast<std::uint64_t>(je), &dbg);
        std::printf("door: t=%.1fs angle=%.2f deg\n", (i + 1) / 60.0F,
                    dbg * 57.29578F);
      }
    }
    float angle = -1.0F;
    failures += check(fx.joints.hinge_angle(
                          sys, static_cast<std::uint64_t>(je), &angle),
                      "door: hinge angle readable");
    constexpr float kDeg = 57.295780F;
    std::printf("door: final angle %.3f deg (limit 90)\n", angle * kDeg);
    // Swings open and stops at the 90-degree limit within +/- 1 deg.
    failures += check(angle > 0.5F, "door: swung open past 30 deg");
    failures += check(std::fabs(angle - 1.5707964F) < 0.0174533F,
                      "door: rests at 90 deg limit (+/- 1 deg)");

    // Removing the joint entity removes the Jolt constraint.
    ecs_delete(fx.ecs, je);
    failures += check(fx.joints.sync(sys) == 0, "door: constraint removed with entity");
  }

  // --- Fixed joint: two boxes welded ------------------------------------
  {
    const std::uint32_t a = make_body(sys, 0.5F, 0.5F, 0.5F, -4.0F, 3.0F, 0.0F, 2001, true,
                                      false /* no sleep: resting face contact deactivates the island */);
    const std::uint32_t b = make_body(sys, 0.5F, 0.5F, 0.5F, -4.0F, 4.2F, 0.0F, 2002, true,
                                      false /* no sleep: see above */);
    const std::uint64_t ea = link_entity(fx.ecs, a, -4.0F, 3.0F, 0.0F);
    const std::uint64_t eb = link_entity(fx.ecs, b, -4.0F, 4.2F, 0.0F);
    const ecs_entity_t fc = ecs_lookup(fx.ecs, "FixedJoint");
    const ecs_entity_t je = ecs_new(fx.ecs);
    ecs_add_id(fx.ecs, je, fc);
    auto* fj = static_cast<engine::physics::FixedJoint*>(ecs_get_mut_id(fx.ecs, je, fc));
    fj->body_a = ea;
    fj->body_b = eb;
    fj->enabled = 1;
    failures += check(fx.joints.sync(sys) == 1, "fixed: constraint created");
    // Drop both; relative offset must stay 1.2 m (welded) after 2 s.
    // allow_sleep=false on both: the pair starts in face contact and Jolt
    // may otherwise deactivate the island mid-drop, leaving the
    // FixedConstraint's cached handles stale on the next step.
    for (int i = 0; i < 120; ++i) {
      fx.world.step(kDt, 1);
    }
    JPH::BodyLockRead la(sys.GetBodyLockInterface(), JPH::BodyID(a));
    JPH::BodyLockRead lb(sys.GetBodyLockInterface(), JPH::BodyID(b));
    float dy = 0.0F;
    if (la.Succeeded() && lb.Succeeded()) {
      dy = (lb.GetBody().GetPosition() - la.GetBody().GetPosition()).GetY();
    }
    std::printf("fixed: dy after drop %.3f (expect ~1.2)\n", dy);
    failures += check(std::fabs(dy - 1.2F) < 0.05F, "fixed: bodies stay welded");
    ecs_delete(fx.ecs, je);
    failures += check(fx.joints.sync(sys) == 0, "fixed: constraint removed");
  }

  // --- Slider joint: travel clamped --------------------------------------
  {
    const std::uint32_t a = make_body(sys, 0.25F, 0.25F, 0.25F, 4.0F, 2.0F, 0.0F, 3001, true);
    const std::uint32_t b = make_body(sys, 0.25F, 0.25F, 0.25F, 5.0F, 2.0F, 0.0F, 3002, true);
    const std::uint64_t ea = link_entity(fx.ecs, a, 4.0F, 2.0F, 0.0F);
    const std::uint64_t eb = link_entity(fx.ecs, b, 5.0F, 2.0F, 0.0F);
    const ecs_entity_t sc = ecs_lookup(fx.ecs, "SliderJoint");
    const ecs_entity_t je = ecs_new(fx.ecs);
    ecs_add_id(fx.ecs, je, sc);
    auto* sj = static_cast<engine::physics::SliderJoint*>(ecs_get_mut_id(fx.ecs, je, sc));
    sj->body_a = ea;
    sj->body_b = eb;
    sj->limits_min = -0.5F;
    sj->limits_max = 0.5F;
    sj->enabled = 1;
    failures += check(fx.joints.sync(sys) == 1, "slider: constraint created");
    // Shove body b along +X hard; the slider must clamp travel to 0.5 m.
    sys.GetBodyInterface().AddImpulse(JPH::BodyID(b), JPH::Vec3(30.0F, 0.0F, 0.0F));
    for (int i = 0; i < 180; ++i) {
      fx.world.step(kDt, 1);
    }
    JPH::BodyLockRead la(sys.GetBodyLockInterface(), JPH::BodyID(a));
    JPH::BodyLockRead lb(sys.GetBodyLockInterface(), JPH::BodyID(b));
    float dx = 99.0F;
    if (la.Succeeded() && lb.Succeeded()) {
      dx = (lb.GetBody().GetPosition() - la.GetBody().GetPosition()).GetX();
    }
    std::printf("slider: dx after shove %.3f (expect <= 1.5 + slack)\n", dx);
    failures += check(dx < 2.0F, "slider: travel clamped by limits");
    ecs_delete(fx.ecs, je);
    failures += check(fx.joints.sync(sys) == 0, "slider: constraint removed");
  }

  // --- Distance joint: bodies stay within range --------------------------
  {
    const std::uint32_t a = make_body(sys, 0.3F, 0.3F, 0.3F, -8.0F, 5.0F, 0.0F, 4001, true);
    const std::uint32_t b = make_body(sys, 0.3F, 0.3F, 0.3F, -6.0F, 5.0F, 0.0F, 4002, true);
    const std::uint64_t ea = link_entity(fx.ecs, a, -8.0F, 5.0F, 0.0F);
    const std::uint64_t eb = link_entity(fx.ecs, b, -6.0F, 5.0F, 0.0F);
    const ecs_entity_t dc = ecs_lookup(fx.ecs, "DistanceJoint");
    const ecs_entity_t je = ecs_new(fx.ecs);
    ecs_add_id(fx.ecs, je, dc);
    auto* dj = static_cast<engine::physics::DistanceJoint*>(
        ecs_get_mut_id(fx.ecs, je, dc));
    dj->body_a = ea;
    dj->body_b = eb;
    dj->anchor_a_x = -8.0F;
    dj->anchor_a_y = 5.0F;
    dj->anchor_b_x = -6.0F;
    dj->anchor_b_y = 5.0F;
    dj->min_distance = 1.0F;
    dj->max_distance = 2.0F;
    dj->enabled = 1;
    failures += check(fx.joints.sync(sys) == 1, "distance: constraint created");
    // Fling them apart; max distance 2 m must hold.
    sys.GetBodyInterface().AddImpulse(JPH::BodyID(a), JPH::Vec3(-20.0F, 0.0F, 0.0F));
    sys.GetBodyInterface().AddImpulse(JPH::BodyID(b), JPH::Vec3(20.0F, 0.0F, 0.0F));
    for (int i = 0; i < 180; ++i) {
      fx.world.step(kDt, 1);
    }
    JPH::BodyLockRead la(sys.GetBodyLockInterface(), JPH::BodyID(a));
    JPH::BodyLockRead lb(sys.GetBodyLockInterface(), JPH::BodyID(b));
    float dist = -1.0F;
    if (la.Succeeded() && lb.Succeeded()) {
      dist = (lb.GetBody().GetPosition() - la.GetBody().GetPosition()).Length();
    }
    std::printf("distance: separation %.3f (expect <= ~2.5)\n", dist);
    failures += check(dist > 0.0F && dist < 2.6F, "distance: max range enforced");
    ecs_delete(fx.ecs, je);
    failures += check(fx.joints.sync(sys) == 0, "distance: constraint removed");
  }

  // --- Spring joint: displaced body oscillates back ----------------------
  {
    const std::uint32_t a = make_body(sys, 0.3F, 0.3F, 0.3F, 8.0F, 5.0F, 0.0F, 5001, false);
    const std::uint32_t b = make_body(sys, 0.3F, 0.3F, 0.3F, 10.0F, 5.0F, 0.0F, 5002, true);
    const std::uint64_t ea = link_entity(fx.ecs, a, 8.0F, 5.0F, 0.0F);
    const std::uint64_t eb = link_entity(fx.ecs, b, 10.0F, 5.0F, 0.0F);
    const ecs_entity_t spc = ecs_lookup(fx.ecs, "SpringJoint");
    const ecs_entity_t je = ecs_new(fx.ecs);
    ecs_add_id(fx.ecs, je, spc);
    auto* sj = static_cast<engine::physics::SpringJoint*>(
        ecs_get_mut_id(fx.ecs, je, spc));
    sj->body_a = ea;
    sj->body_b = eb;
    sj->anchor_a_x = 8.0F;
    sj->anchor_a_y = 5.0F;
    sj->anchor_b_x = 10.0F;
    sj->anchor_b_y = 5.0F;
    sj->frequency = 2.0F;
    sj->damping = 0.3F;
    sj->enabled = 1;
    failures += check(fx.joints.sync(sys) == 1, "spring: constraint created");
    // Pull the dynamic body out to x = 13 and let go; spring pulls back.
    sys.GetBodyInterface().SetPosition(JPH::BodyID(b), JPH::RVec3(13.0, 5.0, 0.0),
                                       JPH::EActivation::Activate);
    for (int i = 0; i < 300; ++i) {  // 5 s: settle toward rest length 2
      fx.world.step(kDt, 1);
    }
    JPH::BodyLockRead la(sys.GetBodyLockInterface(), JPH::BodyID(a));
    JPH::BodyLockRead lb(sys.GetBodyLockInterface(), JPH::BodyID(b));
    float dist = -1.0F;
    if (la.Succeeded() && lb.Succeeded()) {
      dist = (lb.GetBody().GetPosition() - la.GetBody().GetPosition()).Length();
    }
    std::printf("spring: separation %.3f (rest 2.0)\n", dist);
    failures += check(dist > 0.0F && std::fabs(dist - 2.0F) < 0.6F,
                      "spring: returns toward rest length");
    ecs_delete(fx.ecs, je);
    failures += check(fx.joints.sync(sys) == 0, "spring: constraint removed");
  }

  if (failures == 0) {
    std::printf("P6-05 physics joints: ALL PASS\n");
    return 0;
  }
  std::printf("P6-05 physics joints: %d FAILURES\n", failures);
  return 1;
}

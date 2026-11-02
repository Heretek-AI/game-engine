// test_physics_bodies.cpp — P2-04 acceptance (native CTest `engine_physics_bodies`).
//
// Drives the BodySystem fixed-step path (create -> simulate -> read back)
// over Flecs components, exactly as spec section 6 P2-04 demands:
//
//   * Floor: static RigidBody (motion 0) + BoxCollider half extents
//     (50, 0.5, 50) at y=-0.5, so the box top sits exactly at y=0.
//   * Ball: dynamic RigidBody (motion 2) + SphereCollider r=0.5 at y=5.
//   * Step 2 simulated seconds at 50 Hz (100 x BodySystem::sync with
//     fixed_dt=1/50, advance=true) and assert the ball rests at
//     0.5 +/- 0.05 (sphere radius above the floor top).
//   * Removal: deleting the ball entity drops its Jolt body (count 2->1).
//   * Raycast: from y=10 down over the floor hits at y ~= 0
//     (fraction wraps the floor top along the 20-long ray).
//
// No GPU, no window — pure Jolt + Flecs. Unbuffered stdout so a segfault
// keeps every PASS line (same convention as the M4-02 sync test).
// Windows DLL-duplication guard included (same as test_physics_sync:
// this TU's Jolt globals are a second copy when engine_core is a DLL).
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include <Jolt/Jolt.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include "engine/core/job_scheduler.hpp"
#include "engine/core/page_arena_allocator.hpp"
#include "engine/ecs/ecs_world.hpp"
#include "engine/ecs/transform.hpp"
#include "engine/physics/bodies.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/physics/spatial_queries.hpp"
#include "engine/physics/transform_sync.hpp"
#include "flecs.h"

namespace {

auto check(bool ok, const char* name) -> int {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  return ok ? 0 : 1;
}

auto feq(float got, float want, float eps) -> bool {
  const float d = got - want;
  return d > -eps && d < eps;
}

// Spawn-pose writer for the acceptance entities.

auto set_lt(ecs_world_t* world, ecs_entity_t local_id, ecs_entity_t e, float x,
            float y, float z) -> void {
  auto* lt = static_cast<engine::ecs::LocalTransform*>(
      ecs_get_mut_id(world, e, local_id));
  lt->px = x;
  lt->py = y;
  lt->pz = z;
  lt->qx = 0.0F;
  lt->qy = 0.0F;
  lt->qz = 0.0F;
  lt->qw = 1.0F;
  lt->sx = 1.0F;
  lt->sy = 1.0F;
  lt->sz = 1.0F;
}

}  // namespace

auto main() -> int {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  JPH::RegisterDefaultAllocator();
  int failures = 0;

  failures += check(sizeof(engine::physics::BodyInterop) == 16,
                    "layout: RigidBody 16B");
  failures += check(alignof(engine::physics::BodyInterop) == 4,
                    "layout: RigidBody align 4");
  failures += check(sizeof(engine::physics::BoxInterop) == 12,
                    "layout: BoxCollider 12B");
  failures += check(sizeof(engine::physics::SphereInterop) == 4,
                    "layout: SphereCollider 4B");
  failures += check(sizeof(engine::physics::CapsuleInterop) == 8,
                    "layout: CapsuleCollider 8B");

  engine::core::JobScheduler sched;
  engine::core::PageArenaAllocator arena;
  engine::physics::PhysicsWorld phys;
  failures += check(sched.init(), "fixture: scheduler init");
  failures += check(arena.init(128U << 20, 16U << 20), "fixture: arena init");
  engine::physics::PhysicsWorldConfig cfg;
  cfg.temp_allocator_bytes = 16U << 20;
  failures += check(phys.construct(&sched, &arena, cfg),
                    "fixture: physics world construct");
  if (!phys.valid()) {
    std::printf("P2-04 bodies: FIXTURE FAILED\n");
    return 1;
  }

  engine::ecs::EcsWorld ecs;
  failures += check(ecs.construct(), "fixture: ecs construct");
  ecs_world_t* world = ecs.raw();
  failures += check(world != nullptr, "fixture: ecs raw non-null");

  engine::physics::TransformSync sync;
  engine::physics::BodySystem bodies;
  failures += check(sync.init(world), "fixture: sync init");
  failures +=
      check(bodies.init(world), "fixture: bodysystem init (registers P2-04)");
  if (!bodies.valid()) {
    std::printf("P2-04 bodies: BODY INIT FAILED\n");
    return 1;
  }
  // Contact listener mirrors the M4-02 fixture: install now that sync
  // owns it (world constructed before the listener existed).
  phys.native()->SetContactListener(sync.listener()->native());

  // Component ids under the C# full names (BodySystem::init registered
  // them idempotently — look up the existing rows).
  const ecs_entity_t rigid_id =
      ecs_lookup_path_w_sep(world, 0, "Engine.Runtime.RigidBody", "", "", false);
  const ecs_entity_t box_id =
      ecs_lookup_path_w_sep(world, 0, "Engine.Runtime.BoxCollider", "", "", false);
  const ecs_entity_t sphere_id = ecs_lookup_path_w_sep(
      world, 0, "Engine.Runtime.SphereCollider", "", "", false);
  failures += check(rigid_id != 0 && box_id != 0 && sphere_id != 0,
                    "fixture: P2-04 component ids resolve");
  const ecs_entity_t local_id = ecs.local_id();
  failures += check(local_id != 0, "fixture: LocalTransform built-in");

  // --- Floor: static box, top at y=0 ---
  const ecs_entity_t floor = ecs_new(world);
  ecs_add_id(world, floor, local_id);
  ecs_add_id(world, floor, rigid_id);
  ecs_add_id(world, floor, box_id);
  set_lt(world, local_id, floor, 0.0F, -0.5F, 0.0F);
  auto* floor_rb = static_cast<engine::physics::BodyInterop*>(
      ecs_get_mut_id(world, floor, rigid_id));
  floor_rb->mass = 0.0F;
  floor_rb->motion_type = 0;  // static
  auto* floor_box = static_cast<engine::physics::BoxInterop*>(
      ecs_get_mut_id(world, floor, box_id));
  floor_box->half_extents[0] = 50.0F;
  floor_box->half_extents[1] = 0.5F;
  floor_box->half_extents[2] = 50.0F;

  // --- Ball: dynamic sphere r=0.5 from y=5 ---
  const ecs_entity_t ball = ecs_new(world);
  ecs_add_id(world, ball, local_id);
  ecs_add_id(world, ball, rigid_id);
  ecs_add_id(world, ball, sphere_id);
  set_lt(world, local_id, ball, 0.0F, 5.0F, 0.0F);
  auto* ball_rb = static_cast<engine::physics::BodyInterop*>(
      ecs_get_mut_id(world, ball, rigid_id));
  ball_rb->mass = 1.0F;
  ball_rb->motion_type = 2;  // dynamic
  ball_rb->linear_damping = 0.05F;
  ball_rb->angular_damping = 0.05F;
  auto* ball_sp = static_cast<engine::physics::SphereInterop*>(
      ecs_get_mut_id(world, ball, sphere_id));
  ball_sp->radius = 0.5F;

  constexpr float kDt = 1.0F / 50.0F;
  std::uint32_t live = 0;
  for (int i = 0; i < 100; ++i) {  // 2 simulated s at 50 Hz
    live = bodies.sync(phys, sync, kDt, true);
  }
  failures += check(live == 2, "bodies: floor + ball tracked after 2 s");
  failures += check(bodies.body_count() == 2, "bodies: body_count == 2");

  // Rest height: sphere r=0.5 over floor top y=0 -> 0.5 +/- 0.05.
  // Pull writes into LocalTransform (the BodySystem pipeline already ran
  // pull each step; re-read the authoritative column here).
  float ball_y = -1000.0F;
  {
    const auto* lt = static_cast<const engine::ecs::LocalTransform*>(
        ecs_get_id(world, ball, local_id));
    if (lt != nullptr) {
      ball_y = lt->py;
    }
  }
  std::printf("[info] ball rest y = %.4f\n", static_cast<double>(ball_y));
  failures += check(feq(ball_y, 0.5F, 0.05F),
                    "ACCEPT: ball rests at 0.5 +/- 0.05 after 2 s at 50 Hz");

  // --- Raycast: from (5,10,0) straight down hits the floor top at y ~= 0 ---
  // (x=5 clears the resting ball at the origin; the floor spans +/-50).
  {
    engine::physics::RayInput ray{5.0F, 10.0F, 0.0F, 0.0F, -20.0F, 0.0F};
    engine::physics::SpatialHit hit{};
    const std::int32_t rc =
        engine::physics::spatial_raycast(*phys.native(), &ray, &hit);
    failures += check(rc == 1 && hit.hit == 1, "raycast: floor hit reported");
    // Floor top y=0 along the ray (10 -> -10): fraction = (10-0)/20 = 0.5.
    failures += check(feq(hit.fraction, 0.5F, 0.02F), "raycast: fraction ~= 0.5");
    failures += check(feq(hit.px, 5.0F, 0.05F) && feq(hit.py, 0.0F, 0.05F) &&
                          feq(hit.pz, 0.0F, 0.05F),
                      "raycast: hit point ~= (5,0,0)");
    failures += check(hit.ny > 0.9F, "raycast: normal +y (floor top)");
  }

  // --- Removal: deleting the ball drops its Jolt body (2 -> 1) ---
  ecs_delete(world, ball);
  live = bodies.sync(phys, sync, kDt, false);
  failures += check(live == 1, "removal: deleting ball leaves 1 body");
  failures += check(bodies.body_count() == 1, "removal: body_count == 1");

  // Second removal pass must be stable (no double-free / stale entry).
  live = bodies.sync(phys, sync, kDt, false);
  failures += check(live == 1, "removal: second sync stable at 1");

  // Teardown order: bodies + sync own cached queries on this world —
  // destroy them BEFORE the EcsWorld destructor runs ecs_fini.
  bodies.destroy();
  sync.destroy();

  if (failures == 0) {
    std::printf("P2-04 physics bodies: ALL PASS\n");
    return 0;
  }
  std::printf("P2-04 physics bodies: %d FAILURES\n", failures);
  return 1;
}

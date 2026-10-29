// test_physics_jolt.cpp — M4-01 acceptance: EnkiJobSystem on the shared
// enkiTS pool, PageArena-backed temp allocator, layer config, components,
// world step, simulation under gravity with active bodies.
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "engine/core/job_scheduler.hpp"
#include "engine/core/page_arena_allocator.hpp"
#include "engine/physics/components.hpp"
#include "engine/physics/layers.hpp"
#include "engine/physics/physics_world.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

namespace {

auto check(bool ok, const char* name) -> int {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  return ok ? 0 : 1;
}

}  // namespace

auto main() -> int {
  int failures = 0;

  static_assert(sizeof(engine::physics::RigidBody) == 64, "RigidBody size");
  static_assert(alignof(engine::physics::RigidBody) == 64, "RigidBody align");
  static_assert(sizeof(engine::physics::BoxCollider) == 64, "Box size");
  static_assert(sizeof(engine::physics::SphereCollider) == 64, "Sphere size");
  static_assert(sizeof(engine::physics::CapsuleCollider) == 64, "Capsule size");
  static_assert(sizeof(engine::physics::MeshCollider) == 64, "Mesh size");
  static_assert(sizeof(engine::physics::CharacterController) == 64, "CC size");
  failures += check(true, "components: 64B cache-line static asserts");

  engine::core::JobScheduler sched;
  failures += check(sched.init(), "scheduler: init");
  engine::core::PageArenaAllocator arena;
  failures += check(arena.init(128U << 20, 16U << 20), "arena: 128MiB/16MiB");

  engine::physics::PhysicsWorld world;
  engine::physics::PhysicsWorldConfig cfg;
  cfg.temp_allocator_bytes = 16U << 20;
  failures += check(world.construct(&sched, &arena, cfg), "world: construct");
  failures += check(world.valid(), "world: valid");
  failures += check(world.jobs() != nullptr, "world: job system present");
  failures +=
      check(world.jobs()->GetMaxConcurrency() >= 1, "jobs: concurrency >= 1");
  failures += check(world.temp()->capacity() == cfg.temp_allocator_bytes,
                    "temp: capacity matches config");
  failures += check(world.step(1.0f / 60.0f, 1), "world: initial step 1/60s");
  failures += check(world.body_count() == 0, "world: zero bodies initially");
  failures += check(world.temp()->used() == 0, "temp: rewound after step");

  // Create real bodies: static floor + dynamic falling sphere
  JPH::BodyInterface& bi = world.native()->GetBodyInterface();

  JPH::BoxShapeSettings floor_shape_settings(JPH::Vec3(50.0f, 1.0f, 50.0f));
  JPH::ShapeSettings::ShapeResult floor_shape_result = floor_shape_settings.Create();
  JPH::ShapeRefC floor_shape = floor_shape_result.Get();
  JPH::BodyCreationSettings floor_settings(
      floor_shape, JPH::RVec3(0.0f, -1.0f, 0.0f), JPH::Quat::sIdentity(),
      JPH::EMotionType::Static, engine::physics::ObjectLayers::kNonMoving);
  JPH::Body* floor = bi.CreateBody(floor_settings);
  failures += check(floor != nullptr, "body: floor created");
  bi.AddBody(floor->GetID(), JPH::EActivation::DontActivate);

  JPH::SphereShapeSettings sphere_shape_settings(0.5f);
  JPH::ShapeSettings::ShapeResult sphere_shape_result = sphere_shape_settings.Create();
  JPH::ShapeRefC sphere_shape = sphere_shape_result.Get();
  JPH::BodyCreationSettings sphere_settings(
      sphere_shape, JPH::RVec3(0.0f, 10.0f, 0.0f), JPH::Quat::sIdentity(),
      JPH::EMotionType::Dynamic, engine::physics::ObjectLayers::kMoving);
  JPH::Body* sphere = bi.CreateBody(sphere_settings);
  failures += check(sphere != nullptr, "body: sphere created");
  bi.AddBody(sphere->GetID(), JPH::EActivation::Activate);

  failures += check(world.body_count() == 2, "world: 2 bodies added");

  world.native()->OptimizeBroadPhase();

  // Simulate 10 frames under gravity:
  for (int f = 0; f < 10; ++f) {
    failures += check(world.step(1.0f / 60.0f, 1), "world: step frame");
  }

  JPH::RVec3 pos = bi.GetPosition(sphere->GetID());
  failures += check(pos.GetY() < 10.0f, "simulation: sphere moved downward with gravity");
  failures += check(world.temp()->used() == 0, "temp: rewound after active simulation");

  bi.RemoveBody(sphere->GetID());
  bi.DestroyBody(sphere->GetID());
  bi.RemoveBody(floor->GetID());
  bi.DestroyBody(floor->GetID());

  world.destroy();
  failures += check(!world.valid(), "world: invalid after destroy");
  arena.destroy();
  sched.shutdown();

  if (failures == 0) {
    std::printf("M4-01 physics smoke: ALL PASS\n");
    return 0;
  }
  std::printf("M4-01 physics smoke: %d FAILURES\n", failures);
  return 1;
}

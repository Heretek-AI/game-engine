// test_physics_jolt.cpp — M4-01 acceptance: EnkiJobSystem on the shared
// enkiTS pool, PageArena-backed temp allocator, layer config, components,
// world step, sequential factory lifecycle.
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "engine/core/job_scheduler.hpp"
#include "engine/core/page_arena_allocator.hpp"
#include "engine/physics/components.hpp"
#include "engine/physics/physics_world.hpp"

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
  failures += check(world.construct(&sched, &arena, cfg), "world: construct 1");
  failures += check(world.valid(), "world: valid");
  failures += check(world.jobs() != nullptr, "world: job system present");
  failures +=
      check(world.jobs()->GetMaxConcurrency() >= 1, "jobs: concurrency >= 1");
  failures += check(world.temp()->capacity() == cfg.temp_allocator_bytes,
                    "temp: capacity matches config");
  failures += check(world.step(1.0f / 60.0f, 1), "world: step 1 (1/60s)");
  failures += check(world.step(1.0f / 60.0f, 2), "world: step 2 (substeps)");
  failures += check(world.body_count() == 0, "world: zero bodies initially");
  failures += check(world.temp()->used() == 0, "temp: rewound after step");

  world.destroy();
  failures += check(!world.valid(), "world: invalid after destroy 1");

  // Re-construct to verify factory refcounting and re-registration
  failures += check(world.construct(&sched, &arena, cfg), "world: construct 2 (lifecycle re-init)");
  failures += check(world.valid(), "world: valid 2");
  failures += check(world.step(1.0f / 60.0f, 1), "world: step after re-init");
  world.destroy();
  failures += check(!world.valid(), "world: invalid after destroy 2");

  arena.destroy();
  sched.shutdown();

  if (failures == 0) {
    std::printf("M4-01 physics smoke: ALL PASS\n");
    return 0;
  }
  std::printf("M4-01 physics smoke: %d FAILURES\n", failures);
  return 1;
}

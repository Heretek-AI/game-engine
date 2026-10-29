// test_physics_sync.cpp — M4-02 acceptance: parallel Flecs <-> Jolt sync.
//
// AC1: kinematic LocalTransform edits push to Jolt (position changes).
// AC2: after step, active dynamic bodies pull into Flecs columns in parallel.
// AC3: collision events land in the lock-free ring and drain as a batch.
// Rings: standalone MPMC stress (4 producers x 10k pushes, no drops).
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/EActivation.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include "engine/core/job_scheduler.hpp"
#include "engine/core/page_arena_allocator.hpp"
#include "engine/ecs/transform.hpp"
#include "engine/physics/collision_events.hpp"
#include "engine/physics/components.hpp"
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
  bool ok{false};

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
    // Install the M4-02 contact listener before bodies exist.
    world.native()->SetContactListener(sync.listener()->native() == nullptr
                                           ? nullptr
                                           : sync.listener()->native());
    ok = true;
    return true;
  }

  // NOTE: sync.init needs the Flecs world; created per-test below.
  // Teardown is manual in the test (sync.destroy BEFORE ecs_fini — the
  // sync owns a cached query on the world).
};

// Create one dynamic box body at (x,y,z), UserData = entity. Returns the
// full Jolt BodyID (index+sequence via GetIndexAndSequenceNumber).
auto make_box(JPH::PhysicsSystem& sys, float x, float y, float z,
              std::uint64_t user_data) -> std::uint32_t {
  JPH::BoxShapeSettings shape_settings(JPH::Vec3(0.5F, 0.5F, 0.5F));
  const JPH::Shape* shape = shape_settings.Create().Get();
  JPH::BodyCreationSettings bcs(shape, JPH::RVec3(x, y, z),
                                JPH::Quat::sIdentity(),
                                JPH::EMotionType::Dynamic,
                                engine::physics::ObjectLayers::kMoving);
  bcs.mUserData = user_data;
  bcs.mAllowSleeping = false;
  const JPH::BodyID bid =
      sys.GetBodyInterface().CreateAndAddBody(bcs, JPH::EActivation::Activate);
  return bid.GetIndexAndSequenceNumber();
}

}  // namespace

auto main() -> int {
  // Unbuffered stdout: on a segfault the CI log keeps every PASS line,
  // pinpointing the crashing section (Windows gives no core dump).
  setvbuf(stdout, nullptr, _IONBF, 0);
  int failures = 0;

  // --- AC3-unit: ring MPMC stress, no drops, FIFO batches ---
  {
    engine::physics::CollisionEventRing ring;
    failures += check(ring.init(1U << 16), "ring: init 64k");
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 10000;
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> threads;
    for (int p = 0; p < kProducers; ++p) {
      threads.emplace_back([&, p]() {
        ready.fetch_add(1);
        while (!go.load()) {
        }
        for (int i = 0; i < kPerProducer; ++i) {
          engine::physics::CollisionEvent ev{};
          ev.entity_a = static_cast<std::uint64_t>(p);
          ev.entity_b = static_cast<std::uint64_t>(i);
          ev.kind = engine::physics::CollisionEventKind::kEnter;
          while (!ring.try_push(ev)) {
          }  // retry on transient full (16k >> in-flight)
        }
      });
    }
    while (ready.load() < kProducers) {
    }
    go.store(true);
    for (auto& t : threads) {
      t.join();
    }
    std::vector<engine::physics::CollisionEvent> out(1U << 16);
    const std::size_t n = ring.drain_to(out.data(), out.size());
    failures += check(n == static_cast<std::size_t>(kProducers * kPerProducer),
                      "ring: 40k events, no drops");
    failures += check(ring.dropped() == 0, "ring: dropped == 0");
    failures += check(ring.pending() == 0, "ring: empty after drain");
    // Overflow path: tiny ring, single-threaded overfill.
    engine::physics::CollisionEventRing small;
    failures += check(small.init(8), "ring: init 8");
    engine::physics::CollisionEvent ev{};
    int pushed = 0;
    for (int i = 0; i < 64; ++i) {
      if (small.try_push(ev)) {
        ++pushed;
      }
    }
    failures +=
        check(small.dropped() == static_cast<std::uint64_t>(64 - pushed),
              "ring: overflow counted");
    small.destroy();
    ring.destroy();
  }

  // --- AC1+AC2+AC3 integration: flecs world + bodies + step + sync ---
  {
    Fixture fx;
    failures += check(fx.init(), "fixture: scheduler/arena/world");
    if (!fx.ok) {
      std::printf("M4-02 sync: FIXTURE FAILED\n");
      return 1;
    }

    ecs_world_t* world = ecs_init();
    failures += check(world != nullptr, "fixture: ecs_init");
    if (world == nullptr) {
      fx.world.destroy();
      fx.arena.destroy();
      fx.sched.shutdown();
      return 1;
    }

    // Register M1-03 builtins manually (component ids for the query).
    ecs_entity_desc_t led{};
    led.name = "LocalTransform";
    const ecs_entity_t local_ent = ecs_entity_init(world, &led);
    ecs_component_desc_t lcd{};
    lcd.entity = local_ent;
    lcd.type.size =
        static_cast<ecs_size_t>(sizeof(engine::ecs::LocalTransform));
    lcd.type.alignment =
        static_cast<ecs_size_t>(alignof(engine::ecs::LocalTransform));
    const ecs_entity_t local_id = ecs_component_init(world, &lcd);

    failures +=
        check(fx.sync.init(world), "sync: init (PhysicsBody + ring)");
    // Re-install listener now that sync owns it (fixture installed null).
    fx.world.native()->SetContactListener(fx.sync.listener()->native());
    failures += check(fx.sync.listener()->native() != nullptr,
                      "sync: listener installed");

    constexpr int kBodies = 64;
    std::vector<ecs_entity_t> entities;
    entities.reserve(kBodies);
    for (int i = 0; i < kBodies; ++i) {
      const ecs_entity_t e = ecs_new(world);
      ecs_add_id(world, e, local_id);
      ecs_add_id(world, e,
                 static_cast<ecs_entity_t>(fx.sync.body_id()));
      auto* lt = static_cast<engine::ecs::LocalTransform*>(
          ecs_get_mut_id(world, e, local_id));
      lt->px = static_cast<float>(i % 8) * 2.0F;
      lt->py = 5.0F + static_cast<float>(i / 8) * 2.0F;
      lt->pz = 0.0F;
      lt->qx = 0.0F;
      lt->qy = 0.0F;
      lt->qz = 0.0F;
      lt->qw = 1.0F;
      lt->sx = 1.0F;
      lt->sy = 1.0F;
      lt->sz = 1.0F;
      // One dynamic body per entity; link both directions.
      const std::uint32_t bidx = make_box(*fx.world.native(), lt->px, lt->py,
                                          lt->pz, static_cast<std::uint64_t>(e));
      auto* link = static_cast<engine::physics::PhysicsBody*>(ecs_get_mut_id(
          world, e, static_cast<ecs_entity_t>(fx.sync.body_id())));
      link->body_id = bidx;
      link->kinematic = 0;  // sim-driven
      entities.push_back(e);
      // Authoritative spawn: force Jolt to the Flecs pose.
      JPH::RVec3 pos(lt->px, lt->py, lt->pz);
      fx.world.native()->GetBodyInterface().SetPositionAndRotation(
          JPH::BodyID(bidx), pos, JPH::Quat::sIdentity(),
          JPH::EActivation::Activate);
    }
    failures += check(fx.world.body_count() == kBodies, "sync: 64 bodies");

    // AC1: mark body 0 kinematic, move it in Flecs, push -> Jolt moved.
    {
      auto* link0 = static_cast<engine::physics::PhysicsBody*>(ecs_get_mut_id(
          world, entities[0], static_cast<ecs_entity_t>(fx.sync.body_id())));
      link0->kinematic = 1;
      auto* lt0 = static_cast<engine::ecs::LocalTransform*>(
          ecs_get_mut_id(world, entities[0], local_id));
      lt0->px = 42.0F;
      lt0->py = 43.0F;
      lt0->pz = 44.0F;
      const std::uint32_t pushed = fx.sync.push_to_physics(*fx.world.native());
      failures += check(pushed >= 1, "AC1: push >= 1 kinematic");
      JPH::RVec3 p;
      JPH::Quat q;
      fx.world.native()->GetBodyInterface().GetPositionAndRotation(
          JPH::BodyID(link0->body_id), p, q);
      const bool moved = (p.GetX() > 41.9F && p.GetX() < 42.1F &&
                          p.GetY() > 42.9F && p.GetY() < 43.1F);
      failures += check(moved, "AC1: jolt body at pushed pose");
      link0->kinematic = 0;  // back to sim-driven for the drop test
    }

    // Step + AC2 pull: bodies fall under gravity; Flecs must follow.
    float y_before = 0.0F;
    {
      auto* lt = static_cast<engine::ecs::LocalTransform*>(
          ecs_get_mut_id(world, entities[1], local_id));
      y_before = lt->py;
    }
    failures += check(fx.world.step(1.0F / 60.0F, 1), "step: 1/60s");
    const std::uint32_t synced = fx.sync.pull_from_physics(
        *fx.world.native(), static_cast<void*>(fx.sched.native()),
        fx.sched.worker_count());
    failures += check(synced > 0, "AC2: pull synced > 0 bodies");
    {
      auto* lt = static_cast<engine::ecs::LocalTransform*>(
          ecs_get_mut_id(world, entities[1], local_id));
      failures += check(lt->py < y_before, "AC2: flecs Y fell with gravity");
    }

    // AC3: force two bodies to overlap -> contact Enter events in ring.
    {
      auto* l0 = static_cast<engine::physics::PhysicsBody*>(ecs_get_mut_id(
          world, entities[2], static_cast<ecs_entity_t>(fx.sync.body_id())));
      auto* l1 = static_cast<engine::physics::PhysicsBody*>(ecs_get_mut_id(
          world, entities[3], static_cast<ecs_entity_t>(fx.sync.body_id())));
      JPH::RVec3 same(0.0F, 2.0F, 0.0F);
      auto& iface = fx.world.native()->GetBodyInterface();
      iface.SetPositionAndRotation(JPH::BodyID(l0->body_id), same,
                                   JPH::Quat::sIdentity(),
                                   JPH::EActivation::Activate);
      iface.SetPositionAndRotation(JPH::BodyID(l1->body_id), same,
                                   JPH::Quat::sIdentity(),
                                   JPH::EActivation::Activate);
      for (int s = 0; s < 10; ++s) {
        fx.world.step(1.0F / 60.0F, 1);
      }
      std::vector<engine::physics::CollisionEvent> evts(4096);
      const std::size_t n = fx.sync.drain_events(evts.data(), evts.size());
      int enters = 0;
      for (std::size_t i = 0; i < n; ++i) {
        if (evts[i].kind == engine::physics::CollisionEventKind::kEnter) {
          ++enters;
        }
      }
      failures += check(enters > 0, "AC3: contact Enter events captured");
      std::printf("info: drained %zu events (%d enters)\n", n, enters);
    }

    // Teardown order matters: the sync owns a cached query on this world,
    // so destroy it BEFORE ecs_fini (query_fini on a dead world is UAF).
    fx.sync.destroy();
    ecs_fini(world);
    fx.world.destroy();
    fx.arena.destroy();
    fx.sched.shutdown();
  }

  if (failures == 0) {
    std::printf("M4-02 sync: ALL PASS\n");
    return 0;
  }
  std::printf("M4-02 sync: %d FAILURES\n", failures);
  return 1;
}

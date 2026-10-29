// test_physics_queries.cpp — M4-03 acceptance: zero-cost C# spatial queries.
//
// Native-side proof for Physics.Raycast / SphereCast / OverlapSphere /
// RaycastBatch before the managed bindings land:
//   AC1 — single ray hits a known box (fraction/point/normal/body/userdata).
//   AC1 — ray missing everything reports miss.
//   AC1 — sphere cast hits; overlap sphere lists the bodies.
//   AC2 — batch of N rays across enkiTS lanes matches serial results.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <Jolt/Jolt.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/EActivation.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include "engine/core/job_scheduler.hpp"
#include "engine/core/page_arena_allocator.hpp"
#include "engine/physics/layers.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/physics/spatial_queries.hpp"

namespace {

auto check(bool ok, const char* name) -> int {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  return ok ? 0 : 1;
}

struct Fixture {
  engine::core::JobScheduler sched;
  engine::core::PageArenaAllocator arena;
  engine::physics::PhysicsWorld world;
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
    ok = true;
    return true;
  }

  ~Fixture() {
    world.destroy();
    arena.destroy();
    sched.shutdown();
  }
};

// Create one dynamic box body at (x,y,z), UserData = user_data. Returns the
// full Jolt BodyID (index+sequence).
auto make_box(JPH::PhysicsSystem& sys, const JPH::Shape* shape, float x,
              float y, float z, std::uint64_t user_data) -> std::uint32_t {
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

auto feq(float a, float b) -> bool {
  const float d = a - b;
  return d > -1e-3F && d < 1e-3F;
}

}  // namespace

auto main() -> int {
  setvbuf(stdout, nullptr, _IONBF, 0);
  // Windows DLL-duplication guard (same as test_physics_sync): this TU's
  // Jolt globals are a second copy when engine_core is a DLL; register this
  // TU's copy before the first Jolt heap allocation.
  JPH::RegisterDefaultAllocator();
  int failures = 0;

  failures += check(sizeof(engine::physics::SpatialHit) == 48,
                    "SpatialHit 48B static");
  failures += check(sizeof(engine::physics::RayInput) == 24,
                    "RayInput 24B static");

  Fixture fx;
  failures += check(fx.init(), "fixture: scheduler+arena+world");

  // One shared shape Ref for all bodies.
  JPH::BoxShapeSettings shape_settings(JPH::Vec3(0.5F, 0.5F, 0.5F));
  const JPH::Shape::ShapeResult shape_res = shape_settings.Create();
  failures += check(!shape_res.HasError(), "box shape create");
  const JPH::ShapeRefC shape = shape_res.Get();

  JPH::PhysicsSystem* sys = fx.world.native();
  failures += check(sys != nullptr, "world native non-null");

  // Ground: big static-adjacent dynamic box at origin; target box at x=5.
  const std::uint32_t ground_id = make_box(*sys, shape, 0.0F, -2.0F, 0.0F, 101);
  const std::uint32_t target_id = make_box(*sys, shape, 5.0F, 0.0F, 0.0F, 202);
  (void)ground_id;
  failures += check(fx.world.body_count() == 2, "two bodies live");

  // --- AC1: single raycast hits the target box ---------------------------
  {
    // Ray from x=-10 toward +x, length 30: first face of target box at x=4.5
    // (center 5, half-extent 0.5). Ground box top at y=-1.5 — the ray at y=0
    // passes above it.
    engine::physics::RayInput ray{-10.0F, 0.0F, 0.0F, 30.0F, 0.0F, 0.0F};
    engine::physics::SpatialHit hit{};
    const int32_t rc = engine::physics::spatial_raycast(*sys, &ray, &hit);
    failures += check(rc == 1 && hit.hit == 1, "raycast: hit reported");
    failures += check(hit.body_id == target_id, "raycast: body id matches");
    failures += check(hit.user_data == 202, "raycast: user data (entity)");
    failures += check(feq(hit.fraction, 14.5F / 30.0F), "raycast: fraction");
    failures += check(feq(hit.px, -10.0F + 14.5F), "raycast: point x");
    failures += check(feq(hit.nx, -1.0F) && feq(hit.ny, 0.0F),
                      "raycast: normal -x");
  }

  // --- AC1: clean miss ----------------------------------------------------
  {
    engine::physics::RayInput ray{0.0F, 50.0F, 0.0F, 0.0F, 10.0F, 0.0F};
    engine::physics::SpatialHit hit{};
    const int32_t rc = engine::physics::spatial_raycast(*sys, &ray, &hit);
    failures += check(rc == 0 && hit.hit == 0, "raycast: miss reported");
  }

  // --- AC1: sphere cast ---------------------------------------------------
  {
    engine::physics::RayInput ray{-10.0F, 0.0F, 0.0F, 30.0F, 0.0F, 0.0F};
    engine::physics::SpatialHit hit{};
    const int32_t rc =
        engine::physics::spatial_sphere_cast(*sys, &ray, 0.25F, &hit);
    failures += check(rc == 1 && hit.hit == 1, "spherecast: hit reported");
    failures += check(hit.body_id == target_id, "spherecast: body id");
    failures += check(hit.fraction < 14.5F / 30.0F + 1e-3F,
                      "spherecast: fraction <= ray fraction");
  }

  // --- AC1: overlap sphere ------------------------------------------------
  {
    std::vector<std::uint32_t> ids(16, 0);
    std::vector<std::uint64_t> ud(16, 0);
    engine::physics::OverlapOut out{};
    out.body_ids = ids.data();
    out.user_data = ud.data();
    out.capacity = 16;
    const std::uint32_t n =
        engine::physics::spatial_overlap_sphere(*sys, 5.0F, 0.0F, 0.0F, 2.0F, &out);
    failures += check(n == 1 && out.count == 1, "overlap: one body");
    bool found_target = false;
    for (std::uint32_t i = 0; i < out.count; ++i) {
      if (out.body_ids[i] == target_id && out.user_data[i] == 202) {
        found_target = true;
      }
    }
    failures += check(found_target, "overlap: target body + userdata");
    // Wide sphere: both bodies.
    const std::uint32_t n2 =
        engine::physics::spatial_overlap_sphere(*sys, 2.5F, -1.0F, 0.0F, 10.0F, &out);
    failures += check(n2 == 2, "overlap: wide sphere finds both");
  }

  // --- AC2: batch matches serial ------------------------------------------
  {
    constexpr std::uint32_t kRays = 256;
    std::vector<engine::physics::RayInput> rays(kRays);
    std::vector<engine::physics::SpatialHit> serial(kRays), batch(kRays);
    for (std::uint32_t i = 0; i < kRays; ++i) {
      const float y = -5.0F + static_cast<float>(i) * (10.0F / kRays);
      rays[i] = engine::physics::RayInput{-10.0F, y, 0.0F, 30.0F, 0.0F, 0.0F};
    }
    for (std::uint32_t i = 0; i < kRays; ++i) {
      engine::physics::spatial_raycast(*sys, &rays[i], &serial[i]);
    }
    const std::uint32_t done = engine::physics::spatial_raycast_batch(
        *sys, fx.sched.native(), fx.sched.worker_count(), rays.data(),
        batch.data(), kRays);
    failures += check(done == kRays, "batch: all rays processed");
    bool match = true;
    for (std::uint32_t i = 0; i < kRays && match; ++i) {
      match = serial[i].hit == batch[i].hit &&
              serial[i].body_id == batch[i].body_id &&
              feq(serial[i].fraction, batch[i].fraction);
    }
    failures += check(match, "batch: parallel == serial");
    // Serial fallback (null scheduler) also matches.
    std::vector<engine::physics::SpatialHit> fallback(kRays);
    const std::uint32_t done2 = engine::physics::spatial_raycast_batch(
        *sys, nullptr, 0, rays.data(), fallback.data(), kRays);
    bool match2 = done2 == kRays;
    for (std::uint32_t i = 0; i < kRays && match2; ++i) {
      match2 = serial[i].hit == fallback[i].hit &&
               serial[i].body_id == fallback[i].body_id;
    }
    failures += check(match2, "batch: null-sched fallback == serial");
  }

  // --- Forward-progress benchmark (informational, gate-measured in CI) ----
  {
    constexpr std::uint32_t kRays = 4096;
    std::vector<engine::physics::RayInput> rays(kRays);
    std::vector<engine::physics::SpatialHit> hits(kRays);
    for (std::uint32_t i = 0; i < kRays; ++i) {
      const float y = -5.0F + static_cast<float>(i) * (10.0F / kRays);
      rays[i] = engine::physics::RayInput{-10.0F, y, 0.0F, 30.0F, 0.0F, 0.0F};
    }
    const std::uint32_t done = engine::physics::spatial_raycast_batch(
        *sys, fx.sched.native(), fx.sched.worker_count(), rays.data(),
        hits.data(), kRays);
    failures += check(done == kRays, "batch: 4k benchmark processed");
    std::uint32_t hit_count = 0;
    for (const auto& h : hits) {
      hit_count += h.hit;
    }
    std::printf("[info] batch 4k: %u hits\n", hit_count);
    failures += check(hit_count > 0, "batch: benchmark hits nonzero");
  }

  if (failures == 0) {
    std::printf("M4-03 physics queries: ALL PASS\n");
    return 0;
  }
  std::printf("M4-03 physics queries: %d FAILURES\n", failures);
  return 1;
}

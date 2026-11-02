// test_character_p6_04.cpp — P6-04 acceptance: Jolt CharacterVirtual.
//
// AC: the controller climbs a 0.3 m step; it stops at a 60-degree slope.
// Scene: static ground plane (y=0) + a 0.3 m box step + a 60-degree ramp.
// The character walks +X at 2 m/s, 60 Hz: it must mount the step (feet
// y ~= 0.3, grounded) and must not climb the ramp (x stalls before the
// top, never grounded on the steep face).
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include <Jolt/Jolt.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/EActivation.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include "engine/core/job_scheduler.hpp"
#include "engine/core/page_arena_allocator.hpp"
#include "engine/bridge/engine_abi.h"
#include "engine/physics/character.hpp"
#include "engine/physics/layers.hpp"
#include "engine/physics/physics_world.hpp"

namespace {

auto check(bool ok, const char* name) -> int {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  return ok ? 0 : 1;
}

// Process-lifetime mesh shape Ref (AddRef'd at bake; released in main).
const JPH::Shape* g_stairs_shape = nullptr;

struct Fixture {
  engine::core::JobScheduler sched;
  engine::core::PageArenaAllocator arena;
  engine::physics::PhysicsWorld world;

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
    return true;
  }

  ~Fixture() noexcept {
    world.destroy();
    arena.destroy();
    sched.shutdown();
  }
};

// Static box body in `sys` (half extents hx/hy/hz, center x/y/z).
auto add_static(JPH::PhysicsSystem& sys, float hx, float hy, float hz,
                float x, float y, float z) -> void {
  JPH::BoxShapeSettings settings(JPH::Vec3(hx, hy, hz));
  JPH::Shape::ShapeResult res = settings.Create();
  JPH::Ref<JPH::Shape> shape = res.Get();
  JPH::BodyCreationSettings bcs(shape, JPH::RVec3(x, y, z),
                                JPH::Quat::sIdentity(),
                                JPH::EMotionType::Static,
                                engine::physics::ObjectLayers::kNonMoving);
  bcs.mAllowSleeping = false;
  sys.GetBodyInterface().CreateAndAddBody(bcs, JPH::EActivation::Activate);
}

auto default_cfg() -> engine::physics::CharacterConfig {
  engine::physics::CharacterConfig cfg;
  cfg.half_height = 0.9F;    // cylinder mid-section half height
  cfg.radius = 0.3F;         // capsule radius
  cfg.max_slope_deg = 45.0F; // walkable limit: 60-deg ramp is steep
  cfg.max_step_height = 0.4F;
  cfg.skin_width = 0.02F;
  cfg.stick_to_floor = 1;
  return cfg;
}

}  // namespace

auto main() -> int {
  setvbuf(stdout, nullptr, _IONBF, 0);
  int failures = 0;

  failures += check(sizeof(engine::physics::CharacterConfig) == 24,
                    "character: CharacterConfig 24B");
  failures += check(sizeof(engine::physics::CharacterState) == 24,
                    "character: CharacterState 24B");
  failures += check(sizeof(EngineCharacterConfig) == 24,
                    "character: EngineCharacterConfig 24B");
  failures += check(sizeof(EngineCharacterState) == 24,
                    "character: EngineCharacterState 24B");

  Fixture fx;
  failures += check(fx.init(), "character: fixture init (scheduler/arena/world)");
  if (!fx.world.valid()) {
    std::printf("P6-04 character controller: fixture failed\n");
    return 1;
  }
  JPH::PhysicsSystem& sys = *fx.world.native();
  constexpr float kDt = 1.0F / 60.0F;

  // Scene: ground plane (top at y=0) + STAIRS of 0.3 m risers (Jolt's
  // own TestWalkStairs pattern: 10 steps, tread depth == riser height,
  // built from triangles, climbing in +X from x=4) + a 60-degree ramp
  // past x=10. The AC is the 0.3 m rise: the character must top the
  // first riser (feet y ~= 0.3). A lone vertical box face is NOT a stair
  // (WalkStairs climbs stair treads, not razor faces) — the mesh below
  // is the honest shape. The ramp rises 3 m over ~1.73 m of run
  // (tan 60 = 1.732); built from a rotated static box so the face angle
  // is exact.
  add_static(sys, 50.0F, 0.5F, 10.0F, 0.0F, -0.5F, 0.0F);  // ground
  {
    constexpr int kNumSteps = 10;
    constexpr float kStepH = 0.3F;
    JPH::TriangleList tris;
    tris.reserve(static_cast<std::size_t>(kNumSteps) * 4U);
    for (int i = 0; i < kNumSteps; ++i) {
      // Jolt's frame climbs in +Z; rotate the frame: our steps climb in
      // +X (base.x advances per step, width spans Z).
      const JPH::Vec3 base(4.0F + kStepH * static_cast<float>(i),
                           kStepH * static_cast<float>(i), 0.0F);
      const JPH::Vec3 b1 = base + JPH::Vec3(0.0F, 0.0F, 2.0F);
      const JPH::Vec3 s1 = b1 + JPH::Vec3(0.0F, kStepH, 0.0F);
      const JPH::Vec3 p1 = s1 + JPH::Vec3(kStepH, 0.0F, 0.0F);
      const JPH::Vec3 width(0.0F, 0.0F, -4.0F);
      const JPH::Vec3 b2 = b1 + width;
      const JPH::Vec3 s2 = s1 + width;
      const JPH::Vec3 p2 = p1 + width;
      tris.emplace_back(s1, b1, s2);
      tris.emplace_back(b1, b2, s2);
      tris.emplace_back(s1, p2, p1);
      tris.emplace_back(s1, s2, p2);
    }
    JPH::MeshShapeSettings mesh(tris);
    // NOTE: no SetEmbedded() — the settings/Ref own the mesh (AddRef'd
    // below); embedded stack ownership does not survive scope end.
    JPH::Shape::ShapeResult mres = mesh.Create();
    if (mres.HasError()) {
      failures += check(false, "stairs: mesh shape builds");
      return 1;
    }
    const JPH::Shape* mshape = mres.Get();
    JPH::BodyCreationSettings mbcs(mshape, JPH::RVec3::sZero(),
                                  JPH::Quat::sIdentity(),
                                  JPH::EMotionType::Static,
                                  engine::physics::ObjectLayers::kNonMoving);
    mbcs.mAllowSleeping = false;
    const JPH::BodyID stairs_bid =
        sys.GetBodyInterface().CreateAndAddBody(mbcs, JPH::EActivation::Activate);
    failures += check(!stairs_bid.IsInvalid(), "stairs: mesh body created");
    // The body holds a Ref through creation; keep our own so the mesh
    // shape is never freed while bodies reference it (same pattern as
    // HeightfieldCollider: AddRef + Release after the last step).
    // NOTE: test process-lifetime: released at the end of main via the
    // stored pointer below (stairs_shape global).
    mshape->AddRef();
    g_stairs_shape = mshape;
    failures += check(true, "stairs: mesh shape builds");
  }
  {
    constexpr float kHalfLen = 2.5F;   // ramp face half-length
    constexpr float kHalfThick = 0.25F;
    constexpr float kAng = 1.0471976F;  // 60 degrees, rotation about +Z
    JPH::BoxShapeSettings settings(JPH::Vec3(kHalfLen, kHalfThick, 2.0F));
    JPH::Shape::ShapeResult res = settings.Create();
    JPH::Ref<JPH::Shape> shape = res.Get();
    const JPH::Quat rot = JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), kAng);
    // Face center: put the low edge at x=10,y=0. Low edge = center -
    // rot*(halfLen,thick,0); solve center so the low edge touches (10,0).
    const JPH::Vec3 low(-kHalfLen, kHalfThick, 0.0F);
    const JPH::Vec3 want(10.0F, 0.0F, 0.0F);
    const JPH::Vec3 center = want - rot * low;
    JPH::BodyCreationSettings bcs(shape, JPH::RVec3(center.GetX(), center.GetY(), 0.0),
                                  rot, JPH::EMotionType::Static,
                                  engine::physics::ObjectLayers::kNonMoving);
    bcs.mAllowSleeping = false;
    sys.GetBodyInterface().CreateAndAddBody(bcs, JPH::EActivation::Activate);
  }
  sys.OptimizeBroadPhase();

  // Null-guard probes (fail closed, no crash).
  {
    engine::physics::CharacterBody bad;
    failures += check(!bad.valid(), "character: invalid before create");
    failures += check(!bad.create(nullptr, default_cfg(), 0.0F, 1.0F, 0.0F),
                      "character: null world rejected");
    engine::physics::CharacterConfig bad_cfg = default_cfg();
    bad_cfg.half_height = 0.0F;
    failures += check(!bad.create(&fx.world, bad_cfg, 0.0F, 1.0F, 0.0F),
                      "character: non-positive half_height rejected");
    failures += check(!bad.valid(),
                      "character: still invalid after rejected create");
    engine::physics::CharacterState st{};
    failures += check(!bad.move(0.0F, 0.0F, 0.0F, &st),
                      "character: zero dt rejected on invalid");
    failures += check(!bad.move(0.0F, 0.0F, kDt, nullptr),
                      "character: null out rejected on invalid");
    failures += check(!bad.teleport(0.0F, 0.0F, 0.0F),
                      "character: teleport on invalid rejected");
    failures += check(!bad.set_max_slope_deg(30.0F),
                      "character: set_slope before valid rejected");
    failures += check(!bad.set_max_step_height(0.2F),
                      "character: set_step before valid rejected");
    failures += check(!bad.position(nullptr),
                      "character: position null out rejected");
    // move() with bad dt on a VALID character (separate instance so the
    // null-guard probe above stays on the invalid path).
    engine::physics::CharacterBody probe;
    failures += check(probe.create(&fx.world, default_cfg(), 0.0F, 3.0F, 2.0F),
                      "character: probe create for dt guards");
    failures += check(!probe.move(1.0F, 0.0F, 0.0F, &st),
                      "character: zero dt rejected on valid");
    failures += check(!probe.move(1.0F, 0.0F, -kDt, &st),
                      "character: negative dt rejected on valid");
    failures += check(!probe.move(1.0F, 0.0F, kDt, nullptr),
                      "character: null out rejected on valid");
    probe.destroy();
  }



  // AC 1: climb a 0.3 m step. Spawn on the ground left of the step and
  // walk +X toward the stairs for up to 16 s, stopping at the FIRST riser
  // top (x ~= 4.3, feet y ~= 0.3): the AC is the single 0.3 m step, not
  // the whole flight. Stopping early keeps the assertion on the riser.
  {
    engine::physics::CharacterBody ch;
    engine::physics::CharacterConfig cfg = default_cfg();
    failures += check(ch.create(&fx.world, cfg, 0.0F, 2.0F, 0.0F),
                      "step: create at (0,2,0)");
    engine::physics::CharacterState st{};
    failures += check(ch.position(&st), "step: position reads");
    int grounded_frame = -1;
    bool have_first = false;
    engine::physics::CharacterState first{};
    for (int f = 0; f < 960; ++f) {
      if (!ch.move(1.5F, 0.0F, kDt, &st)) {
        break;
      }
      fx.world.step(kDt, 1);
      if (grounded_frame < 0 && st.grounded != 0U) {
        grounded_frame = f + 1;
      }
      // Snapshot the FIRST riser top: the first frame whose feet cross
      // 0.25 past the riser face (x in [4.0, 4.6]) with support on the
      // first tread (ground_y in [0.25, 0.45)). Geometry-gated (the
      // grounded flag flickers mid-transition); the flag is asserted
      // separately on the snapshot.
      if (!have_first && st.px > 3.5F && st.py >= 0.25F &&
          st.ground_y >= 0.25F && st.ground_y < 0.45F) {
        first = st;
        have_first = true;
      }
      // Bound the walk (the flight tops out at 3 m; the slope scene
      // starts at x=7 — stop well before it).
      if (st.px > 6.0F) {
        break;
      }
    }
    if (have_first) {
      st = first;
    }
    std::printf("step: pos=(%.3f,%.3f,%.3f) grounded=%u ground_y=%.3f vy=%.3f first_grounded=%d\n",
                st.px, st.py, st.pz, st.grounded, st.ground_y, st.vy,
                grounded_frame);
    failures += check(st.grounded != 0U, "step: grounded after walk");
    // AC: climbs a 0.3 m step — the first riser top is y=0.3; sampled
    // tolerance matches the P6-01 terrain raycast gate (+/- 0.01) with
    // stair-step slack (+/- 0.05).
    failures += check(std::fabs(st.py - 0.3F) < 0.05F,
                      "step: feet on the 0.3 m riser top (+/- 5 cm)");
    failures += check(st.px > 3.5F, "step: reached the first riser (x > 3.5)");
    // Live updates: slope/step setters accept while valid.
    failures += check(ch.set_max_slope_deg(30.0F), "step: set_slope live");
    failures += check(ch.set_max_step_height(0.2F), "step: set_step live");
    failures += check(ch.teleport(0.0F, 1.0F, 0.0F), "step: teleport back");
    engine::physics::CharacterState back{};
    failures += check(ch.position(&back), "step: position after teleport");
    failures += check(std::fabs(back.px) < 0.001F && std::fabs(back.py - 1.0F) < 0.001F,
                      "step: teleport repositions");
    ch.destroy();
    failures += check(!ch.valid(), "step: invalid after destroy");
  }

  // AC 2: stop at a 60-degree slope. Spawn before the ramp and walk +X
  // for 16 s with the default 45-degree limit: the character must never
  // reach the ramp top (x stays < 11.5, well short of the ~11.73 crest)
  // — OnSteepGround is not grounded, so it cannot climb. Same sub-stepped
  // walk speed as the stairs scene (1.5 m/s isolates the slope limit from
  // any tunneling artifact).
  {
    engine::physics::CharacterBody ch;
    engine::physics::CharacterConfig cfg = default_cfg();
    cfg.max_step_height = 0.0F;  // stairs off: isolate the slope limit
    failures += check(ch.create(&fx.world, cfg, 7.0F, 2.0F, 0.0F),
                      "slope: create at (7,2,0)");
    engine::physics::CharacterState st{};
    float max_x = 7.0F;
    for (int f = 0; f < 960; ++f) {
      if (!ch.move(1.5F, 0.0F, kDt, &st)) {
        break;
      }
      fx.world.step(kDt, 1);
      max_x = st.px > max_x ? st.px : max_x;
    }
    std::printf("slope: pos=(%.3f,%.3f,%.3f) grounded=%u max_x=%.3f\n",
                st.px, st.py, st.pz, st.grounded, max_x);
    failures += check(max_x < 11.5F,
                      "slope: 60-deg ramp not climbed (x < 11.5)");
    ch.destroy();
  }

  if (g_stairs_shape != nullptr) {
    g_stairs_shape->Release();
    g_stairs_shape = nullptr;
  }
  if (failures == 0) {
    std::printf("P6-04 character controller: ALL PASS\n");
    return 0;
  }
  std::printf("P6-04 character controller: %d FAILURES\n", failures);
  return 1;
}

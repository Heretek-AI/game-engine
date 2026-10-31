// test_physics_2d.cpp — P6-07 acceptance: Box2D v3 10-box stack settles.
//
// AC: 10 dynamic boxes drop onto a static ground slab; after 4 s at
// 60 Hz every box sleeps (speed ~0, b2Body_IsAwake false) and the drain
// reports a non-zero begin-contact count (>= 10, one per box landing).
// The drain shares engine::physics::CollisionEventRing — the same 32B
// record the 3D path delivers to C# (P2-05 shape parity).
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "engine/physics/collision_events.hpp"
#include "engine/physics/components2d.hpp"
#include "engine/physics/world2d.hpp"

namespace {

auto check(bool ok, const char* name) -> int {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  return ok ? 0 : 1;
}

}  // namespace

auto main() -> int {
  setvbuf(stdout, nullptr, _IONBF, 0);
  int failures = 0;

  failures += check(sizeof(engine::physics::Rigidbody2D) == 64, "2d: Rigidbody2D 64B");
  failures += check(sizeof(engine::physics::BoxCollider2D) == 64, "2d: BoxCollider2D 64B");
  failures += check(sizeof(engine::physics::CircleCollider2D) == 64, "2d: CircleCollider2D 64B");
  failures += check(sizeof(engine::physics::PolygonCollider2D) == 64, "2d: PolygonCollider2D 64B");
  failures += check(sizeof(engine::physics::CollisionEvent) == 32, "2d: CollisionEvent 32B (3D parity)");

  engine::physics::World2D world;
  failures += check(world.construct({}), "2d: world construct");
  failures += check(world.valid(), "2d: world valid");

  engine::physics::CollisionEventRing ring;
  failures += check(ring.init(1U << 14), "2d: ring init 16k");

  // Static ground slab: 100x20 box at y=-10 (top surface at y=0).
  engine::physics::Rigidbody2D ground_rb;
  ground_rb.type = engine::physics::Body2DType::kStatic;
  engine::physics::BoxCollider2D ground_box;
  ground_box.hx = 50.0F;
  ground_box.hy = 10.0F;
  const b2BodyId ground =
      world.create_body(1, 0.0F, -10.0F, 0.0F, ground_rb, ground_box);
  failures += check(!B2_IS_NULL(ground), "2d: ground created");

  // 10 dynamic boxes (1x1) stacked with a 0.05 gap, dropped from y=2.
  engine::physics::Rigidbody2D rb;
  rb.type = engine::physics::Body2DType::kDynamic;
  engine::physics::BoxCollider2D box;
  box.hx = 0.5F;
  box.hy = 0.5F;
  box.friction = 0.6F;
  b2BodyId ids[10]{};
  for (int i = 0; i < 10; ++i) {
    ids[i] = world.create_body(static_cast<std::uint64_t>(i + 2), 0.0F,
                               2.0F + static_cast<float>(i) * 1.05F, 0.0F, rb, box);
  }
  bool all_created = true;
  for (auto id : ids) {
    all_created = all_created && !B2_IS_NULL(id);
  }
  failures += check(all_created, "2d: 10 boxes created");
  failures += check(world.body_count() == 11, "2d: body_count 11");

  // Circle + polygon collider smoke: one of each, off to the side.
  engine::physics::CircleCollider2D circ;
  circ.radius = 0.5F;
  const b2BodyId cb =
      world.create_body(100, -5.0F, 5.0F, 0.0F, rb, circ);
  failures += check(!B2_IS_NULL(cb), "2d: circle body created");
  engine::physics::PolygonCollider2D poly;
  poly.count = 3;
  poly.vx[0] = -0.5F;
  poly.vy[0] = -0.5F;
  poly.vx[1] = 0.5F;
  poly.vy[1] = -0.5F;
  poly.vx[2] = 0.0F;
  poly.vy[2] = 0.5F;
  const b2BodyId pb =
      world.create_body(101, 5.0F, 5.0F, 0.0F, rb, poly);
  failures += check(!B2_IS_NULL(pb), "2d: polygon body created");

  constexpr float kDt = 1.0F / 60.0F;
  std::uint32_t total_begins = 0;
  for (int step = 0; step < 240; ++step) {
    const std::uint32_t n = world.step(kDt, &ring);
    // Count kEnter records drained this step.
    engine::physics::CollisionEvent ev{};
    // (ring already received them via step; count via drain below)
    (void)ev;
    total_begins += n;
  }
  // Drain anything left and count kEnter vs kExit.
  std::uint32_t enters = 0;
  std::uint32_t exits = 0;
  engine::physics::CollisionEvent batch[512];
  for (;;) {
    const std::size_t got = ring.drain_to(batch, 512);
    if (got == 0) {
      break;
    }
    for (std::size_t i = 0; i < got; ++i) {
      if (batch[i].kind == engine::physics::CollisionEventKind::kEnter) {
        ++enters;
      } else {
        ++exits;
      }
    }
  }
  failures += check(enters >= 10, "2d: >=10 begin contacts (one per box)");
  std::printf("[INFO] 2d: step-drained=%u enters=%u exits=%u\n", total_begins, enters, exits);

  // Settle check: every stack box slow and asleep, resting at its layer.
  // Poses come from the world's own query (single Box2D copy inside
  // engine_core — the test TU must not call b2* directly, or it links a
  // second static copy with a disjoint world table; see P6-07 notes).
  bool settled = true;
  for (int i = 0; i < 10; ++i) {
    float px = 0.0F;
    float py = 0.0F;
    float speed = 0.0F;
    bool awake = true;
    if (!world.body_state(ids[i], &px, &py, &speed, &awake)) {
      settled = false;
      continue;
    }
    const float want_y = 0.5F + static_cast<float>(i) * 1.0F;
    const bool ok = !awake && speed < 0.05F && std::fabs(px) < 0.15F &&
                    std::fabs(py - want_y) < 0.15F;
    if (!ok) {
      std::printf("[INFO] 2d: box%d y=%.3f speed=%.4f awake=%d\n", i, py,
                  speed, awake ? 1 : 0);
      settled = false;
    }
  }
  failures += check(settled, "2d: 10-box stack settles + sleeps");

  // destroy_body smoke.
  world.destroy_body(cb);
  failures += check(world.body_count() == 12, "2d: destroy_body decrements");

  world.destroy();
  failures += check(!world.valid(), "2d: world destroyed");
  ring.destroy();

  std::printf(failures == 0 ? "2D-PHYSICS-OK\n" : "2D-PHYSICS-FAIL %d\n", failures);
  return failures == 0 ? 0 : 1;
}

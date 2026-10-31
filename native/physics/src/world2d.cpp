// world2d.cpp — P6-07: Box2D v3 world adapter (2D physics kernel).
//
// WARNING (load-bearing include rule): this TU is the ONLY one (besides
// a test TU linking engine_core) that may textually include
// <box2d/box2d.h>. It arrives via the engine_box2d_wrapper interface
// (SYSTEM + -Wno-conversion), so -Wconversion -Werror never fires inside
// Box2D headers. Box2D is a C API (B2_API extern "C") — no exceptions
// cross it; this TU is noexcept throughout.
#include "engine/physics/world2d.hpp"

// Box2D v3 C API (P6-00 spike call sequence, §4.12):
//   b2WorldDef wdef = b2DefaultWorldDef(); wdef.gravity = {gx, gy};
//   b2WorldId world = b2CreateWorld(&wdef);
//   b2BodyId b = b2CreateBody(world, &bdef);   // userData = Flecs entity
//   b2ShapeDef sdef = b2DefaultShapeDef();     // friction via sdef.material
//   sdef.enableContactEvents = true;           // opt-in per shape (§4.12.2)
//   b2CreatePolygonShape(b, &sdef, &b2MakeBox(hx, hy));
//   b2World_Step(world, dt, subSteps); ev = b2World_GetContactEvents(world);
#include <box2d/box2d.h>

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::physics {

namespace {

// Box2D body type from the 2D rigidbody component.
auto to_b2_type(Body2DType t) noexcept -> b2BodyType {
  switch (t) {
    case Body2DType::kStatic:
      return b2_staticBody;
    case Body2DType::kKinematic:
      return b2_kinematicBody;
    default:
      return b2_dynamicBody;
  }
}

// Shared shape-def fill: density + friction (v3: sdef.material.friction —
// sdef.friction does not exist, §4.12 gotcha 1) + contact-event opt-in
// (§4.12 gotcha 2) + sensor flag in shape user-data bit 0 (read back at
// drain time for the event's sensor field).
auto fill_shape_def(b2ShapeDef* sdef, float density, float friction,
                    bool sensor) noexcept -> void {
  sdef->density = density;
  sdef->material.friction = friction;
  sdef->enableContactEvents = true;
  sdef->userData = reinterpret_cast<void*>(sensor ? 1 : 0);  // NOLINT
}

// Shared body-def fill: type, pose, damping, sleep, user-data = entity.
auto fill_body_def(b2BodyDef* bdef, std::uint64_t entity, float px, float py,
                   float angle, const Rigidbody2D& rb) noexcept -> void {
  bdef->type = to_b2_type(rb.type);
  bdef->position = {px, py};
  bdef->rotation = b2MakeRot(angle);
  bdef->linearDamping = rb.linear_damping;
  bdef->angularDamping = rb.angular_damping;
  bdef->gravityScale = rb.gravity_scale;
  bdef->enableSleep = rb.allow_sleeping;
  bdef->isBullet = rb.bullet;
  bdef->isEnabled = rb.active;
  bdef->motionLocks.angularZ = rb.fixed_rotation;
  bdef->userData = reinterpret_cast<void*>(entity);  // NOLINT
}

}  // namespace

World2D::~World2D() noexcept { destroy(); }

auto World2D::construct(const World2DConfig& cfg) noexcept -> bool {
  if (!B2_IS_NULL(world_)) {
    return false;
  }
  b2WorldDef wdef = b2DefaultWorldDef();
  wdef.gravity = {cfg.gravity_x, cfg.gravity_y};
  world_ = b2CreateWorld(&wdef);
  if (B2_IS_NULL(world_)) {
    return false;
  }
  b2World_EnableSleeping(world_, true);
  sub_steps_ = cfg.sub_steps == 0 ? 4 : cfg.sub_steps;
  return true;
}

auto World2D::destroy() noexcept -> void {
  if (!B2_IS_NULL(world_)) {
    b2DestroyWorld(world_);
    world_ = b2_nullWorldId;
  }
  body_count_ = 0;
}

auto World2D::create_body(std::uint64_t entity, float px, float py,
                          float angle, const Rigidbody2D& rb,
                          const BoxCollider2D& box) noexcept -> b2BodyId {
  if (B2_IS_NULL(world_) || entity == 0) {
    return b2_nullBodyId;
  }
  b2BodyDef bdef = b2DefaultBodyDef();
  fill_body_def(&bdef, entity, px, py, angle, rb);
  const b2BodyId body = b2CreateBody(world_, &bdef);
  if (B2_IS_NULL(body)) {
    return b2_nullBodyId;
  }
  ++body_count_;
  b2ShapeDef sdef = b2DefaultShapeDef();
  fill_shape_def(&sdef, box.density, box.friction, box.sensor || rb.sensor);
  const b2Polygon poly = b2MakeBox(box.hx, box.hy);
  const b2ShapeId shape = b2CreatePolygonShape(body, &sdef, &poly);
  if (B2_IS_NULL(shape)) {
    b2DestroyBody(body);
    --body_count_;
    return b2_nullBodyId;
  }
  // Bodies are active by default (bdef.isEnabled) — no wake call needed.
  return body;
}

auto World2D::create_body(std::uint64_t entity, float px, float py,
                          float angle, const Rigidbody2D& rb,
                          const CircleCollider2D& circle) noexcept -> b2BodyId {
  if (B2_IS_NULL(world_) || entity == 0) {
    return b2_nullBodyId;
  }
  b2BodyDef bdef = b2DefaultBodyDef();
  fill_body_def(&bdef, entity, px, py, angle, rb);
  const b2BodyId body = b2CreateBody(world_, &bdef);
  if (B2_IS_NULL(body)) {
    return b2_nullBodyId;
  }
  ++body_count_;
  b2ShapeDef sdef = b2DefaultShapeDef();
  fill_shape_def(&sdef, circle.density, circle.friction,
                 circle.sensor || rb.sensor);
  const b2Circle c{{circle.cx, circle.cy}, circle.radius};
  const b2ShapeId shape = b2CreateCircleShape(body, &sdef, &c);
  if (B2_IS_NULL(shape)) {
    b2DestroyBody(body);
    --body_count_;
    return b2_nullBodyId;
  }
  return body;
}

auto World2D::create_body(std::uint64_t entity, float px, float py,
                          float angle, const Rigidbody2D& rb,
                          const PolygonCollider2D& poly) noexcept -> b2BodyId {
  constexpr int kMaxVerts = 6;
  if (B2_IS_NULL(world_) || entity == 0 || poly.count == 0 ||
      poly.count > kMaxVerts) {
    return b2_nullBodyId;
  }
  b2BodyDef bdef = b2DefaultBodyDef();
  fill_body_def(&bdef, entity, px, py, angle, rb);
  const b2BodyId body = b2CreateBody(world_, &bdef);
  if (B2_IS_NULL(body)) {
    return b2_nullBodyId;
  }
  ++body_count_;
  b2Vec2 pts[kMaxVerts];
  for (int i = 0; i < poly.count; ++i) {
    pts[i] = {poly.vx[i], poly.vy[i]};
  }
  const b2Hull hull = b2ComputeHull(pts, poly.count);
  if (hull.count == 0) {
    b2DestroyBody(body);
    --body_count_;
    return b2_nullBodyId;
  }
  b2ShapeDef sdef = b2DefaultShapeDef();
  fill_shape_def(&sdef, poly.density, poly.friction,
                 poly.sensor != 0 || rb.sensor);
  const b2Polygon shape_poly = b2MakePolygon(&hull, 0.0F);
  const b2ShapeId shape = b2CreatePolygonShape(body, &sdef, &shape_poly);
  if (B2_IS_NULL(shape)) {
    b2DestroyBody(body);
    --body_count_;
    return b2_nullBodyId;
  }
  return body;
}

auto World2D::destroy_body(b2BodyId body) noexcept -> void {
  if (B2_IS_NULL(world_) || B2_IS_NULL(body)) {
    return;
  }
  if (b2Body_IsValid(body)) {
    b2DestroyBody(body);
    if (body_count_ > 0) {
      --body_count_;
    }
  }
}

auto World2D::step(float delta_time, CollisionEventRing* ring) noexcept
    -> std::uint32_t {
  if (B2_IS_NULL(world_) || delta_time <= 0.0F) {
    return 0;
  }
  b2World_Step(world_, delta_time, static_cast<int>(sub_steps_));
  if (ring == nullptr) {
    return 0;
  }
  return drain_events(ring);
}

auto World2D::body_count() const noexcept -> std::uint32_t {
  return B2_IS_NULL(world_) ? 0 : body_count_;
}

auto World2D::body_state(b2BodyId body, float* px, float* py, float* speed,
                         bool* awake) const noexcept -> bool {
  if (B2_IS_NULL(world_) || B2_IS_NULL(body) || px == nullptr ||
      py == nullptr || speed == nullptr || awake == nullptr) {
    return false;
  }
  if (!b2Body_IsValid(body)) {
    return false;
  }
  const b2Vec2 p = b2Body_GetPosition(body);
  const b2Vec2 v = b2Body_GetLinearVelocity(body);
  *px = p.x;
  *py = p.y;
  *speed = std::sqrt(v.x * v.x + v.y * v.y);
  *awake = b2Body_IsAwake(body);
  return true;
}

auto World2D::drain_events(CollisionEventRing* ring) noexcept -> std::uint32_t {
  const b2ContactEvents events = b2World_GetContactEvents(world_);
  std::uint32_t drained = 0;
  for (int i = 0; i < events.beginCount; ++i) {
    const b2ContactBeginTouchEvent& e = events.beginEvents[i];
    const b2BodyId ba = b2Shape_GetBody(e.shapeIdA);
    const b2BodyId bb = b2Shape_GetBody(e.shapeIdB);
    if (B2_IS_NULL(ba) || B2_IS_NULL(bb)) {
      continue;
    }
    CollisionEvent ev{};
    ev.entity_a = reinterpret_cast<std::uint64_t>(  // NOLINT
        b2Body_GetUserData(ba));
    ev.entity_b = reinterpret_cast<std::uint64_t>(  // NOLINT
        b2Body_GetUserData(bb));
    // Contact point: manifold midpoint is in local frames; the event
    // carries body-A origin (the 3D path carries the contact point —
    // the 2D path keeps the same field meaning with the A-body pose,
    // documented in Physics2D.CollisionEvents).
    const b2Vec2 pa = b2Body_GetPosition(ba);
    ev.point_x = pa.x;
    ev.point_y = pa.y;
    ev.point_z = 0.0F;
    ev.kind = CollisionEventKind::kEnter;
    const auto sa = reinterpret_cast<std::uintptr_t>(  // NOLINT
        b2Shape_GetUserData(e.shapeIdA));
    const auto sb = reinterpret_cast<std::uintptr_t>(  // NOLINT
        b2Shape_GetUserData(e.shapeIdB));
    ev.sensor = static_cast<std::uint8_t>((sa | sb) & 1U);
    if (ring->try_push(ev)) {
      ++drained;
    }
  }
  for (int i = 0; i < events.endCount; ++i) {
    const b2ContactEndTouchEvent& e = events.endEvents[i];
    // End events may reference destroyed shapes — resolve via the
    // contact id is unsafe post-destroy; shapes may be gone, so read
    // bodies defensively (null body => entity 0, same convention as
    // the 3D path's kExit records).
    std::uint64_t ea = 0;
    std::uint64_t eb = 0;
    if (b2Shape_IsValid(e.shapeIdA) && b2Shape_IsValid(e.shapeIdB)) {
      const b2BodyId ba = b2Shape_GetBody(e.shapeIdA);
      const b2BodyId bb = b2Shape_GetBody(e.shapeIdB);
      if (!B2_IS_NULL(ba) && !B2_IS_NULL(bb)) {
        ea = reinterpret_cast<std::uint64_t>(  // NOLINT
            b2Body_GetUserData(ba));
        eb = reinterpret_cast<std::uint64_t>(  // NOLINT
            b2Body_GetUserData(bb));
      }
    }
    CollisionEvent ev{};
    ev.entity_a = ea;
    ev.entity_b = eb;
    ev.kind = CollisionEventKind::kExit;
    if (ring->try_push(ev)) {
      ++drained;
    }
  }
  return drained;
}

}  // namespace engine::physics

// character.cpp — P6-04: CharacterBody implementation (CharacterVirtual).
//
// Shape convention (CharacterBaseSettings::mShape doc + Jolt's own test):
// the bottom of the shape sits at local (0,0,0). A bare CapsuleShape is
// centered, so it is wrapped in a RotatedTranslatedShape offset by
// (half_height + radius) — as UnitTests/Physics/CharacterVirtualTests.cpp
// does. Supporting volume accepts contacts touching the lower sphere.
//
// Per-move velocity (Jolt test Step pattern): on ground and not moving
// away, horizontal input replaces XZ while Y keeps gravity integration;
// in air, XZ input adds to the preserved vertical velocity. Gravity
// integrates every move (vy_ -= 9.81 * dt, starting from the post-move
// velocity so falls accelerate). ExtendedUpdate runs Update + WalkStairs
// (step-up = max_step_height) + StickToFloor (step-down 0.5 m) in one
// call. Steep slopes (beyond max_slope_deg) report OnSteepGround —
// grounded() is OnGround only, so the acceptance "stops at 60°" holds.
#include "engine/physics/character.hpp"

// WARNING (load-bearing include rule): this TU is one of the few that may
// textually include <Jolt/...>. Headers arrive via the
// engine_physics_jolt_wrapper interface (SYSTEM + -Wno-conversion), so
// -Wconversion -Werror never fires inside Jolt headers.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

#include <Jolt/Jolt.h>
#include <Jolt/Core/Reference.h>
#include <Jolt/Physics/Body/BodyFilter.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include "engine/physics/physics_world.hpp"
#include "engine/physics/layers.hpp"

namespace engine::physics {

namespace {

// Gravity (m/s^2, Y-down). Jolt's test reads system->GetGravity(); the
// engine world never changes it from the default (0,-9.81,0), so pin the
// constant here rather than reaching into the system each move.
constexpr float kGravityY = -9.81F;
// Default slope limit when the caller passes <= 0 (matches navmesh.hpp's
// 45-degree default convention from the P6-03 review).
constexpr float kDefaultSlopeDeg = 45.0F;
// StickToFloor projection distance (Jolt ExtendedUpdateSettings default:
// (0,-0.5,0) — floor found within 0.5 m keeps contact on small steps).
constexpr float kStickStepDown = 0.5F;
// Degrees -> radians.
constexpr float kDegToRad = 3.14159265358979323846F / 180.0F;

auto deg_to_rad(float deg) noexcept -> float { return deg * kDegToRad; }

auto clamp_slope_deg(float deg) noexcept -> float {
  return deg <= 0.0F ? kDefaultSlopeDeg : deg;
}

auto clamp_step(float s) noexcept -> float { return s < 0.0F ? 0.0F : s; }

auto clamp_skin(float s) noexcept -> float { return s < 0.0F ? 0.0F : s; }

// Fill an output state from the character (feet position, grounded flag,
// ground height, vertical velocity). Ground height falls back to the feet
// Y when the character is unsupported (no contact to report).
auto fill_state(const JPH::CharacterVirtual& ch, float vy,
                CharacterState* out) noexcept -> void {
  const JPH::RVec3 pos = ch.GetPosition();
  out->px = static_cast<float>(pos.GetX());
  out->py = static_cast<float>(pos.GetY());
  out->pz = static_cast<float>(pos.GetZ());
  const bool grounded =
      ch.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
  out->grounded = grounded ? 1U : 0U;
  if (grounded) {
    const JPH::RVec3 gp = ch.GetGroundPosition();
    out->ground_y = static_cast<float>(gp.GetY());
  } else {
    out->ground_y = out->py;
  }
  out->vy = vy;
}

}  // namespace

CharacterBody::~CharacterBody() noexcept { destroy(); }

auto CharacterBody::create(PhysicsWorld* world, const CharacterConfig& cfg,
                           float x, float y, float z) noexcept -> bool {
  if (world == nullptr || !world->valid() || world->native() == nullptr ||
      world->temp() == nullptr || character_ != nullptr) {
    return false;
  }
  if (!(cfg.half_height > 0.0F) || !(cfg.radius > 0.0F)) {
    return false;
  }
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
    return false;
  }

  // Shape: capsule (half_height, radius) offset so its bottom sits at
  // local origin. The STACK capsule is only the settings input: the
  // RotatedTranslatedShapeSettings::Create output (a Ref-held shape) is
  // what the character queries, and CharacterBase keeps it as a
  // RefConst<Shape> — which shares ownership with shape_res, both dying
  // at scope end. Keep our own Ref (shape_) so the shape outlives the
  // create() scope (destroy() releases it with the character).
  // Heap capsule (Jolt's own CharacterVirtualTests pattern): `new
  // CapsuleShape` is refcounted — the RotatedTranslatedShapeSettings ctor
  // takes the bare pointer and the Create() output Ref shares ownership.
  // A STACK capsule dies at scope end, leaving the RT shape's inner
  // pointer dangling (the segfault). Heap + Ref keeps it alive; the
  // character's RefConst<Shape> + our shape_ Ref release it at destroy().
  auto* capsule =
      new JPH::CapsuleShape(cfg.half_height, cfg.radius);  // NOLINT: init-time only
  const JPH::Vec3 offset(0.0F, cfg.half_height + cfg.radius, 0.0F);
  const JPH::RotatedTranslatedShapeSettings offset_shape(
      offset, JPH::Quat::sIdentity(), capsule);
  // ShapeResult must outlive the .Get() borrow: copy the Ref out first,
  // then check (a failed shape must not reach the CharacterVirtual ctor).
  const JPH::Shape::ShapeResult shape_res = offset_shape.Create();
  JPH::Ref<JPH::Shape> shape = shape_res.Get();
  if (shape_res.HasError() || shape == nullptr) {
    return false;
  }

  // Settings on the stack (copied by the ctor): slope limit, padding,
  // supporting volume over the lower sphere (Jolt test convention:
  // plane at -2*half_height accepts contacts touching the capsule's
  // lower half — the test uses -mHeightStanding where height = full
  // cylinder; here full cylinder = 2*half_height).
  JPH::CharacterVirtualSettings settings;
  settings.mShape = shape;
  settings.mMaxSlopeAngle = deg_to_rad(clamp_slope_deg(cfg.max_slope_deg));
  settings.mCharacterPadding = clamp_skin(cfg.skin_width);
  settings.mSupportingVolume = JPH::Plane(
      JPH::Vec3::sAxisY(), -2.0F * cfg.half_height - cfg.radius);

  // CharacterVirtual overrides operator new with no nothrow form and this
  // TU builds -fno-exceptions — malloc + placement new, matching
  // physics_world.cpp's init-time pattern.
  void* mem = std::malloc(sizeof(JPH::CharacterVirtual));
  if (mem == nullptr) {
    return false;
  }
  auto* ch = new (mem) JPH::CharacterVirtual(  // NOLINT: init-time only
      &settings, JPH::RVec3(x, y, z), JPH::Quat::sIdentity(),
      world->native());
  world_ = world;
  character_ = ch;
  // Keep our own Ref to the shape (the header stays Jolt-free: shape_buf_
  // holds a placement-new'd JPH::Ref<JPH::Shape>). CharacterBase shares
  // ownership via its own RefConst<Shape>, so the shape outlives the
  // settings + shape_res scope.
  static_assert(sizeof(shape_buf_) >= sizeof(JPH::Ref<JPH::Shape>),
                "shape_buf_ must fit JPH::Ref<JPH::Shape>");
  new (&shape_buf_) JPH::Ref<JPH::Shape>(shape);  // NOLINT: init-time only
  cfg_ = cfg;
  vy_ = 0.0F;
  contacts_dirty_ = false;
  return true;
}

auto CharacterBody::destroy() noexcept -> void {
  // Destroy the character first, then release our shape Ref (CharacterBase
  // holds its own Ref, so the shape stays alive until both release).
  // destroy() is idempotent, but the placement-new'd Ref exists only after
  // a successful create(): guard with character_ so a failed create or a
  // double destroy never runs the dtor on unconstructed bytes.
  const bool had_character = character_ != nullptr;
  if (had_character) {
    character_->~CharacterVirtual();
    std::free(character_);  // NOLINT: teardown only, matches malloc+placement
    character_ = nullptr;
  }
  if (had_character) {
    auto* ref = reinterpret_cast<JPH::Ref<JPH::Shape>*>(&shape_buf_);  // NOLINT
    ref->~Ref();
  }
  world_ = nullptr;
  vy_ = 0.0F;
  contacts_dirty_ = false;
}

auto CharacterBody::move(float dx, float dz, float dt, CharacterState* out) noexcept
    -> bool {
  if (character_ == nullptr || world_ == nullptr || out == nullptr) {
    return false;
  }
  if (!(dt > 0.0F) || !std::isfinite(dt)) {
    return false;
  }
  if (!std::isfinite(dx) || !std::isfinite(dz)) {
    return false;
  }
  JPH::PhysicsSystem* sys = world_->native();
  JPH::TempAllocator* alloc = world_->temp();
  if (sys == nullptr || alloc == nullptr) {
    return false;
  }

  // A teleport leaves stale contacts: refresh before the first update so
  // the ground state reflects the new pose (Jolt RefreshContacts doc).
  if (contacts_dirty_) {
    character_->RefreshContacts(
        sys->GetDefaultBroadPhaseLayerFilter(
            ObjectLayers::kMoving),
        sys->GetDefaultLayerFilter(ObjectLayers::kMoving),
        JPH::BodyFilter(), JPH::ShapeFilter(), *alloc);
    contacts_dirty_ = false;
  }

  // Velocity (Jolt CharacterVirtualTests Step pattern): on ground and not
  // moving away, horizontal input replaces XZ while Y keeps gravity
  // integration; in air, XZ input adds to the preserved vertical velocity.
  // Gravity integrates every move from the persistent vy_ so falls
  // accelerate, clamped to a game terminal velocity so supported frames
  // never bank fall debt (unbounded vy_ made WalkStairs refuse the
  // 0.3 m riser).
  // Fast walkers tunnel the WalkStairs probes: at 1.5 m/s the frame
  // travel is 2.5 cm but the riser needs a full step-up + forward sweep.
  // Sub-step the ExtendedUpdate so each sub-move travels <= ~1 cm (Jolt's
  // own TestWalkStairs runs 60/120/240/360 Hz for the same reason).
  constexpr float kMaxStepLen = 0.01F;
  const float planar = std::sqrt(dx * dx + dz * dz);
  int sub_steps = 1;
  if (planar > 1.0e-6F) {
    sub_steps = static_cast<int>(std::ceil(planar * dt / kMaxStepLen));
    sub_steps = sub_steps < 1 ? 1 : (sub_steps > 8 ? 8 : sub_steps);
  }
  const float sub_dt = dt / static_cast<float>(sub_steps);
  for (int s = 0; s < sub_steps; ++s) {
    vy_ += kGravityY * sub_dt;
    constexpr float kMaxFallSpeed = 10.0F;
    if (vy_ < -kMaxFallSpeed) {
      vy_ = -kMaxFallSpeed;
    }
    character_->SetLinearVelocity(JPH::Vec3(dx, vy_, dz));

  // ExtendedUpdate = Update + WalkStairs + StickToFloor. Step-up carries
  // the configured max_step_height (0 disables stairs); stick-down is
  // the Jolt default 0.5 m when stick_to_floor is set, zeroed otherwise.
  // The WalkStairs forward probes default to 0.15 m; at 1.5 m/s the
  // per-move travel is 2.5 cm, so extend the forward test past the
  // character's leading radius + padding (0.3 + 0.02) — otherwise the
  // probe starts inside the riser face and the climb is refused. The min
  // step-forward forces a full stride when the frame travel is small.
  JPH::CharacterVirtual::ExtendedUpdateSettings ext;
  ext.mWalkStairsStepUp = JPH::Vec3(0.0F, clamp_step(cfg_.max_step_height), 0.0F);
  // Forward probes are magnitudes along the velocity direction (not
  // vectors): extend the step-forward test past the capsule's leading
  // radius + padding (0.3 + 0.02) plus Jolt's 0.15 m default — otherwise
  // the probe starts inside the riser face and the climb is refused.
  ext.mWalkStairsStepForwardTest =
      cfg_.radius + clamp_skin(cfg_.skin_width) + 0.15F;
  ext.mWalkStairsMinStepForward = 0.05F;
  ext.mStickToFloorStepDown =
      cfg_.stick_to_floor != 0 ? JPH::Vec3(0.0F, -kStickStepDown, 0.0F)
                               : JPH::Vec3::sZero();
    character_->ExtendedUpdate(
        sub_dt, JPH::Vec3(0.0F, kGravityY, 0.0F), ext,
        sys->GetDefaultBroadPhaseLayerFilter(
            ObjectLayers::kMoving),
        sys->GetDefaultLayerFilter(ObjectLayers::kMoving),
        JPH::BodyFilter(), JPH::ShapeFilter(), *alloc);
  }

  // Persist the post-move vertical velocity (slope slides + stair lifts
  // feed back into the next gravity integration), re-clamped so supported
  // frames never bank fall debt. When supported, re-anchor to the ground
  // velocity (Jolt test Step pattern) so standing vy_ stays ~0.
  vy_ = character_->GetLinearVelocity().GetY();
  {
    constexpr float kMaxFallSpeed = 10.0F;
    if (vy_ < -kMaxFallSpeed) {
      vy_ = -kMaxFallSpeed;
    }
  }
  if (character_->GetGroundState() ==
      JPH::CharacterBase::EGroundState::OnGround) {
    const JPH::Vec3 gv = character_->GetGroundVelocity();
    const float rel = vy_ - gv.GetY();
    if (rel < 0.1F && rel > -3.0F) {
      vy_ = gv.GetY();
    }
  }
  fill_state(*character_, vy_, out);
  return true;
}

auto CharacterBody::teleport(float x, float y, float z) noexcept -> bool {
  if (character_ == nullptr) {
    return false;
  }
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
    return false;
  }
  character_->SetPosition(JPH::RVec3(x, y, z));
  vy_ = 0.0F;
  contacts_dirty_ = true;
  return true;
}

auto CharacterBody::set_max_slope_deg(float max_slope_deg) noexcept -> bool {
  if (character_ == nullptr) {
    return false;
  }
  cfg_.max_slope_deg = clamp_slope_deg(max_slope_deg);
  character_->SetMaxSlopeAngle(deg_to_rad(cfg_.max_slope_deg));
  return true;
}

auto CharacterBody::set_max_step_height(float max_step_height) noexcept -> bool {
  if (character_ == nullptr) {
    return false;
  }
  cfg_.max_step_height = clamp_step(max_step_height);
  return true;
}

auto CharacterBody::position(CharacterState* out) const noexcept -> bool {
  if (character_ == nullptr || out == nullptr) {
    return false;
  }
  // position() is const: vy_ is mutable-by-design state, read directly.
  fill_state(*character_, vy_, out);
  return true;
}

}  // namespace engine::physics

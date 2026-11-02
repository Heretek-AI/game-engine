// character.hpp — P6-04: Jolt CharacterVirtual wrapper.
//
// Owns one JPH::CharacterVirtual bound to a PhysicsWorld's PhysicsSystem:
// capsule shape (bottom at local origin, per Jolt's CharacterBaseSettings
// convention — a bare CapsuleShape is centered, so it is wrapped in a
// RotatedTranslatedShape offset, exactly as Jolt's own
// CharacterVirtualTests does), ExtendedUpdate per move(), grounded flag
// from GetGroundState, slope limit via mMaxSlopeAngle, step height via the
// WalkStairs step-up vector.
//
// Lifetime: caller-owned (same model as the M4-01 PhysicsWorld pattern —
// the adapter borrows the PhysicsSystem + the world's temp allocator, both
// must outlive it). create() configures shape + slope from the caller's
// CharacterConfig; move() advances one fixed step. Frame path: no
// malloc/new, noexcept throughout (CharacterVirtual uses
// JPH_OVERRIDE_NEW_DELETE with no nothrow form — malloc + placement new
// at create time only, matching physics_world.cpp).
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"

#include <cstdio>

// WARNING (load-bearing include rule): this header must NOT textually
// include <Jolt/...> (the wrapper interface carries the SYSTEM include +
// -Wno-conversion suppression for adapter TUs only). The shape Ref lives
// behind an opaque byte buffer (placement-new'd in create(), released in
// destroy()) so the header stays Jolt-free.
#include <cstddef>

namespace JPH {
class PhysicsSystem;
class CharacterVirtual;
class TempAllocator;
}  // namespace JPH

namespace engine::physics {

class PhysicsWorld;

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251 4275)  // STL members + Jolt non-dll bases: same-toolchain, OBJECT lib linked into engine_core.
#endif

// Character creation parameters (blittable mirror of the ABI
// EngineCharacterConfig: field order must match engine_abi.h exactly).
struct CharacterConfig {
  float half_height{0.9F};      // cylinder mid-section half height (> 0)
  float radius{0.3F};           // capsule radius (> 0)
  float max_slope_deg{45.0F};   // walkable slope limit, degrees (<= 0 => 45)
  float max_step_height{0.4F};  // WalkStairs step-up, meters (>= 0)
  float skin_width{0.02F};      // character padding, meters (>= 0)
  std::uint8_t stick_to_floor{1};  // 1: project onto floor each move
  std::uint8_t reserved[3]{};  // pad to 24 (matches EngineCharacterConfig)
};

// Per-move state written back to the caller (blittable mirror of the ABI
// EngineCharacterState: position = feet contact point, grounded = 1 when
// GetGroundState() == OnGround, ground Y of the support contact).
struct CharacterState {
  float px{0.0F};
  float py{0.0F};
  float pz{0.0F};
  std::uint32_t grounded{0};  // 1: OnGround, 0: otherwise (steep/air)
  float ground_y{0.0F};
  float vy{0.0F};  // vertical velocity after the move (m/s)
};

class ENGINE_CORE_API CharacterBody {
 public:
  CharacterBody() noexcept = default;
  CharacterBody(const CharacterBody&) = delete;
  auto operator=(const CharacterBody&) -> CharacterBody& = delete;
  ~CharacterBody() noexcept;

  // Bind to the world's PhysicsSystem + temp allocator and build the
  // CharacterVirtual at (x,y,z). The world (system + allocator) must
  // outlive this body. Returns false on null world, unconstructed world,
  // or non-positive half_height/radius.
  auto create(PhysicsWorld* world, const CharacterConfig& cfg, float x,
              float y, float z) noexcept -> bool;
  auto destroy() noexcept -> void;

  [[nodiscard]] auto valid() const noexcept -> bool {
    return character_ != nullptr;
  }

  // Advance one step: desired horizontal velocity (dx,dz in m/s) is
  // combined with gravity-integrated vertical velocity, then
  // ExtendedUpdate runs (Update + WalkStairs with max_step_height +
  // StickToFloor when stick_to_floor). dt must be > 0. Writes *out
  // (feet position, grounded flag, ground height, vertical velocity).
  // Returns false on null/invalid/out null/bad dt.
  auto move(float dx, float dz, float dt, CharacterState* out) noexcept
      -> bool;

  // Teleport the character (SetPosition + contact refresh on next move).
  // Returns false when invalid.
  auto teleport(float x, float y, float z) noexcept -> bool;

  // Live slope-limit update (radians -> cos, same as Jolt's
  // SetMaxSlopeAngle). max_slope_deg <= 0 restores the 45-degree default.
  // Returns false when invalid.
  auto set_max_slope_deg(float max_slope_deg) noexcept -> bool;

  // Live step-height update (WalkStairs step-up vector). Negative
  // clamps to 0 (stairs disabled). Returns false when invalid.
  auto set_max_step_height(float max_step_height) noexcept -> bool;

  // Read the current feet position without stepping. Returns false on
  // null/out or invalid.
  auto position(CharacterState* out) const noexcept -> bool;

 private:
  PhysicsWorld* world_{nullptr};
  JPH::CharacterVirtual* character_{nullptr};
  // CharacterBase holds the shape as a Ref (sharing ownership with the
  // settings' Ref). The settings object dies at the end of create(), so
  // hold our own Ref here to keep the RotatedTranslatedShape alive for
  // the character's lifetime (destroy() releases it with the character).
  // Opaque bytes (Jolt-free header): a placement-new'd JPH::Ref<JPH::Shape>
  // in create(), explicitly destroyed in destroy(). Sized for a Ref
  // (pointer-sized) with pointer alignment.
  alignas(void*) unsigned char shape_buf_[sizeof(void*)];
  CharacterConfig cfg_{};
  float vy_{0.0F};  // persistent vertical velocity (gravity-integrated)
  bool contacts_dirty_{false};  // teleport pending: refresh before update
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

static_assert(sizeof(CharacterConfig) == 24,
              "CharacterConfig must be 24 bytes");
static_assert(sizeof(CharacterState) == 24,
              "CharacterState must be 24 bytes");

}  // namespace engine::physics

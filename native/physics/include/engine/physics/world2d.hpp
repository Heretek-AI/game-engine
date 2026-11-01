// world2d.hpp — P6-07: Box2D v3 world (2D physics kernel).
//
// Owns: b2WorldId + contact-event drain into the shared
// CollisionEventRing (same 32B record as the 3D path, so C# sees one
// event shape). Entity mapping: bodies carry the Flecs entity as
// Box2D body user-data (set at creation); shapes carry the sensor flag
// in shape user-data bit 0 (contact listener reads it at drain time).
//
// Frame path: step(dt) -> b2World_Step + drain_contact_events (no malloc,
// no exceptions, noexcept throughout). Sleeping stays ON (the 10-box
// stack settles only when bodies sleep — the acceptance asserts it).
#pragma once

#include <cstddef>
#include <cstdint>

#include "box2d/box2d.h"

#include "engine/core/engine_core_export.hpp"
#include "engine/physics/collision_events.hpp"
#include "engine/physics/components2d.hpp"

namespace engine::physics {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251)  // b2WorldId + ring pointer (same-toolchain, OBJECT lib).
#endif

struct World2DConfig {
  float gravity_x{0.0F};
  float gravity_y{-10.0F};
  std::uint32_t sub_steps{4};  // b2World_Step sub-step count
};

class ENGINE_CORE_API World2D {
 public:
  World2D() noexcept = default;
  World2D(const World2D&) = delete;
  auto operator=(const World2D&) -> World2D& = delete;
  ~World2D() noexcept;

  // Create the Box2D world (gravity from cfg). Returns false when a
  // world already exists or creation fails. Init-time only.
  auto construct(const World2DConfig& cfg = {}) noexcept -> bool;
  auto destroy() noexcept -> void;

  [[nodiscard]] auto valid() const noexcept -> bool {
    return !B2_IS_NULL(world_);
  }
  [[nodiscard]] auto native() noexcept -> b2WorldId { return world_; }

  // Create one body + its collider shape from 2D components.
  // entity: Flecs entity carried as body user-data (event mapping).
  // Returns the Box2D body id (b2_nullBodyId on failure — B2_IS_NULL).
  // Init/setup-time only (call between construct and the first step, or
  // on entity spawn). b2BodyId is an opaque POD handle: hold it by
  // value, never serialize it (store/load round-trips are lossy).
  auto create_body(std::uint64_t entity, float px, float py, float angle,
                   const Rigidbody2D& rb, const BoxCollider2D& box) noexcept
      -> b2BodyId;
  auto create_body(std::uint64_t entity, float px, float py, float angle,
                   const Rigidbody2D& rb,
                   const CircleCollider2D& circle) noexcept -> b2BodyId;
  auto create_body(std::uint64_t entity, float px, float py, float angle,
                   const Rigidbody2D& rb,
                   const PolygonCollider2D& poly) noexcept -> b2BodyId;
  auto destroy_body(b2BodyId body) noexcept -> void;

  // Pose/velocity query for tests and the Flecs sync side: position,
  // speed (|v|), and awake flag. False when the id is invalid.
  auto body_state(b2BodyId body, float* px, float* py, float* speed,
                  bool* awake) const noexcept -> bool;

  // Fixed-step update: b2World_Step + drain of begin/end contact events
  // into `ring` (null ring => step only, events discarded). Returns
  // events drained (0 when ring is null).
  auto step(float delta_time, CollisionEventRing* ring) noexcept
      -> std::uint32_t;

  [[nodiscard]] auto body_count() const noexcept -> std::uint32_t;

 private:
  // Drain b2World_GetContactEvents into the shared ring. Both shapes of
  // each pair opt in at creation (enableContactEvents), so the returned
  // arrays need no filtering — only the entity mapping (body user-data)
  // and the sensor bit (shape user-data bit 0) are resolved here.
  auto drain_events(CollisionEventRing* ring) noexcept -> std::uint32_t;

  b2WorldId world_{b2_nullWorldId};
  std::uint32_t sub_steps_{4};
  std::uint32_t body_count_{0};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace engine::physics

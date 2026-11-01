// terrain_heightfield_collider.hpp — P6-01: Jolt HeightFieldShape collider.
//
// Builds a STATIC Jolt body from a HeightmapGrid: samples = grid heights
// (already world heights), shape offset = 0, scale = (spacing, 1, spacing),
// body positioned at (origin_x, 0, origin_z). `block_size` and `bits_per_sample`
// default to the spike-verified values (2 / 8 — full float precision at
// 9x9 classification; 8 bits per 2x2 block keeps the ±0.01 gate with
// margin, see test AC4).
//
// The shape outlives the body via Jolt Ref; `destroy()` removes + destroys
// the body. Raycasts see the body as soon as it is added;
// `OptimizeBroadPhase()` after a batch of static bodies is an optional
// broadphase-quality optimization (§4.14.1). Height samples are copied into
// the shape at create time — the grid may be freed after create().
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"

namespace JPH {
class Shape;
}

namespace engine::physics {
class PhysicsWorld;
}

namespace engine::terrain {

struct HeightmapGrid;

struct HeightfieldColliderConfig {
  float origin_x{0.0F};   // world X of sample (0,0)
  float origin_z{0.0F};   // world Z of sample (0,0)
  std::uint32_t block_size{2};      // Jolt block size [2,8]
  std::uint32_t bits_per_sample{8};  // Jolt compression bits [1,16]
  std::uint64_t user_data{0};  // Body::SetUserData (Flecs entity)
};

class ENGINE_CORE_API HeightfieldCollider {
 public:
  HeightfieldCollider() noexcept = default;
  HeightfieldCollider(const HeightfieldCollider&) = delete;
  auto operator=(const HeightfieldCollider&) -> HeightfieldCollider& = delete;
  ~HeightfieldCollider() noexcept { destroy(); }

  // Create the heightfield shape + static body in `world`. Borrows world;
  // the grid is copied into the shape. Returns false on bad args.
  [[nodiscard]] auto create(engine::physics::PhysicsWorld& world,
                            const HeightmapGrid& grid,
                            const HeightfieldColliderConfig& cfg) noexcept
      -> bool;

  // Remove the body from the world (no-op when not created).
  auto destroy() noexcept -> void;

  [[nodiscard]] auto valid() const noexcept -> bool { return body_id_ != 0xFFFFFFFFu; }
  [[nodiscard]] auto body_id() const noexcept -> std::uint32_t {
    return body_id_;
  }

 private:
  engine::physics::PhysicsWorld* world_{nullptr};
  const JPH::Shape* shape_{nullptr};  // Ref-held (Create() adds a ref)
  std::uint32_t body_id_{0xFFFFFFFFu};
};

}  // namespace engine::terrain

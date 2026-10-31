// components2d.hpp — P6-07: native ECS 2D physics components (Box2D v3).
//
// Blittable, 64-byte aligned (Flecs column rule — matches RigidBody et al).
// Entity mapping follows the 3D path: bodies carry the Flecs entity as
// Box2D body user-data (set at creation, read back at event drain).
#pragma once

#include <cstddef>
#include <cstdint>

namespace engine::physics {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)  // alignas(64) padding by design.
#endif

enum class Body2DType : std::uint8_t { kStatic = 0, kKinematic = 1, kDynamic = 2 };

// Rigidbody2D: one dynamic/static/kinematic Box2D body per entity.
struct alignas(64) Rigidbody2D {
  float gravity_scale{1.0f};     // Box2D gravity scale multiplier
  float linear_damping{0.0f};    // velocity decay /s (Box2D default 0)
  float angular_damping{0.0f};   // angular velocity decay /s (Box2D default 0)
  bool fixed_rotation{false};    // lock rotation (stack stability)
  bool bullet{false};            // continuous collision (fast bodies)
  bool allow_sleeping{true};     // tentative sleep (Box2D sleeping on)
  bool sensor{false};            // trigger volume: reports overlap, no response
  bool active{true};
  Body2DType type{Body2DType::kDynamic};
  // 12 + 5 + 1 = 18 used; pad to 64.
  std::uint8_t reserved[46];
};

// BoxCollider2D: axis-aligned box in local space (half extents).
struct alignas(64) BoxCollider2D {
  float hx{0.5f}, hy{0.5f};  // half extents (meters)
  float density{1.0f};       // kg/m^2 (mass override when > 0)
  float friction{0.6f};      // surface friction (b2SurfaceMaterial)
  float restitution{0.0f};   // bounciness [0,1]
  bool sensor{false};        // trigger volume: reports overlap, no response
  // 21 used; pad to 64.
  std::uint8_t reserved[43];
};

// CircleCollider2D: circle in local space (center + radius).
struct alignas(64) CircleCollider2D {
  float cx{0.0f}, cy{0.0f};  // center in body-local space (meters)
  float radius{0.5f};
  float density{1.0f};
  float friction{0.6f};
  float restitution{0.0f};
  bool sensor{false};
  // 25 used; pad to 64.
  std::uint8_t reserved[39];
};

// PolygonCollider2D: convex polygon in local space, up to 6 vertices
// (Box2D supports 8; two slots trade for density/friction/count/sensor
// inside the 64B budget).
struct alignas(64) PolygonCollider2D {
  float vx[6]{};  // vertex x, body-local meters (counter-clockwise, convex)
  float vy[6]{};  // vertex y
  float density{1.0f};
  float friction{0.6f};
  std::uint8_t count{0};  // active vertices [0..6] (0 = no shape)
  std::uint8_t sensor{0};  // trigger volume: reports overlap, no response
  // 48 + 8 + 2 = 58 used; pad to 64.
  std::uint8_t reserved[6];
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

static_assert(sizeof(Rigidbody2D) == 64, "Rigidbody2D must be one cache line");
static_assert(sizeof(BoxCollider2D) == 64,
              "BoxCollider2D must be one cache line");
static_assert(sizeof(CircleCollider2D) == 64,
              "CircleCollider2D must be one cache line");
static_assert(sizeof(PolygonCollider2D) == 64,
              "PolygonCollider2D must be one cache line");
static_assert(alignof(Rigidbody2D) == 64, "Rigidbody2D alignas(64)");
static_assert(alignof(BoxCollider2D) == 64, "BoxCollider2D alignas(64)");
static_assert(alignof(CircleCollider2D) == 64, "CircleCollider2D alignas(64)");
static_assert(alignof(PolygonCollider2D) == 64, "PolygonCollider2D alignas(64)");

}  // namespace engine::physics

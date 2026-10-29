// components.hpp — M4-01: native ECS physics components.
//
// Blittable, 64-byte aligned (Flecs column rule): RigidBody, BoxCollider,
// SphereCollider, CapsuleCollider, MeshCollider, CharacterController.
#pragma once

#include <cstddef>
#include <cstdint>


namespace engine::physics {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)  // alignas(64) padding by design.
#endif

enum class BodyType : std::uint8_t { kStatic = 0, kKinematic = 1, kDynamic = 2 };
enum class MotionQuality : std::uint8_t { kDiscrete = 0, kLinearCast = 1 };

struct alignas(64) RigidBody {
  float mass{1.0f};              // kg; 0 => static
  float friction{0.5f};          // Coulomb friction coefficient
  float restitution{0.0f};       // bounciness [0,1]
  float linear_damping{0.05f};   // velocity decay /s
  float angular_damping{0.05f};  // angular velocity decay /s
  float gravity_factor{1.0f};    // gravity scale
  BodyType type{BodyType::kDynamic};
  MotionQuality motion_quality{MotionQuality::kDiscrete};
  bool allow_sleeping{true};
  bool sensor{false};  // trigger volume: reports overlap, no response
  bool active{true};
  // 24 + 5 = 29 used; pad to 64.
  std::uint8_t reserved[35];
};

struct alignas(64) BoxCollider {
  float hx{0.5f}, hy{0.5f}, hz{0.5f};  // half extents
  float density{1000.0f};              // kg/m^3 (mass override when > 0)
  bool sensor{false};
  // 17 used; pad to 64.
  std::uint8_t reserved[47];
};

struct alignas(64) SphereCollider {
  float radius{0.5f};
  float density{1000.0f};
  bool sensor{false};
  // 9 used; pad to 64.
  std::uint8_t reserved[55];
};

struct alignas(64) CapsuleCollider {
  float half_height{0.5f};  // cylinder mid-section half height
  float radius{0.3f};
  float density{1000.0f};
  bool sensor{false};
  // 13 used; pad to 64.
  std::uint8_t reserved[51];
};

struct alignas(64) MeshCollider {
  std::uint64_t vertex_data{0};  // opaque handle to cooked mesh blob
  std::uint64_t vertex_count{0};
  std::uint64_t index_data{0};  // opaque handle to index blob
  std::uint64_t index_count{0};
  float density{1000.0f};
  bool sensor{false};
  // 37 used; pad to 64.
  std::uint8_t reserved[27];
};

struct alignas(64) CharacterController {
  float half_height{0.9f};
  float radius{0.3f};
  float max_slope_angle{0.7853982f};  // 45 deg
  float max_step_height{0.4f};
  float skin_width{0.02f};
  bool stick_to_floor{true};
  bool sensor{false};
  // 22 used; pad to 64.
  std::uint8_t reserved[42];
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

static_assert(sizeof(RigidBody) == 64, "RigidBody must be one cache line");
static_assert(sizeof(BoxCollider) == 64, "BoxCollider must be one cache line");
static_assert(sizeof(SphereCollider) == 64,
              "SphereCollider must be one cache line");
static_assert(sizeof(CapsuleCollider) == 64,
              "CapsuleCollider must be one cache line");
static_assert(sizeof(MeshCollider) == 64,
              "MeshCollider must be one cache line");
static_assert(sizeof(CharacterController) == 64,
              "CharacterController must be one cache line");
static_assert(alignof(RigidBody) == 64, "RigidBody alignas(64)");
static_assert(alignof(BoxCollider) == 64, "BoxCollider alignas(64)");
static_assert(alignof(SphereCollider) == 64, "SphereCollider alignas(64)");
static_assert(alignof(CapsuleCollider) == 64, "CapsuleCollider alignas(64)");
static_assert(alignof(MeshCollider) == 64, "MeshCollider alignas(64)");
static_assert(alignof(CharacterController) == 64,
              "CharacterController alignas(64)");

}  // namespace engine::physics

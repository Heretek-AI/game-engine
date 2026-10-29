// layers.hpp — M4-01: Jolt object/broadphase layers + filters.
// Object layers: NON_MOVING (static), MOVING (dynamic), TRIGGER, DEBRIS.
// Broadphase layers: STATIC, DYNAMIC, TRIGGER, DEBRIS (one-to-one).
#pragma once

#include "engine/core/engine_core_export.hpp"

#include <cstdint>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>


namespace engine::physics {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251 4275)  // STL members + Jolt non-dll bases: same-toolchain, OBJECT lib linked into engine_core.
#endif

namespace ObjectLayers {
inline constexpr JPH::ObjectLayer kNonMoving = 0;
inline constexpr JPH::ObjectLayer kMoving = 1;
inline constexpr JPH::ObjectLayer kTrigger = 2;
inline constexpr JPH::ObjectLayer kDebris = 3;
inline constexpr JPH::ObjectLayer kCount = 4;
}  // namespace ObjectLayers

namespace BroadPhaseLayers {
inline constexpr JPH::BroadPhaseLayer kStatic{0};
inline constexpr JPH::BroadPhaseLayer kDynamic{1};
inline constexpr JPH::BroadPhaseLayer kTrigger{2};
inline constexpr JPH::BroadPhaseLayer kDebris{3};
inline constexpr JPH::uint kCount = 4;
}  // namespace BroadPhaseLayers

// Maps each object layer to its broadphase layer (one-to-one).
class ENGINE_CORE_API BPLayerInterfaceImpl final
    : public JPH::BroadPhaseLayerInterface {
 public:
  BPLayerInterfaceImpl() noexcept = default;
  auto GetNumBroadPhaseLayers() const -> JPH::uint override {
    return BroadPhaseLayers::kCount;
  }
  auto GetBroadPhaseLayer(JPH::ObjectLayer inLayer) const
      -> JPH::BroadPhaseLayer override {
    switch (inLayer) {
      case ObjectLayers::kNonMoving:
        return BroadPhaseLayers::kStatic;
      case ObjectLayers::kMoving:
        return BroadPhaseLayers::kDynamic;
      case ObjectLayers::kTrigger:
        return BroadPhaseLayers::kTrigger;
      case ObjectLayers::kDebris:
        return BroadPhaseLayers::kDebris;
      default:
        return BroadPhaseLayers::kStatic;
    }
  }
};

// Which object layer collides with which broadphase layer.
class ObjectVsBroadPhaseLayerFilterImpl
    : public JPH::ObjectVsBroadPhaseLayerFilter {
 public:
  ObjectVsBroadPhaseLayerFilterImpl() noexcept = default;
  auto ShouldCollide(JPH::ObjectLayer inLayer1,
                     JPH::BroadPhaseLayer inLayer2) const -> bool override {
    switch (inLayer1) {
      case ObjectLayers::kNonMoving:
        return inLayer2 == BroadPhaseLayers::kDynamic ||
               inLayer2 == BroadPhaseLayers::kDebris;
      case ObjectLayers::kMoving:
        return true;  // Moving hits everything (static/dynamic/trigger/debris).
      case ObjectLayers::kTrigger:
        return inLayer2 == BroadPhaseLayers::kDynamic;
      case ObjectLayers::kDebris:
        return inLayer2 == BroadPhaseLayers::kStatic ||
               inLayer2 == BroadPhaseLayers::kDynamic;
      default:
        return false;
    }
  }
};

// Which pairs of object layers collide.
class ObjectLayerPairFilterImpl
    : public JPH::ObjectLayerPairFilter {
 public:
  ObjectLayerPairFilterImpl() noexcept = default;
  auto ShouldCollide(JPH::ObjectLayer inObject1,
                     JPH::ObjectLayer inObject2) const -> bool override {
    switch (inObject1) {
      case ObjectLayers::kNonMoving:
        return inObject2 == ObjectLayers::kMoving ||
               inObject2 == ObjectLayers::kDebris;
      case ObjectLayers::kMoving:
        return true;
      case ObjectLayers::kTrigger:
        return inObject2 == ObjectLayers::kMoving;
      case ObjectLayers::kDebris:
        return inObject2 == ObjectLayers::kNonMoving ||
               inObject2 == ObjectLayers::kMoving;
      default:
        return false;
    }
  }
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace engine::physics

// terrain_heightfield_collider.cpp — P6-01: static Jolt body from a grid.
//
// WARNING (load-bearing include rule, same as the physics TUs): this is
// the ONLY terrain TU that textually includes <Jolt/...>. It arrives via
// the engine_physics_jolt_wrapper interface (SYSTEM + -Wno-conversion),
// so -Wconversion -Werror never fires inside Jolt headers.
#include "engine/terrain/terrain_heightfield_collider.hpp"

#include <vector>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/HeightFieldShape.h>
#include <Jolt/Physics/EActivation.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include "engine/physics/layers.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/terrain/terrain_heightmap.hpp"

namespace engine::terrain {

auto HeightfieldCollider::create(engine::physics::PhysicsWorld& world,
                                 const HeightmapGrid& grid,
                                 const HeightfieldColliderConfig& cfg) noexcept
    -> bool {
  destroy();
  if (!world.valid() || world.native() == nullptr ||
      grid.samples == nullptr || grid.width < 2 || grid.height < 2 ||
      grid.width != grid.height || grid.spacing <= 0.0F) {
    return false;
  }
  // Jolt HeightFieldShape requires a square grid; sampleCount/blockSize
  // must be >= 2 (spec §4.12). Clamp config into the valid ranges.
  std::uint32_t block = cfg.block_size < 2 ? 2 : cfg.block_size;
  block = block > 8 ? 8 : block;
  std::uint32_t bits = cfg.bits_per_sample < 1 ? 1 : cfg.bits_per_sample;
  bits = bits > 16 ? 16 : bits;
  if (grid.width / block < 2) {
    return false;
  }
  const std::size_t n =
      static_cast<std::size_t>(grid.width) * grid.height;
  std::vector<float> samples(n);
  for (std::size_t i = 0; i < n; ++i) {
    samples[i] = grid.samples[i];
  }
  const JPH::Vec3 offset(cfg.origin_x, grid.min_height, cfg.origin_z);
  const float range = grid.max_height - grid.min_height;
  const JPH::Vec3 scale(grid.spacing, range > 0.0F ? 1.0F : 1.0F,
                        grid.spacing);
  // NOTE: HeightFieldShape surface = offset + scale * (x, samples[y*N+x],
  // y). Our grid already stores WORLD heights, so Y scale stays 1 and the
  // shape compresses the absolute values (8 bits per 2x2 block keeps the
  // ±0.01 gate with margin at this terrain size — proven in the test).
  JPH::HeightFieldShapeSettings hs(samples.data(), offset, scale,
                                   grid.width);
  hs.mBlockSize = block;
  hs.mBitsPerSample = bits;
  JPH::Shape::ShapeResult res = hs.Create();
  if (res.HasError() || res.Get() == nullptr) {
    return false;
  }
  const JPH::Shape* shape = res.Get();
  JPH::BodyCreationSettings bcs(shape, JPH::RVec3(offset.GetX(), 0.0F,
                                                  offset.GetZ()),
                                JPH::Quat::sIdentity(),
                                JPH::EMotionType::Static,
                                engine::physics::ObjectLayers::kNonMoving);
  bcs.mUserData = cfg.user_data;
  bcs.mAllowSleeping = false;
  bcs.mIsSensor = false;
  const JPH::BodyID bid =
      world.native()->GetBodyInterface().CreateAndAddBody(
          bcs, JPH::EActivation::DontActivate);
  if (bid.IsInvalid()) {
    return false;
  }
  // The body holds a Ref through creation; keep our own so destroy() can
  // still release deterministically (AddRef/Release are thread-safe).
  shape->AddRef();
  shape_ = shape;
  world_ = &world;
  body_id_ = bid.GetIndexAndSequenceNumber();
  return true;
}

auto HeightfieldCollider::destroy() noexcept -> void {
  if (world_ != nullptr && world_->valid() && world_->native() != nullptr &&
      body_id_ != 0xFFFFFFFFu) {
    JPH::BodyInterface& iface = world_->native()->GetBodyInterface();
    JPH::BodyID bid(body_id_);
    iface.RemoveBody(bid);
    iface.DestroyBody(bid);
  }
  if (shape_ != nullptr) {
    shape_->Release();
    shape_ = nullptr;
  }
  world_ = nullptr;
  body_id_ = 0xFFFFFFFFu;
}

}  // namespace engine::terrain

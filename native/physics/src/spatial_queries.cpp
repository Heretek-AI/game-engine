// spatial_queries.cpp — M4-03: Jolt narrow-phase queries + enkiTS batch fan.
#include "engine/physics/spatial_queries.hpp"

// WARNING (load-bearing include rule): this TU + job_system_enki.cpp +
// physics_world.cpp + temp_allocator_arena.cpp are the ONLY TUs that may
// textually include <Jolt/...>. They arrive via the
// engine_physics_jolt_wrapper interface (SYSTEM + -Wno-conversion), so
// -Wconversion -Werror never fires inside Jolt headers.
#include <cstddef>
#include <cstdint>

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollector.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include "TaskScheduler_c.h"

namespace engine::physics {

namespace {

// Fill a miss (out must be non-null).
auto fill_miss(SpatialHit* out) noexcept -> void {
  out->px = 0.0F;
  out->py = 0.0F;
  out->pz = 0.0F;
  out->fraction = 1.0F;
  out->user_data = 0;
  out->nx = 0.0F;
  out->ny = 1.0F;
  out->nz = 0.0F;
  out->body_id = 0xFFFFFFFFu;
  out->hit = 0;
}

// Fill a hit from a ray result + locked body.
auto fill_hit(const JPH::Body* body, const JPH::RRayCast& ray, float fraction,
              const JPH::SubShapeID& sub_shape, SpatialHit* out) noexcept
    -> void {
  const JPH::RVec3 point = ray.GetPointOnRay(fraction);
  const JPH::Vec3 normal = body->GetWorldSpaceSurfaceNormal(sub_shape, point);
  out->px = point.GetX();
  out->py = point.GetY();
  out->pz = point.GetZ();
  out->fraction = fraction;
  out->user_data = body->GetUserData();
  out->nx = normal.GetX();
  out->ny = normal.GetY();
  out->nz = normal.GetZ();
  out->body_id = body->GetID().GetIndexAndSequenceNumber();
  out->hit = 1;
}

// Collect-every-body for OverlapSphere (shape-vs-shape path): records the
// touched body of each contact.
struct ShapeOverlapCollector final : public JPH::CollideShapeCollector {
  explicit ShapeOverlapCollector(OverlapOut* o) : out(o) {}

  OverlapOut* out{nullptr};
  std::uint64_t pending_user_data{0};  // NOLINT

  auto AddHit(const JPH::CollideShapeResult& inResult) -> void override {
    if (out == nullptr) {
      ForceEarlyOut();
      return;
    }
    // One body can produce several contacts (compound/mesh): dedupe so a
    // single body never consumes multiple slots (linear scan is fine at
    // typical overlap capacities).
    const std::uint32_t id = inResult.mBodyID2.GetIndexAndSequenceNumber();
    for (std::uint32_t i = 0; i < out->count; ++i) {
      if (out->body_ids[i] == id) {
        return;
      }
    }
    if (out->count >= out->capacity) {
      ForceEarlyOut();
      return;
    }
    const std::uint32_t slot = out->count++;
    out->body_ids[slot] = id;
    if (out->user_data != nullptr) {
      out->user_data[slot] = pending_user_data;
    }
  }

  auto OnBody(const JPH::Body& inBody) -> void override {
    pending_user_data = inBody.GetUserData();
  }
};

// Legacy point-collector adapter (kept for NarrowPhaseQuery::CollidePoint
// callers; OverlapSphere uses the shape path above).
struct OverlapCollector final : public JPH::CollidePointCollector {
  OverlapOut* out{nullptr};
  std::uint64_t pending_user_data{0};  // NOLINT

  auto AddHit(const JPH::CollidePointResult& inResult) -> void override {
    if (out == nullptr || out->count >= out->capacity) {
      ForceEarlyOut();
      return;
    }
    const std::uint32_t slot = out->count++;
    out->body_ids[slot] = inResult.mBodyID.GetIndexAndSequenceNumber();
    if (out->user_data != nullptr) {
      // UserData needs the body under lock: NarrowPhaseQuery calls
      // OnBody(body) under the body lock before AddHit, so cache it there.
      out->user_data[slot] = pending_user_data;
    }
  }

  auto OnBody(const JPH::Body& inBody) -> void override {
    pending_user_data = inBody.GetUserData();
  }
};

}  // namespace

auto spatial_raycast(const JPH::PhysicsSystem& system, const RayInput* ray,
                     SpatialHit* out) noexcept -> int32_t {
  if (ray == nullptr || out == nullptr) {
    return 0;
  }
  const JPH::RRayCast jray(JPH::RVec3(ray->ox, ray->oy, ray->oz),
                           JPH::Vec3(ray->dx, ray->dy, ray->dz));
  JPH::RayCastResult hit;
  const bool found = system.GetNarrowPhaseQuery().CastRay(jray, hit);
  if (!found) {
    fill_miss(out);
    return 0;
  }
  // Resolve body under a shared read lock (locking query already holds
  // per-body locks during the cast, but the result outlives it — re-lock).
  const JPH::BodyLockRead lock(system.GetBodyLockInterface(), hit.mBodyID);
  if (!lock.Succeeded()) {
    fill_miss(out);
    return 0;
  }
  fill_hit(&lock.GetBody(), jray, hit.mFraction, hit.mSubShapeID2, out);
  return 1;
}

auto spatial_sphere_cast(const JPH::PhysicsSystem& system, const RayInput* ray,
                         float radius, SpatialHit* out) noexcept -> int32_t {
  if (ray == nullptr || out == nullptr || radius <= 0.0F) {
    return 0;
  }
  // Stack sphere, marked embedded so a transient Ref cannot delete it.
  // No heap, no reuse hazard (each cast builds its own shape).
  const JPH::SphereShape sphere(radius);
  sphere.SetEmbedded();
  const JPH::RMat44 center =
      JPH::RMat44::sTranslation(JPH::RVec3(ray->ox, ray->oy, ray->oz));
  const JPH::RShapeCast cast(&sphere, JPH::Vec3(1.0F, 1.0F, 1.0F), center,
                             JPH::Vec3(ray->dx, ray->dy, ray->dz));
  const JPH::ShapeCastSettings settings;
  JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> collector;
  system.GetNarrowPhaseQuery().CastShape(cast, settings, JPH::RVec3::sZero(),
                                         collector);
  if (!collector.HadHit()) {
    fill_miss(out);
    return 0;
  }
  const JPH::BodyLockRead lock(system.GetBodyLockInterface(),
                               collector.mHit.mBodyID2);
  if (!lock.Succeeded()) {
    fill_miss(out);
    return 0;
  }
  const JPH::Body* body = &lock.GetBody();
  const float fraction = collector.mHit.mFraction;
  const JPH::Vec3 cp = collector.mHit.mContactPointOn2;
  const JPH::Vec3 normal = body->GetWorldSpaceSurfaceNormal(
      collector.mHit.mSubShapeID2, JPH::RVec3(cp));
  out->px = cp.GetX();
  out->py = cp.GetY();
  out->pz = cp.GetZ();
  out->fraction = fraction;
  out->user_data = body->GetUserData();
  out->nx = normal.GetX();
  out->ny = normal.GetY();
  out->nz = normal.GetZ();
  out->body_id = body->GetID().GetIndexAndSequenceNumber();
  out->hit = 1;
  return 1;
}

auto spatial_overlap_sphere(const JPH::PhysicsSystem& system, float cx,
                            float cy, float cz, float radius,
                            OverlapOut* out) noexcept -> std::uint32_t {
  if (out == nullptr || out->body_ids == nullptr || out->capacity == 0 ||
      radius <= 0.0F) {
    return 0;
  }
  out->count = 0;
  // Overlap: CollideShape with the sphere against the world (shape-vs-shape
  // uses a CollideShapeCollector). Point-query path would miss bodies fully
  // containing the sphere.
  const JPH::SphereShape sphere(radius);
  const JPH::CollideShapeSettings settings;
  const JPH::RMat44 center = JPH::RMat44::sTranslation(JPH::RVec3(cx, cy, cz));
  // Base offset at the sphere center keeps contact math near-origin.
  ShapeOverlapCollector shape_collector(out);
  system.GetNarrowPhaseQuery().CollideShape(
      &sphere, JPH::Vec3(1.0F, 1.0F, 1.0F), center, settings,
      JPH::RVec3(cx, cy, cz), shape_collector);
  return out->count;
}

namespace {

// Batch lane: disjoint [begin,end) into shared rays/hits via base offsets.
struct BatchLane {
  const JPH::PhysicsSystem* system{nullptr};
  const RayInput* rays{nullptr};
  SpatialHit* hits{nullptr};
  std::uint32_t count{0};
};

auto run_batch_range(std::uint32_t start, std::uint32_t end,
                     std::uint32_t /*thread*/, void* args) noexcept -> void {
  const auto* lane = static_cast<const BatchLane*>(args);
  for (std::uint32_t i = start; i < end && i < lane->count; ++i) {
    spatial_raycast(*lane->system, &lane->rays[i], &lane->hits[i]);
  }
}

}  // namespace

auto spatial_raycast_batch(const JPH::PhysicsSystem& system,
                           enkiTaskScheduler* sched,
                           std::uint32_t worker_count, const RayInput* rays,
                           SpatialHit* hits,
                           std::uint32_t count) noexcept -> std::uint32_t {
  if (rays == nullptr || hits == nullptr || count == 0) {
    return 0;
  }
  // Serial fallback: no scheduler, single worker, or trivial counts.
  if (sched == nullptr || worker_count == 0 || count < 2) {
    BatchLane lane{&system, rays, hits, count};
    run_batch_range(0, count, 0, &lane);
    return count;
  }
  std::uint32_t lanes = worker_count + 1;  // + calling thread
  if (lanes > count) {
    lanes = count;
  }
  constexpr std::uint32_t kMinPerLane = 64;  // grain: amortize task overhead
  while (lanes > 1 && count / lanes < kMinPerLane) {
    --lanes;
  }
  if (lanes <= 1) {
    BatchLane lane{&system, rays, hits, count};
    run_batch_range(0, count, 0, &lane);
    return count;
  }
  // One TaskSet per worker lane; lane 0 runs inline on the caller.
  // Lanes <= worker_count+1 <= 257 (scheduler caps at 256 workers);
  // clamp defensively to 64 sets max.
  constexpr std::uint32_t kMaxSets = 64;
  if (lanes > kMaxSets) {
    lanes = kMaxSets;
  }
  enkiTaskSet* sets[kMaxSets]{};
  BatchLane view[kMaxSets];
  const std::uint32_t per_lane = count / lanes;
  const std::uint32_t rem = count % lanes;
  std::uint32_t begin = 0;
  for (std::uint32_t l = 0; l < lanes; ++l) {
    const std::uint32_t take = per_lane + (l < rem ? 1 : 0);
    view[l].system = &system;
    view[l].rays = rays + begin;
    view[l].hits = hits + begin;
    view[l].count = take;
    begin += take;
  }
  std::uint32_t launched = 0;
  for (std::uint32_t l = 1; l < lanes; ++l) {
    sets[l] = enkiCreateTaskSet(sched, run_batch_range);
    if (sets[l] == nullptr) {
      break;
    }
    enkiSetArgsTaskSet(sets[l], &view[l]);
    enkiAddTaskSetMinRange(sched, sets[l], &view[l], view[l].count, 1);
    launched = l;
  }
  if (launched + 1 < lanes) {
    // Allocation failure partway: task lanes [1, launched] run as tasks;
    // run lane 0 + unlaunched lanes [launched+1, lanes) inline.
    run_batch_range(0, view[0].count, 0, &view[0]);
    for (std::uint32_t l = launched + 1; l < lanes; ++l) {
      run_batch_range(0, view[l].count, 0, &view[l]);
    }
    for (std::uint32_t l = 1; l <= launched; ++l) {
      enkiWaitForTaskSet(sched, sets[l]);
      enkiDeleteTaskSet(sched, sets[l]);
    }
    return count;
  }
  // Lane 0 inline on the calling thread.
  run_batch_range(0, view[0].count, 0, &view[0]);
  for (std::uint32_t l = 1; l < lanes; ++l) {
    enkiWaitForTaskSet(sched, sets[l]);
    enkiDeleteTaskSet(sched, sets[l]);
  }
  return count;
}

}  // namespace engine::physics

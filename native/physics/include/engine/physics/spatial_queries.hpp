// spatial_queries.hpp — M4-03: zero-cost C# spatial queries over Jolt.
//
// Wraps JPH::NarrowPhaseQuery (locking variant) for the four managed query
// shapes: single RayCast, SphereCast, OverlapSphere, and the parallel
// RayCastBatch fan-out. Frame path: no malloc, no exceptions, noexcept.
//
// Threading: the locking NarrowPhaseQuery takes per-body shared locks
// internally (required with the shared enkiTS pool, where the waiting
// thread also pumps tasks). The batch entry shards ray ranges over enkiTS
// TaskSets — one TaskSet per worker lane, each writing disjoint hit slots,
// then a single WaitForAll join (wait-only; enkiTS owns set lifetimes).
// The ABI bridge (engine_abi.cpp) binds the batch entry to the owning
// PhysicsWorld's job system + worker count, so managed RaycastBatch fans
// out over the same shared pool (lane 0 inline on the caller).
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"

namespace JPH {
class PhysicsSystem;
}  // namespace JPH

struct enkiTaskScheduler;

namespace engine::physics {

// One ray: origin + direction*length packed as 6 floats (24 bytes).
struct RayInput {
  float ox{0.0F};
  float oy{0.0F};
  float oz{0.0F};
  float dx{0.0F};
  float dy{1.0F};
  float dz{0.0F};
};

// One query hit. 48 bytes, 8-byte aligned — the managed mirror declares the
// same field order (Physics.RaycastHit). body_id is the full Jolt BodyID
// (index+sequence via GetIndexAndSequenceNumber); user_data is the body's
// UserData (Flecs entity, set at body creation); fraction is the
// [0,1] ray parameter (hit = origin + fraction * direction).
struct SpatialHit {
  float px{0.0F};
  float py{0.0F};
  float pz{0.0F};
  float fraction{0.0F};
  std::uint64_t user_data{0};
  float nx{0.0F};
  float ny{1.0F};
  float nz{0.0F};
  std::uint32_t body_id{0xFFFFFFFFu};
  std::uint32_t hit{0};  // 1 = hit, 0 = miss (fraction then 1.0)
};

static_assert(sizeof(SpatialHit) == 48, "SpatialHit must be 48 bytes");
static_assert(alignof(SpatialHit) == 8, "SpatialHit align 8");

// Overlap output: packed body list, written by OverlapSphere.
struct OverlapOut {
  std::uint32_t* body_ids{nullptr};    // full Jolt BodyIDs (index+sequence)
  std::uint64_t* user_data{nullptr};   // per-body UserData (may be null)
  std::uint32_t capacity{0};
  std::uint32_t count{0};
};

// Single raycast against the locking narrow-phase query.
// Returns 1 on hit (fills *out), 0 on miss (out->hit = 0).
auto ENGINE_CORE_API spatial_raycast(const JPH::PhysicsSystem& system,
                                     const RayInput* ray,
                                     SpatialHit* out) noexcept -> int32_t;

// Sphere cast (convex sweep): sphere of `radius` swept from (ox,oy,oz)
// along (dx,dy,dz). Returns 1 on hit, 0 on miss.
auto ENGINE_CORE_API spatial_sphere_cast(const JPH::PhysicsSystem& system,
                                         const RayInput* ray, float radius,
                                         SpatialHit* out) noexcept -> int32_t;

// Sphere overlap: all bodies intersecting the sphere. Writes up to
// out->capacity entries, sets out->count. Returns count written.
auto ENGINE_CORE_API spatial_overlap_sphere(const JPH::PhysicsSystem& system,
                                            float cx, float cy, float cz,
                                            float radius,
                                            OverlapOut* out) noexcept
    -> std::uint32_t;

// Parallel batch raycast: `count` rays -> `count` hits, sharded over
// `worker_count` enkiTS lanes (sched must be running; null/zero =>
// serial fallback). One TaskSet per lane, disjoint [begin,end) ranges,
// single WaitForAll join. Returns rays processed.
auto ENGINE_CORE_API spatial_raycast_batch(const JPH::PhysicsSystem& system,
                                           enkiTaskScheduler* sched,
                                           std::uint32_t worker_count,
                                           const RayInput* rays,
                                           SpatialHit* hits,
                                           std::uint32_t count) noexcept
    -> std::uint32_t;

}  // namespace engine::physics

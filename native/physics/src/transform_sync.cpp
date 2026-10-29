// transform_sync.cpp — M4-02: parallel Flecs <-> Jolt transform sync.
//
// AC1 push: serial Flecs query over (LocalTransform, PhysicsBody);
//   each kinematic/dynamic link calls SetPositionAndRotationWhenChanged
//   (Jolt internally skips broadphase churn when the delta is negligible).
// AC2 pull: snapshot active BodyIDs under lock, then an enkiTS TaskSet
//   fans body-index ranges over workers; each worker reads position/rotation
//   via the locking body interface (per-body shared lock — safe with the
//   shared enkiTS pool) and writes the entity's LocalTransform through
//   ecs_get_mut_id.
// AC3 events: ContactListenerImpl (below) pushes Enter records on callbacks
//   and Exit records on OnContactRemoved. Jolt CACHES nothing for us across
//   the step, so Exit carries zeroed entity ids when bodies are gone;
//   enter/exit pairing by entity is the consumer's job (M4-03).
#include "engine/physics/transform_sync.hpp"

#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

// Jolt headers ONLY here (never from engine headers): this TU links the
// wrapper interface (SYSTEM include, -Wno-conversion), so -Wconversion
// -Werror never fires inside Jolt.
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyManager.h>  // BodyIDVector (scratch snapshot)
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/EActivation.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include "TaskScheduler_c.h"
#include "engine/ecs/transform.hpp"
#include "flecs.h"

namespace engine::physics {

namespace {

// JoltContactForwarder: forwards contact callbacks into the lock-free ring.
// Callbacks run on physics worker threads DURING Update() — the body of
// each override must be wait-free: no malloc, no locks, no Jolt lock
// interface (bodies are already locked by the island solver; the header
// explicitly forbids locking calls here). Body::GetUserData/GetPosition
// are plain field reads, safe under the solver's read sharing.
//
// IMPORTANT (threading contract — verified against the vendored flecs
// sources in deps/flecs/src and the pinned Jolt v5.6.0 headers):
// the pull-phase shard workers below (enkiTS) ONLY touch Jolt state
// (BodyLockRead + GetPosition/GetRotation/GetUserData) and write into a
// private staging buffer. They NEVER call ecs_* or write Flecs columns:
// ecs_get_mut_id resolves entity records through the shared entity index,
// so concurrent calls from a pool with a parallel in-flight step are
// unsupported; the serial drain on the calling thread owns ALL Flecs
// mutation (see pull_from_physics).
class JoltContactForwarder final : public JPH::ContactListener {
 public:
  explicit JoltContactForwarder(CollisionEventRing* ring) noexcept
      : ring_(ring) {}

  void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2,
                      const JPH::ContactManifold& manifold,
                      JPH::ContactSettings& /*settings*/) override {
    if (ring_ == nullptr) {
      return;
    }
    CollisionEvent ev{};
    ev.entity_a = b1.GetUserData();
    ev.entity_b = b2.GetUserData();
    // mBaseOffset is the world-space anchor of the contact patch.
    const JPH::RVec3 p = manifold.mBaseOffset;
    ev.point_x = p.GetX();
    ev.point_y = p.GetY();
    ev.point_z = p.GetZ();
    ev.kind = CollisionEventKind::kEnter;
    ev.sensor =
        static_cast<std::uint8_t>(b1.IsSensor() || b2.IsSensor() ? 1 : 0);
    ring_->try_push(ev);
  }

  void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
    if (ring_ == nullptr) {
      return;
    }
    // Bodies are NOT dereferenceable here (may be destroyed): cache the
    // full BodyIDs (index+sequence) so the consumer can match against the
    // Enter records' entity ids. Full entity ids are unavailable; enter/exit
    // pairing by entity is the M4-03 consumer's job (it holds the
    // body->entity map at drain time).
    CollisionEvent ev{};
    ev.entity_a = pair.GetBody1ID().GetIndexAndSequenceNumber();
    ev.entity_b = pair.GetBody2ID().GetIndexAndSequenceNumber();
    ev.kind = CollisionEventKind::kExit;
    ring_->try_push(ev);
  }

 private:
  CollisionEventRing* ring_{nullptr};
};

// Pull shard: one contiguous range of snapshotted active BodyIDs.
// Staging row: the shard workers fill these (Jolt-side only, no Flecs);
// the serial drain below applies them to LocalTransform columns.
struct PullRow {
  std::uint64_t entity{0};
  float px{0.0F};
  float py{0.0F};
  float pz{0.0F};
  float qx{0.0F};
  float qy{0.0F};
  float qz{0.0F};
  float qw{1.0F};
  std::uint32_t valid{0};
};

struct PullShardArgs {
  JPH::PhysicsSystem* system{nullptr};
  PullRow* rows{nullptr};
  const JPH::BodyID* ids{nullptr};
  std::uint32_t count{0};
  std::atomic<std::uint32_t>* done{nullptr};
};

// enkiTS range task: read each body's transform via the LOCKING body
// interface (per-body shared lock — safe with a shared enkiTS pool where
// the main thread's wait also pumps tasks) and stage (entity, pos, quat)
// into the caller's rows. Entities come from Body::GetUserData (set at
// creation). NO Flecs access here: the serial drain in pull_from_physics
// (calling thread) performs every ecs_get_mut_id + column write after the
// wait, so all world mutation stays single-threaded.
auto PullShardRun(std::uint32_t start, std::uint32_t end,
                  std::uint32_t /*threadnum*/, void* args) -> void {
  auto* a = static_cast<PullShardArgs*>(args);
  if (a == nullptr || start >= end || a->ids == nullptr ||
      a->rows == nullptr) {
    return;
  }
  const JPH::BodyLockInterfaceLocking& locks =
      static_cast<const JPH::BodyLockInterfaceLocking&>(
          a->system->GetBodyLockInterface());
  for (std::uint32_t i = start; i < end; ++i) {
    const JPH::BodyID bid = a->ids[i];
    JPH::BodyLockRead lock(locks, bid);
    if (!lock.Succeeded()) {
      continue;  // destroyed between snapshot and pull; skip
    }
    const JPH::Body& body = lock.GetBody();
    if (!body.IsActive()) {
      continue;  // slept between snapshot and pull; keep last Flecs pose
    }
    const auto entity = static_cast<std::uint64_t>(body.GetUserData());
    if (entity == 0) {
      continue;  // no Flecs link (untracked body)
    }
    const JPH::RVec3 p = body.GetPosition();
    const JPH::Quat q = body.GetRotation();
    PullRow& row = a->rows[i];
    row.entity = entity;
    row.px = p.GetX();
    row.py = p.GetY();
    row.pz = p.GetZ();
    row.qx = q.GetX();
    row.qy = q.GetY();
    row.qz = q.GetZ();
    row.qw = q.GetW();
    row.valid = 1;
    if (a->done != nullptr) {
      a->done->fetch_add(1, std::memory_order_relaxed);
    }
  }
}

}  // namespace

auto ContactListenerImpl::bind(CollisionEventRing* ring,
                               const JPH::BodyInterface* /*iface*/) noexcept
    -> void {
  ring_ = ring;
  // NOLINTNEXTLINE: init-time only; this TU builds -fno-exceptions.
  void* mem = std::malloc(sizeof(JoltContactForwarder));
  if (mem == nullptr) {
    listener_ = nullptr;
    return;
  }
  // NOLINTNEXTLINE: init-time only, freed never (process-lifetime listener).
  listener_ = new (mem) JoltContactForwarder(ring_);
}

TransformSync::~TransformSync() noexcept { destroy(); }

auto TransformSync::init(ecs_world_t* world,
                         const TransformSyncConfig& cfg) noexcept -> bool {
  if (world_ != nullptr || world == nullptr) {
    return false;
  }
  // Register the PhysicsBody link component (explicit size/align, no
  // ECS_COMPONENT macro — this TU builds -fno-exceptions/-fno-rtti).
  ecs_entity_desc_t ed{};
  ed.name = "PhysicsBody";
  const ecs_entity_t ent = ecs_entity_init(world, &ed);
  ecs_component_desc_t cd{};
  cd.entity = ent;
  cd.type.size = static_cast<ecs_size_t>(sizeof(PhysicsBody));
  cd.type.alignment = static_cast<ecs_size_t>(alignof(PhysicsBody));
  body_id_ = ecs_component_init(world, &cd);
  if (body_id_ == 0) {
    return false;
  }
  cfg_ = cfg;
  if (cfg_.min_range == 0) {
    cfg_.min_range = 256;
  }
  if (cfg_.event_capacity == 0) {
    cfg_.event_capacity = 1U << 16;
  }
  if (!ring_.init(cfg_.event_capacity)) {
    body_id_ = 0;
    return false;
  }
  listener_.bind(&ring_, nullptr);
  if (listener_.native() == nullptr) {
    ring_.destroy();
    body_id_ = 0;
    return false;
  }
  // Cache the (LocalTransform, PhysicsBody) query once: per-frame
  // ecs_query_init/ecs_query_fini would malloc on the frame path.
  const ecs_entity_t local_id = ecs_lookup(world, "LocalTransform");
  if (local_id != 0) {
    ecs_query_desc_t qd{};
    qd.terms[0].id = local_id;
    qd.terms[1].id = static_cast<ecs_entity_t>(body_id_);
    qd.cache_kind = EcsQueryCacheAuto;
    query_ = ecs_query_init(world, &qd);
  }
  // Persistent active-BodyID scratch vector (capacity stable across pulls:
  // Jolt's GetActiveBodies keeps capacity on clear, so steady-state pulls
  // allocate nothing).
  void* vec_mem = std::malloc(sizeof(JPH::BodyIDVector));
  if (vec_mem == nullptr) {
    if (query_ != nullptr) {
      ecs_query_fini(query_);
      query_ = nullptr;
    }
    ring_.destroy();
    body_id_ = 0;
    return false;
  }
  // Persistent active-BodyID scratch vector (capacity stable across pulls:
  // Jolt's GetActiveBodies keeps capacity on clear, so steady-state pulls
  // allocate nothing). PullRow staging rides alongside (realloc-grown,
  // zeroed per pull).
  auto* vec = new (vec_mem) JPH::BodyIDVector();  // NOLINT: init-time only
  vec->reserve(4096);
  active_vec_ = vec;
  void* staging_mem = std::calloc(4096, sizeof(PullRow));
  if (staging_mem == nullptr) {
    std::destroy_at(vec);
    std::free(vec_mem);
    if (query_ != nullptr) {
      ecs_query_fini(query_);
      query_ = nullptr;
    }
    ring_.destroy();
    body_id_ = 0;
    return false;
  }
  staging_vec_ = staging_mem;
  staging_cap_ = 4096;
  world_ = world;
  return true;
}

auto TransformSync::destroy() noexcept -> void {
  // Note: the JoltContactForwarder malloc is intentionally process-lifetime
  // (one 8-byte object; freed at process exit with the module). Ring slots,
  // the BodyID scratch vector, and the pull staging buffer are freed here.
  if (query_ != nullptr) {
    ecs_query_fini(query_);
    query_ = nullptr;
  }
  if (active_vec_ != nullptr) {
    auto* vec = static_cast<JPH::BodyIDVector*>(active_vec_);
    std::destroy_at(vec);
    std::free(vec);  // NOLINT: teardown only
    active_vec_ = nullptr;
  }
  if (staging_vec_ != nullptr) {
    std::free(staging_vec_);  // NOLINT: teardown only
    staging_vec_ = nullptr;
    staging_cap_ = 0;
  }
  task_set_ = nullptr;  // owned by the enkiTS scheduler pool, not us
  ring_.destroy();
  world_ = nullptr;
  body_id_ = 0;
}

auto TransformSync::push_to_physics(JPH::PhysicsSystem& system) noexcept
    -> std::uint32_t {
  if (world_ == nullptr || body_id_ == 0) {
    return 0;
  }
  // Lazy query bind (CodeRabbit review): init() runs before LocalTransform
  // may be registered (test registers builtins per-world after Fixture).
  // Retry once here — a single init-time ecs_query_init, never per frame.
  if (query_ == nullptr) {
    const ecs_entity_t local_id = ecs_lookup(world_, "LocalTransform");
    if (local_id == 0) {
      return 0;  // component not registered yet; AC2 pull shares this path
    }
    ecs_query_desc_t qd{};
    qd.terms[0].id = local_id;
    qd.terms[1].id = static_cast<ecs_entity_t>(body_id_);
    qd.cache_kind = EcsQueryCacheAuto;
    query_ = ecs_query_init(world_, &qd);
    if (query_ == nullptr) {
      return 0;
    }
  }
  JPH::BodyInterface& iface = system.GetBodyInterface();
  std::uint32_t pushed = 0;

  ecs_iter_t it = ecs_query_iter(world_, query_);
  while (ecs_query_next(&it)) {
    auto* locals = static_cast<engine::ecs::LocalTransform*>(
        ecs_field_w_size(&it, sizeof(engine::ecs::LocalTransform), 0));
    auto* links = static_cast<PhysicsBody*>(
        ecs_field_w_size(&it, sizeof(PhysicsBody), 1));
    if (locals == nullptr || links == nullptr) {
      continue;
    }
    for (int r = 0; r < it.count; ++r) {
      const PhysicsBody& link = links[r];
      if (link.body_id == 0xFFFFFFFFu) {
        continue;  // unlinked
      }
      if (link.kinematic == 0) {
        continue;  // static or sim-driven: never push
      }
      const engine::ecs::LocalTransform& t = locals[r];
      const JPH::RVec3 pos(t.px, t.py, t.pz);
      const JPH::Quat rot(t.qx, t.qy, t.qz, t.qw);
      iface.SetPositionAndRotationWhenChanged(
          JPH::BodyID(link.body_id), pos, rot, JPH::EActivation::Activate);
      ++pushed;
    }
  }
  return pushed;
}

auto TransformSync::pull_from_physics(JPH::PhysicsSystem& system,
                                      void* sched_native,
                                      std::uint32_t /*worker_count*/) noexcept
    -> std::uint32_t {
  if (world_ == nullptr || body_id_ == 0) {
    return 0;
  }
  const ecs_entity_t local_id = ecs_lookup(world_, "LocalTransform");
  if (local_id == 0) {
    return 0;
  }
  // Snapshot active rigid bodies into the persistent scratch vector
  // (capacity stable across pulls — no per-frame malloc in steady state).
  // The parallel phase then only reads this snapshot, never the manager.
  auto* active = static_cast<JPH::BodyIDVector*>(active_vec_);
  if (active == nullptr) {
    return 0;
  }
  active->clear();
  system.GetActiveBodies(JPH::EBodyType::RigidBody, *active);
  if (active->empty()) {
    return 0;
  }
  const auto total = static_cast<std::uint32_t>(active->size());

  // Staging buffer: one row per snapshotted body (parallel workers fill,
  // serial drain applies). Capacity tracks the snapshot (amortized growth,
  // never shrinks — no per-frame malloc in steady state). Rows are plain
  // floats/ints, so realloc is memcpy-safe. The enkiTS TaskSet writes each
  // row exactly once (disjoint index ranges); the drain runs strictly after
  // the wait, so no atomics are needed on the rows themselves.
  auto* rows = static_cast<PullRow*>(staging_vec_);
  if (rows == nullptr) {
    return 0;
  }
  std::size_t staged_cap = staging_cap_;
  if (staged_cap < active->size()) {
    // Grow: realloc keeps existing rows (none live across pulls, so a move
    // is fine), zero the extension. Growth only, capped at the snapshot.
    std::size_t need = active->size();
    if (need > static_cast<std::size_t>(0xFFFFFFFEU)) {
      return 0;  // absurd snapshot; fail closed rather than overflow
    }
    void* mem = std::realloc(staging_vec_, need * sizeof(PullRow));
    if (mem == nullptr) {
      return 0;
    }
    std::memset(static_cast<char*>(mem) + staged_cap * sizeof(PullRow), 0,
                (need - staged_cap) * sizeof(PullRow));
    staging_vec_ = mem;
    staging_cap_ = need;
    rows = static_cast<PullRow*>(mem);
    staged_cap = need;
  }
  std::memset(rows, 0, staged_cap * sizeof(PullRow));

  std::atomic<std::uint32_t> done{0};
  PullShardArgs args{};
  args.system = &system;
  args.rows = rows;
  args.ids = active->data();
  args.count = total;
  args.done = &done;

  auto* sched = static_cast<enkiTaskScheduler*>(sched_native);
  if (sched != nullptr && total > 1) {
    enkiTaskSet* set = static_cast<enkiTaskSet*>(task_set_);
    if (set == nullptr) {
      set = enkiCreateTaskSet(sched, PullShardRun);
      task_set_ = set;  // cached for reuse; freed by scheduler teardown
    }
    if (set != nullptr) {
      enkiAddTaskSetMinRange(sched, set, &args, total, cfg_.min_range);
      enkiWaitForTaskSet(sched, set);
    } else {
      // TaskSet alloc failed: run serially into the same staging buffer.
      PullShardRun(0, total, 0, &args);
    }
  } else {
    // Serial fallback (null scheduler, single body).
    PullShardRun(0, total, 0, &args);
  }
  // Serial drain (calling thread owns ALL Flecs mutation): apply staged
  // rows to LocalTransform columns. ecs_get_mut_id is single-threaded
  // here, so entity-liveness validation is exact — stale links skip.
  std::uint32_t synced = 0;
  for (std::uint32_t i = 0; i < total; ++i) {
    const PullRow& row = rows[i];
    if (row.valid == 0 || row.entity == 0) {
      continue;
    }
    auto* lt = static_cast<engine::ecs::LocalTransform*>(ecs_get_mut_id(
        world_, static_cast<ecs_entity_t>(row.entity),
        static_cast<ecs_entity_t>(local_id)));
    if (lt == nullptr) {
      continue;  // entity dead or link stale
    }
    lt->px = row.px;
    lt->py = row.py;
    lt->pz = row.pz;
    lt->qx = row.qx;
    lt->qy = row.qy;
    lt->qz = row.qz;
    lt->qw = row.qw;
    ++synced;
  }
  (void)done;
  return synced;
}

auto TransformSync::drain_events(CollisionEvent* out,
                                 std::size_t max_out) noexcept -> std::size_t {
  return ring_.drain_to(out, max_out);
}

}  // namespace engine::physics

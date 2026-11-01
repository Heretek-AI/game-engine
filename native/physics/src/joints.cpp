// joints.cpp — P6-05: JointManager implementation (Jolt constraints).
//
// Lifecycle notes:
// * init() registers the five joint components + reserves the entry array
//   (init-time malloc only).
// * sync() reconciles: for each joint entity in the five tables, look up
//   the two body entities' PhysicsBody links to resolve Jolt BodyIDs, then
//   create the matching Jolt constraint in world space. Dead joint entities
//   (or removed components) get their Jolt constraint removed + deleted.
// * destroy() removes + deletes every owned constraint before the
//   PhysicsSystem dies — Jolt constraints hold raw Body* so they must never
//   outlive the system.
// * Body resolution: PhysicsBody.body_id is the full
//   GetIndexAndSequenceNumber(); BodyID(body_id) reconstructs it.
#include "engine/physics/joints.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

#include <Jolt/Jolt.h>
#include <Jolt/Core/Reference.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Constraints/Constraint.h>
#include <Jolt/Physics/Constraints/DistanceConstraint.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>
#include <Jolt/Physics/EActivation.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include "engine/physics/transform_sync.hpp"
#include "flecs.h"

namespace engine::physics {

namespace {

constexpr std::uint32_t kInvalidBodyId = 0xFFFFFFFFu;
constexpr std::uint32_t kInitialCapacity = 64;

// Concrete-type deleter: Jolt constraints override operator delete
// per-class (sized, JPH_OVERRIDE_NEW_DELETE); deleting through the
// Constraint* base picks the wrong size. The manager holds constraints as
// Ref<Constraint> (AddConstraint AddRefs into the ConstraintManager list;
// RemoveConstraint drops that Ref). Creation gives refcount 0 (raw), the
// entry-Ref assignment AddRefs to 1 (ours), AddConstraint's internal copy
// AddRefs to 2 — so remove_entry's RemoveConstraint followed by clearing
// our Ref drops exactly to 0 and frees via the concrete type. NO raw
// delete EVER (raw delete races the manager's Ref: double-free on remove,
// dangling walk in GetActiveConstraints). Dispatch on the live subtype
// (which also covers kSpring: springs ARE DistanceConstraints).

// Look up the PhysicsBody link for a Flecs entity; returns INVALID when the
// entity is dead, has no link, or the link is unbound.
auto BodyIdOf(ecs_world_t* world, std::uint64_t entity) noexcept -> std::uint32_t {
  if (world == nullptr || entity == 0) {
    return kInvalidBodyId;
  }
  const ecs_entity_t eid = static_cast<ecs_entity_t>(entity);
  if (!ecs_is_alive(world, eid)) {
    return kInvalidBodyId;
  }
  const auto* link = static_cast<const PhysicsBody*>(
      ecs_get_id(world, eid, ecs_lookup(world, "PhysicsBody")));
  if (link == nullptr) {
    return kInvalidBodyId;
  }
  return link->body_id;
}

}  // namespace

JointManager::~JointManager() noexcept { destroy(); }

namespace {

// Flecs allocates component storage raw and never runs C++ default member
// initializers: an `ecs_add_id(SliderJoint)` left anchor_*/limits_* as
// whatever the allocator returned (zero on Linux by luck, garbage on
// Windows -> NaN constraints). This ctor hook gives every added joint the
// header's defaults.
// Copies bytes from a stack prototype instead of placement-new: the joint
// structs are alignas(64), but a column is only as aligned as the world's
// allocator makes it, and GCC emits aligned AVX stores for `new (p) T{}`.
template <typename T>
void DefaultCtor(void* ptr, std::int32_t count,
                 const ecs_type_info_t* /*ti*/) {
  const T proto{};
  auto* bytes = static_cast<unsigned char*>(ptr);
  for (std::int32_t i = 0; i < count; ++i) {
    std::memcpy(bytes + static_cast<std::size_t>(i) * sizeof(T), &proto,
                sizeof(T));
  }
}

}  // namespace

auto JointManager::init(ecs_world_t* world) noexcept -> bool {
  if (world_ != nullptr || world == nullptr) {
    return false;
  }
  // Register the five joint components (explicit size/align — the TU
  // compiles -fno-exceptions/-fno-rtti, no ECS_COMPONENT macro).
  const char* names[5] = {"FixedJoint", "HingeJoint", "SliderJoint", "DistanceJoint", "SpringJoint"};
  const std::size_t sizes[5] = {sizeof(FixedJoint), sizeof(HingeJoint), sizeof(SliderJoint),
                                sizeof(DistanceJoint), sizeof(SpringJoint)};
  const std::size_t aligns[5] = {alignof(FixedJoint), alignof(HingeJoint), alignof(SliderJoint),
                                 alignof(DistanceJoint), alignof(SpringJoint)};
  for (int i = 0; i < 5; ++i) {
    ecs_entity_desc_t ed{};
    ed.name = names[i];
    const ecs_entity_t ent = ecs_entity_init(world, &ed);
    if (ent == 0) {
      return false;
    }
    ecs_component_desc_t cd{};
    cd.entity = ent;
    cd.type.size = static_cast<ecs_size_t>(sizes[i]);
    cd.type.alignment = static_cast<ecs_size_t>(aligns[i]);
    const ecs_entity_t comp = ecs_component_init(world, &cd);
    if (comp == 0) {
      return false;
    }
    ecs_type_hooks_t hooks{};
    switch (i) {
      case 0: hooks.ctor = &DefaultCtor<FixedJoint>; break;
      case 1: hooks.ctor = &DefaultCtor<HingeJoint>; break;
      case 2: hooks.ctor = &DefaultCtor<SliderJoint>; break;
      case 3: hooks.ctor = &DefaultCtor<DistanceJoint>; break;
      default: hooks.ctor = &DefaultCtor<SpringJoint>; break;
    }
    ecs_set_hooks_id(world, comp, &hooks);
  }
  void* mem = std::malloc(sizeof(Entry) * kInitialCapacity);
  if (mem == nullptr) {
    return false;
  }
  for (std::uint32_t i = 0; i < kInitialCapacity; ++i) {
    new (static_cast<Entry*>(mem) + i) Entry{};
  }
  entries_ = static_cast<Entry*>(mem);
  // Parallel Ref array: placement-new each Ref (array came from malloc, so
  // no ctors ran) — malloc for the Refs is init-time only, same as entries.
  void* rmem = std::malloc(sizeof(JPH::Ref<JPH::Constraint>) * kInitialCapacity);
  if (rmem == nullptr) {
    std::free(mem);
    return false;
  }
  refs_ = static_cast<JPH::Ref<JPH::Constraint>*>(rmem);
  for (std::uint32_t i = 0; i < kInitialCapacity; ++i) {
    new (&refs_[i]) JPH::Ref<JPH::Constraint>();
  }
  capacity_ = kInitialCapacity;
  count_ = 0;
  world_ = world;
  return true;
}

auto JointManager::destroy() noexcept -> void {
  // Release OUR Ref on each live constraint first: the ConstraintManager
  // still holds ITS Ref on every added constraint, so the count drops to 1,
  // never 0 — no free happens here. The PhysicsSystem teardown later drops
  // the manager's Ref and frees via the concrete type. Order that works:
  // manager Refs MUST die before the system (fixture destroys joints first
  // — ~Fixture does joints/sync/ecs/world/arena/sched — so our Refs are
  // gone while the system still stands). Run the Ref dtors by hand (array
  // came from malloc — same pattern as TransformSync teardown).
  if (refs_ != nullptr) {
    for (std::uint32_t i = 0; i < capacity_; ++i) {
      refs_[i].~Ref();
    }
    std::free(refs_);  // NOLINT: teardown only
    refs_ = nullptr;
  }
  if (entries_ != nullptr) {
    std::free(entries_);  // NOLINT: teardown only
    entries_ = nullptr;
  }
  capacity_ = 0;
  count_ = 0;
  world_ = nullptr;
}

auto JointManager::find_entry(std::uint64_t joint_entity) noexcept -> std::int64_t {
  for (std::uint32_t i = 0; i < count_; ++i) {
    if (entries_[i].joint_entity == joint_entity) {
      return static_cast<std::int64_t>(i);
    }
  }
  return -1;
}

auto JointManager::remove_entry(JPH::PhysicsSystem& system, std::size_t idx) noexcept -> void {
  if (idx >= count_) {
    return;
  }
  Entry& e = entries_[idx];
  // Remove from the PhysicsSystem FIRST so the ConstraintManager drops
  // ITS Ref<Constraint>; then clearing our Ref drops the count to zero and
  // frees via the concrete type (RefTarget<Constraint>::Release deletes
  // through the derived type — the sized-operator-delete mismatch that
  // crashed raw `delete` never happens). No raw delete EVER.
  system.RemoveConstraint(e.constraint);
  refs_[idx] = nullptr;
  // Swap-remove both arrays together.
  entries_[idx] = entries_[count_ - 1];
  entries_[count_ - 1] = Entry{};
  refs_[idx] = refs_[count_ - 1];
  refs_[count_ - 1] = nullptr;
  --count_;
}

auto JointManager::hinge_angle(JPH::PhysicsSystem& system, std::uint64_t joint_entity,
                               float* out_angle) noexcept -> bool {
  if (out_angle == nullptr) {
    return false;
  }
  const std::int64_t idx = find_entry(joint_entity);
  if (idx < 0) {
    return false;
  }
  auto* hinge = static_cast<JPH::HingeConstraint*>(entries_[idx].constraint);
  if (hinge == nullptr || entries_[idx].type != JointType::kHinge) {
    return false;
  }
  (void)system;
  *out_angle = hinge->GetCurrentAngle();
  return true;
}

auto JointManager::sync(JPH::PhysicsSystem& system) noexcept -> std::uint32_t {
  if (world_ == nullptr || entries_ == nullptr) {
    return 0;
  }

  // Mark pass: bitset over entries via a parallel alive array is overkill
  // for joint counts — use a serial "seen" stamp pass instead: compact the
  // entry list by removing entries whose joint entity is dead or lost its
  // component, then create constraints for joint entities not yet tracked.
  //
  // Pass 1: drop dead entries.
  for (std::uint32_t i = 0; i < count_;) {
    const std::uint64_t je = entries_[i].joint_entity;
    bool alive = false;
    if (je != 0 && ecs_is_alive(world_, static_cast<ecs_entity_t>(je))) {
      // Still carries the matching component?
      const char* want = "";
      switch (entries_[i].type) {
        case JointType::kFixed: want = "FixedJoint"; break;
        case JointType::kHinge: want = "HingeJoint"; break;
        case JointType::kSlider: want = "SliderJoint"; break;
        case JointType::kDistance: want = "DistanceJoint"; break;
        case JointType::kSpring: want = "SpringJoint"; break;
      }
      const ecs_entity_t comp = ecs_lookup(world_, want);
      alive = comp != 0 && ecs_has_id(world_, static_cast<ecs_entity_t>(je), comp);
    }
    if (!alive) {
      remove_entry(system, i);  // swap-removes; do not advance
    } else {
      ++i;
    }
  }

  // Pass 2: create constraints for untracked joint entities. One scan per
  // joint type (five small table scans; joint counts are tiny).
  struct ScanSpec {
    const char* name;
    JointType type;
  };
  const ScanSpec specs[5] = {
      {"FixedJoint", JointType::kFixed},   {"HingeJoint", JointType::kHinge},
      {"SliderJoint", JointType::kSlider},  {"DistanceJoint", JointType::kDistance},
      {"SpringJoint", JointType::kSpring},
  };
  for (const auto& spec : specs) {
    const ecs_entity_t comp = ecs_lookup(world_, spec.name);
    if (comp == 0) {
      continue;
    }
    ecs_query_desc_t qd{};
    qd.terms[0].id = comp;
    qd.cache_kind = EcsQueryCacheAuto;
    ecs_query_t* q = ecs_query_init(world_, &qd);
    if (q == nullptr) {
      continue;
    }
    ecs_iter_t it = ecs_query_iter(world_, q);
    while (ecs_query_next(&it)) {
      const auto* joints =
          static_cast<const std::uint8_t*>(ecs_field_w_size(&it, 64, 0));
      if (joints == nullptr) {
        continue;
      }
      for (int r = 0; r < it.count; ++r) {
        const std::uint64_t je = static_cast<std::uint64_t>(it.entities[r]);
        if (find_entry(je) >= 0) {
          continue;  // already constrained
        }
        const void* data = joints + static_cast<std::size_t>(r) * 64U;
        std::uint64_t body_a = 0;
        std::uint64_t body_b = 0;
        std::uint8_t enabled = 1;
        std::memcpy(&body_a, static_cast<const std::uint8_t*>(data) + 0, 8);
        std::memcpy(&body_b, static_cast<const std::uint8_t*>(data) + 8, 8);
        if (spec.type == JointType::kFixed) {
          enabled = *(static_cast<const std::uint8_t*>(data) + 16);
        } else if (spec.type == JointType::kHinge) {
          enabled = *(static_cast<const std::uint8_t*>(data) + 40);
        } else if (spec.type == JointType::kSlider) {
          enabled = *(static_cast<const std::uint8_t*>(data) + 36);
        } else {
          enabled = *(static_cast<const std::uint8_t*>(data) + 48);
        }
        if (enabled == 0 || body_a == 0) {
          continue;  // disabled or no primary body
        }
        const std::uint32_t bid_a = BodyIdOf(world_, body_a);
        if (bid_a == kInvalidBodyId) {
          continue;  // body not created yet; retry next sync
        }
        // body_b == 0 means world anchor: create a static anchor body?
        // Jolt two-body constraints need two bodies. For a world anchor
        // we use the fixed body path: create constraint against a static
        // body created on demand is out of scope — require both bodies.
        if (body_b == 0) {
          continue;
        }
        const std::uint32_t bid_b = BodyIdOf(world_, body_b);
        if (bid_b == kInvalidBodyId) {
          continue;
        }
        // Grow both arrays (init-time style realloc — amortized, not
        // per-frame in steady state). Refs need ctor/dtor by hand: destroy
        // the old range, realloc the bytes, placement-new the full range,
        // then re-AddRef the live ones from the raw handles.
        if (count_ >= capacity_) {
          const std::uint32_t ncap = capacity_ * 2;
          void* mem = std::realloc(entries_, sizeof(Entry) * ncap);
          if (mem == nullptr) {
            return count_;
          }
          entries_ = static_cast<Entry*>(mem);
          for (std::uint32_t i = capacity_; i < ncap; ++i) {
            new (&entries_[i]) Entry{};
          }
          void* rmem = std::malloc(sizeof(JPH::Ref<JPH::Constraint>) * ncap);
          if (rmem == nullptr) {
            return count_;
          }
          auto* nrefs = static_cast<JPH::Ref<JPH::Constraint>*>(rmem);
          for (std::uint32_t i = 0; i < ncap; ++i) {
            new (&nrefs[i]) JPH::Ref<JPH::Constraint>();
          }
          for (std::uint32_t i = 0; i < count_; ++i) {
            nrefs[i] = entries_[i].constraint;
          }
          for (std::uint32_t i = 0; i < capacity_; ++i) {
            refs_[i].~Ref();
          }
          std::free(refs_);
          refs_ = nrefs;
          capacity_ = ncap;
        }
        JPH::BodyID jid_a(bid_a);
        JPH::BodyID jid_b(bid_b);
        // Snapshot body state under read locks, then release BEFORE Create:
        // constraint creation reads body state, and holding locks across
        // Create risks deadlock with the solver's lock ordering. Snapshot
        // the COM transforms + dynamic flags needed for world-space setup.
        JPH::RVec3 pos_a, pos_b;
        JPH::Quat rot_a, rot_b;
        bool dyn_a = false;
        bool dyn_b = false;
        {
          JPH::BodyLockRead lock_a(system.GetBodyLockInterface(), jid_a);
          JPH::BodyLockRead lock_b(system.GetBodyLockInterface(), jid_b);
          if (!lock_a.Succeeded() || !lock_b.Succeeded()) {
            continue;
          }
          const JPH::Body& ba = lock_a.GetBody();
          const JPH::Body& bb = lock_b.GetBody();
          pos_a = ba.GetPosition();
          rot_a = ba.GetRotation();
          pos_b = bb.GetPosition();
          rot_b = bb.GetRotation();
          dyn_a = ba.IsDynamic();
          dyn_b = bb.IsDynamic();
        }
        if (!dyn_a && !dyn_b) {
          continue;  // Jolt two-body constraints need a dynamic body
        }
        (void)pos_a;
        (void)rot_a;
        (void)pos_b;
        (void)rot_b;
        // Create needs Body& — but locks are released. Bodies stay alive
        // because removal only happens through remove_entry (serial with
        // this scan) or PhysicsSystem teardown (manager destroyed first),
        // and no solver step runs concurrently with sync(). Re-lock for
        // write across Create + AddConstraint (short, uncontended).
        JPH::BodyLockWrite wlock_a(system.GetBodyLockInterface(), jid_a);
        JPH::BodyLockWrite wlock_b(system.GetBodyLockInterface(), jid_b);
        if (!wlock_a.Succeeded() || !wlock_b.Succeeded()) {
          continue;
        }
        JPH::Body& ba = wlock_a.GetBody();
        JPH::Body& bb = wlock_b.GetBody();
        JPH::Constraint* c = nullptr;
        switch (spec.type) {
          case JointType::kFixed: {
            JPH::FixedConstraintSettings s;
            s.mSpace = JPH::EConstraintSpace::WorldSpace;
            s.mAutoDetectPoint = true;
            c = s.Create(ba, bb);
            break;
          }
          case JointType::kHinge: {
            const auto* hj = static_cast<const HingeJoint*>(data);
            JPH::HingeConstraintSettings s;
            s.mSpace = JPH::EConstraintSpace::WorldSpace;
            s.mPoint1 = JPH::RVec3(hj->anchor_x, hj->anchor_y, hj->anchor_z);
            s.mPoint2 = JPH::RVec3(hj->anchor_x, hj->anchor_y, hj->anchor_z);
            s.mHingeAxis1 = JPH::Vec3::sAxisY();
            s.mNormalAxis1 = JPH::Vec3::sAxisX();
            s.mHingeAxis2 = JPH::Vec3::sAxisY();
            s.mNormalAxis2 = JPH::Vec3::sAxisX();
            s.mLimitsMin = hj->limits_min;
            s.mLimitsMax = hj->limits_max;
            s.mMaxFrictionTorque = hj->max_friction_torque;
            c = s.Create(ba, bb);
            break;
          }
          case JointType::kSlider: {
            const auto* sj = static_cast<const SliderJoint*>(data);
            JPH::SliderConstraintSettings s;
            s.mSpace = JPH::EConstraintSpace::WorldSpace;
            s.mAutoDetectPoint = true;
            s.mSliderAxis1 = JPH::Vec3::sAxisX();
            s.mNormalAxis1 = JPH::Vec3::sAxisY();
            s.mSliderAxis2 = JPH::Vec3::sAxisX();
            s.mNormalAxis2 = JPH::Vec3::sAxisY();
            s.mLimitsMin = sj->limits_min;
            s.mLimitsMax = sj->limits_max;
            if (sj->anchor_x != 0.0F || sj->anchor_y != 0.0F || sj->anchor_z != 0.0F) {
              s.mAutoDetectPoint = false;
              s.mPoint1 = JPH::RVec3(sj->anchor_x, sj->anchor_y, sj->anchor_z);
              s.mPoint2 = JPH::RVec3(sj->anchor_x, sj->anchor_y, sj->anchor_z);
            }
            c = s.Create(ba, bb);
            break;
          }
          case JointType::kDistance: {
            const auto* dj = static_cast<const DistanceJoint*>(data);
            JPH::DistanceConstraintSettings s;
            s.mSpace = JPH::EConstraintSpace::WorldSpace;
            s.mPoint1 = JPH::RVec3(dj->anchor_a_x, dj->anchor_a_y, dj->anchor_a_z);
            s.mPoint2 = JPH::RVec3(dj->anchor_b_x, dj->anchor_b_y, dj->anchor_b_z);
            s.mMinDistance = dj->min_distance;
            s.mMaxDistance = dj->max_distance;
            c = s.Create(ba, bb);
            break;
          }
          case JointType::kSpring: {
            const auto* sj = static_cast<const SpringJoint*>(data);
            JPH::DistanceConstraintSettings s;
            s.mSpace = JPH::EConstraintSpace::WorldSpace;
            s.mPoint1 = JPH::RVec3(sj->anchor_a_x, sj->anchor_a_y, sj->anchor_a_z);
            s.mPoint2 = JPH::RVec3(sj->anchor_b_x, sj->anchor_b_y, sj->anchor_b_z);
            s.mMinDistance = -1.0F;  // creation-time distance
            s.mMaxDistance = -1.0F;
            s.mLimitsSpringSettings.mMode = JPH::ESpringMode::FrequencyAndDamping;
            s.mLimitsSpringSettings.mFrequency = sj->frequency;
            s.mLimitsSpringSettings.mDamping = sj->damping;
            c = s.Create(ba, bb);
            break;
          }
        }
        if (c == nullptr) {
          continue;
        }
        system.AddConstraint(c);
        entries_[count_].joint_entity = je;
        entries_[count_].type = spec.type;
        entries_[count_].constraint = c;
        refs_[count_] = c;  // OUR Ref (manager holds its own after Add)
        ++count_;
      }  // for r
    }    // while next
    ecs_query_fini(q);
  }
  return count_;
}

}  // namespace engine::physics

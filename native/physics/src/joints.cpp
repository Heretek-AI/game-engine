// joints.cpp — P6-05: JointManager implementation (Jolt constraints).
//
// Lifecycle notes:
// * init() registers the five joint components, creates one cached query
//   per joint type, and reserves the entry array.
// * sync() reconciles: for each joint entity in the five tables, look up
//   the two body entities' PhysicsBody links to resolve Jolt BodyIDs, then
//   create the matching Jolt constraint in world space. Dead joint entities,
//   removed or disabled components, and changed body links get their Jolt
//   constraint removed (re-created next pass when still valid).
// * destroy() removes every owned constraint from the last synced
//   PhysicsSystem — Jolt constraints hold raw Body* so they must never
//   outlive the bodies or the system.
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
#include <Jolt/Physics/Body/BodyLockMulti.h>
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
auto BodyIdOf(ecs_world_t* world, ecs_entity_t physics_body,
              std::uint64_t entity) noexcept -> std::uint32_t {
  if (world == nullptr || entity == 0 || physics_body == 0) {
    return kInvalidBodyId;
  }
  const ecs_entity_t eid = static_cast<ecs_entity_t>(entity);
  if (!ecs_is_alive(world, eid)) {
    return kInvalidBodyId;
  }
  const auto* link = static_cast<const PhysicsBody*>(
      ecs_get_id(world, eid, physics_body));
  if (link == nullptr) {
    return kInvalidBodyId;
  }
  return link->body_id;
}

// Body links + enabled flag of one joint component (all five layouts start
// with body_a @0, body_b @8; enabled sits at a per-type offset).
struct JointLinks {
  std::uint64_t body_a;
  std::uint64_t body_b;
  std::uint8_t enabled;
};

auto ReadLinks(JointType type, const void* data) noexcept -> JointLinks {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  JointLinks out{};
  std::memcpy(&out.body_a, bytes + 0, 8);
  std::memcpy(&out.body_b, bytes + 8, 8);
  std::size_t enabled_at = 48;  // Distance, Spring
  if (type == JointType::kFixed) {
    enabled_at = 16;
  } else if (type == JointType::kHinge) {
    enabled_at = 40;
  } else if (type == JointType::kSlider) {
    enabled_at = 36;
  }
  out.enabled = bytes[enabled_at];
  return out;
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
    comp_ids_[i] = static_cast<std::uint64_t>(comp);
  }
  for (int i = 0; i < 5; ++i) {
    ecs_query_desc_t qd{};
    qd.terms[0].id = static_cast<ecs_entity_t>(comp_ids_[i]);
    qd.cache_kind = EcsQueryCacheAuto;
    queries_[i] = ecs_query_init(world, &qd);
    if (queries_[i] == nullptr) {
      for (int j = 0; j < i; ++j) {
        ecs_query_fini(queries_[j]);
        queries_[j] = nullptr;
      }
      return false;
    }
  }
  // PhysicsBody may be registered by TransformSync after us; resolved
  // lazily in sync() until found.
  physics_body_id_ = static_cast<std::uint64_t>(ecs_lookup(world, "PhysicsBody"));
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
  // Remove every live constraint from the system first (drops the
  // system's Ref); clearing our Ref below then frees it.
  if (system_ != nullptr && entries_ != nullptr && refs_ != nullptr) {
    for (std::uint32_t i = 0; i < count_; ++i) {
      if (entries_[i].constraint != nullptr) {
        system_->RemoveConstraint(entries_[i].constraint);
      }
    }
  }
  if (world_ != nullptr) {
    for (auto& q : queries_) {
      if (q != nullptr) {
        ecs_query_fini(q);
        q = nullptr;
      }
    }
  }
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
  system_ = nullptr;
  physics_body_id_ = 0;
  for (auto& c : comp_ids_) {
    c = 0;
  }
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
  system_ = &system;
  if (physics_body_id_ == 0) {
    physics_body_id_ = static_cast<std::uint64_t>(ecs_lookup(world_, "PhysicsBody"));
  }
  const auto physics_body = static_cast<ecs_entity_t>(physics_body_id_);

  // Pass 1: drop entries whose joint entity died, lost or disabled its
  // component, or now links different bodies (Pass 2 rebuilds those).
  for (std::uint32_t i = 0; i < count_;) {
    const std::uint64_t je = entries_[i].joint_entity;
    bool keep = false;
    if (je != 0 && ecs_is_alive(world_, static_cast<ecs_entity_t>(je))) {
      const auto comp = static_cast<ecs_entity_t>(
          comp_ids_[static_cast<std::uint32_t>(entries_[i].type)]);
      const void* data = comp != 0 ? ecs_get_id(world_, static_cast<ecs_entity_t>(je), comp) : nullptr;
      if (data != nullptr) {
        const JointLinks links = ReadLinks(entries_[i].type, data);
        keep = links.enabled != 0 &&
               BodyIdOf(world_, physics_body, links.body_a) == entries_[i].body_a &&
               BodyIdOf(world_, physics_body, links.body_b) == entries_[i].body_b;
      }
    }
    if (!keep) {
      remove_entry(system, i);  // swap-removes; do not advance
    } else {
      ++i;
    }
  }

  // Pass 2: create constraints for untracked joint entities. One scan per
  // joint type (five small table scans; joint counts are tiny).
  const JointType types[5] = {JointType::kFixed, JointType::kHinge, JointType::kSlider,
                              JointType::kDistance, JointType::kSpring};
  for (int t = 0; t < 5; ++t) {
    const JointType type = types[t];
    ecs_query_t* q = queries_[t];
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
        const JointLinks links = ReadLinks(type, data);
        const std::uint64_t body_a = links.body_a;
        const std::uint64_t body_b = links.body_b;
        if (links.enabled == 0 || body_a == 0) {
          continue;  // disabled or no primary body
        }
        const std::uint32_t bid_a = BodyIdOf(world_, physics_body, body_a);
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
        const std::uint32_t bid_b = BodyIdOf(world_, physics_body, body_b);
        if (bid_b == kInvalidBodyId || bid_b == bid_a) {
          continue;  // unresolved, or a joint from a body to itself
        }
        // Grow both arrays (init-time style realloc — amortized, not
        // per-frame in steady state). Refs need ctor/dtor by hand: destroy
        // the old range, realloc the bytes, placement-new the full range,
        // then re-AddRef the live ones from the raw handles.
        if (count_ >= capacity_) {
          const std::uint32_t ncap = capacity_ * 2;
          void* mem = std::realloc(entries_, sizeof(Entry) * ncap);
          if (mem == nullptr) {
            ecs_iter_fini(&it);
            return count_;
          }
          entries_ = static_cast<Entry*>(mem);
          for (std::uint32_t i = capacity_; i < ncap; ++i) {
            new (&entries_[i]) Entry{};
          }
          void* rmem = std::malloc(sizeof(JPH::Ref<JPH::Constraint>) * ncap);
          if (rmem == nullptr) {
            ecs_iter_fini(&it);
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
        // Lock both bodies with one multi-body lock: two separate locks can
        // map to the same mutex in Jolt's mutex array and self-deadlock.
        const JPH::BodyID ids[2] = {JPH::BodyID(bid_a), JPH::BodyID(bid_b)};
        JPH::BodyLockMultiWrite wlock(system.GetBodyLockInterface(), ids, 2);
        JPH::Body* pa = wlock.GetBody(0);
        JPH::Body* pb = wlock.GetBody(1);
        if (pa == nullptr || pb == nullptr) {
          continue;
        }
        if (!pa->IsDynamic() && !pb->IsDynamic()) {
          continue;  // Jolt two-body constraints need a dynamic body
        }
        JPH::Body& ba = *pa;
        JPH::Body& bb = *pb;
        JPH::Constraint* c = nullptr;
        switch (type) {
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
        entries_[count_].type = type;
        entries_[count_].constraint = c;
        entries_[count_].body_a = bid_a;
        entries_[count_].body_b = bid_b;
        refs_[count_] = c;  // OUR Ref (manager holds its own after Add)
        ++count_;
      }  // for r
    }    // while next
  }
  return count_;
}

}  // namespace engine::physics

// collision_events.hpp — M4-02: lock-free native collision/trigger event ring.
//
// Jolt's ContactListener callbacks (OnContactAdded / OnContactRemoved)
// fire on physics worker threads DURING PhysicsSystem::Update — they must
// never allocate, take locks, or call into Flecs/C#. This header provides:
//
//   CollisionEvent  — one blittable 32-byte record (entity ids carried as
//                     Jolt UserData = Flecs entity, set at body creation).
//   CollisionEventRing — fixed-capacity MPMC ring with per-slot sequence
//                     numbers (Dmitry Vyukov's bounded MPMC queue). try_push
//                     from ANY thread (contact callbacks), drain_to from a
//                     single consumer after the step. Overflow is counted,
//                     never blocking: on a full ring the event is dropped and
//                     dropped() increments (telemetry for the milestone gate).
//
// Frame path: zero malloc, zero locks (single CAS per slot), noexcept.
// Drain hands contiguous CollisionEvent batches to the caller — the C#
// dispatch side (M4-03) views them as ReadOnlySpan<CollisionEvent>.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/core/engine_core_export.hpp"

namespace engine::physics {

// Collision/trigger event kinds reported by the contact listener.
enum class CollisionEventKind : std::uint8_t {
  kEnter = 0,  // OnContactAdded: first step this pair touches
  kExit = 1,   // OnContactRemoved: pair separated (or a body was destroyed)
};

// One collision record. Blittable, 32 bytes, 8-byte aligned — the managed
// mirror (M4-03) declares the same field order. entity_a/entity_b carry
// Jolt Body::GetUserData() (the Flecs entity, set at body creation); zero
// means "unknown side" (e.g. body destroyed before Exit drained).
struct CollisionEvent {
  std::uint64_t entity_a{0};
  std::uint64_t entity_b{0};
  float point_x{0.0F};
  float point_y{0.0F};
  float point_z{0.0F};
  CollisionEventKind kind{CollisionEventKind::kEnter};
  std::uint8_t sensor{0};  // 1 when either body is a sensor (trigger event)
  std::uint8_t reserved[2]{};
};

static_assert(sizeof(CollisionEvent) == 32, "CollisionEvent must be 32 bytes");
static_assert(alignof(CollisionEvent) == 8, "CollisionEvent align 8");

// Bounded MPMC ring buffer with per-slot sequence numbers.
// Capacity is rounded up to a power of two at init(). try_push is safe from
// any number of producer threads; drain_to must be called from ONE consumer
// thread at a time (the post-step drain). All methods noexcept; no malloc
// after init (slots + sequences allocated once at init, freed at destroy).
class ENGINE_CORE_API CollisionEventRing {
 public:
  CollisionEventRing() noexcept = default;
  CollisionEventRing(const CollisionEventRing&) = delete;
  auto operator=(const CollisionEventRing&) -> CollisionEventRing& = delete;
  ~CollisionEventRing() noexcept;

  // Allocate slot storage for `capacity` events (rounded up to pow2).
  // Returns false on zero capacity or allocation failure. Init-time only.
  auto init(std::size_t capacity) noexcept -> bool;
  auto destroy() noexcept -> void;

  [[nodiscard]] auto capacity() const noexcept -> std::size_t {
    return mask_ + 1;
  }
  [[nodiscard]] auto dropped() const noexcept -> std::uint64_t {
    return dropped_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] auto valid() const noexcept -> bool {
    return slots_ != nullptr;
  }

  // Producer: copy `ev` into the ring. Returns false (and counts a drop)
  // when the ring is momentarily full. Lock-free, noexcept.
  auto try_push(const CollisionEvent& ev) noexcept -> bool;

  // Consumer: move up to `max_out` pending events into `out` (contiguous),
  // in FIFO order. Returns events written. Single-consumer only.
  auto drain_to(CollisionEvent* out, std::size_t max_out) noexcept
      -> std::size_t;

  // Consumer: pending event count (approximate under concurrent producers).
  [[nodiscard]] auto pending() const noexcept -> std::size_t;

 private:
  struct Slot {
    std::atomic<std::uint64_t> seq{0};
    CollisionEvent ev{};
  };

  Slot* slots_{nullptr};
  std::size_t mask_{0};  // capacity - 1 (capacity always pow2)
  alignas(64) std::atomic<std::uint64_t> head_{0};  // consumer cursor
  alignas(64) std::atomic<std::uint64_t> tail_{0};  // producer cursor
  alignas(64) std::atomic<std::uint64_t> dropped_{0};
};

}  // namespace engine::physics

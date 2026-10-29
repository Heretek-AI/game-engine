// collision_events.cpp — M4-02: bounded MPMC ring implementation.
//
// Vyukov's bounded MPMC queue: each slot carries a sequence number; a
// producer CAS-claims tail, spins until its slot's seq == tail (slot free),
// writes the event, then publishes seq = tail+1. The consumer reads head,
// spins until slot seq == head+1 (slot ready), copies out, marks free.
// A full ring (seq mismatch) drops the event and counts it — the physics
// step must never block on a slow consumer.
#include "engine/physics/collision_events.hpp"

#include <cstdlib>

namespace engine::physics {

namespace {

// Round up to the next power of two (minimum 2).
auto next_pow2(std::size_t n) noexcept -> std::size_t {
  std::size_t p = 2;
  while (p < n) {
    p <<= 1U;
  }
  return p;
}

}  // namespace

CollisionEventRing::~CollisionEventRing() noexcept { destroy(); }

auto CollisionEventRing::init(std::size_t capacity) noexcept -> bool {
  if (slots_ != nullptr || capacity == 0) {
    return false;
  }
  const std::size_t cap = next_pow2(capacity);
  // NOLINTNEXTLINE: init-time only; this TU builds -fno-exceptions.
  void* mem = std::malloc(sizeof(Slot) * cap);
  if (mem == nullptr) {
    return false;
  }
  auto* slots = static_cast<Slot*>(mem);
  for (std::size_t i = 0; i < cap; ++i) {
    // Placement-new: Slot holds an atomic (non-trivial init).
    new (&slots[i]) Slot();  // NOLINT: init-time only
    slots[i].seq.store(i, std::memory_order_relaxed);
  }
  slots_ = slots;
  mask_ = cap - 1;
  head_.store(0, std::memory_order_relaxed);
  tail_.store(0, std::memory_order_relaxed);
  dropped_.store(0, std::memory_order_relaxed);
  return true;
}

auto CollisionEventRing::destroy() noexcept -> void {
  if (slots_ != nullptr) {
    const std::size_t cap = mask_ + 1;
    for (std::size_t i = 0; i < cap; ++i) {
      slots_[i].~Slot();
    }
    std::free(slots_);  // NOLINT: teardown only
    slots_ = nullptr;
  }
  mask_ = 0;
  head_.store(0, std::memory_order_relaxed);
  tail_.store(0, std::memory_order_relaxed);
  dropped_.store(0, std::memory_order_relaxed);
}

auto CollisionEventRing::try_push(const CollisionEvent& ev) noexcept -> bool {
  if (slots_ == nullptr) {
    return false;
  }
  std::uint64_t pos = tail_.load(std::memory_order_relaxed);
  for (;;) {
    Slot& slot = slots_[pos & mask_];
    const std::uint64_t seq = slot.seq.load(std::memory_order_acquire);
    const auto diff = static_cast<std::int64_t>(seq) -
                      static_cast<std::int64_t>(pos);
    if (diff == 0) {
      // Slot is free; claim it.
      if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed,
                                      std::memory_order_relaxed)) {
        slot.ev = ev;
        slot.seq.store(pos + 1, std::memory_order_release);
        return true;
      }
      // Lost the race: `pos` reloaded by the CAS; retry.
      continue;
    }
    if (diff < 0) {
      // Ring full: consumer hasn't freed this slot yet.
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    // Another producer claimed ahead of us; reload the cursor.
    pos = tail_.load(std::memory_order_relaxed);
  }
}

auto CollisionEventRing::drain_to(CollisionEvent* out,
                                  std::size_t max_out) noexcept -> std::size_t {
  if (slots_ == nullptr || out == nullptr || max_out == 0) {
    return 0;
  }
  std::size_t n = 0;
  std::uint64_t pos = head_.load(std::memory_order_relaxed);
  while (n < max_out) {
    Slot& slot = slots_[pos & mask_];
    const std::uint64_t seq = slot.seq.load(std::memory_order_acquire);
    const auto diff = static_cast<std::int64_t>(seq) -
                      static_cast<std::int64_t>(pos + 1);
    if (diff != 0) {
      break;  // Empty (diff < 0) or producer still writing (diff > 0).
    }
    out[n] = slot.ev;
    slot.seq.store(pos + mask_ + 1, std::memory_order_release);
    ++pos;
    ++n;
  }
  head_.store(pos, std::memory_order_relaxed);
  return n;
}

auto CollisionEventRing::pending() const noexcept -> std::size_t {
  if (slots_ == nullptr) {
    return 0;
  }
  const std::uint64_t tail = tail_.load(std::memory_order_acquire);
  const std::uint64_t head = head_.load(std::memory_order_acquire);
  return tail >= head ? static_cast<std::size_t>(tail - head) : 0;
}

}  // namespace engine::physics

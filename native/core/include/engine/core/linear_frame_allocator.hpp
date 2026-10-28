// linear_frame_allocator.hpp — M1-02: per-frame bump allocator.
//
// Lifetime model:
//   - The allocator owns one contiguous, cache-aligned heap reservation for
//     the whole run (backed by the PageArenaAllocator or plain aligned
//     memory; ownership is taken by the caller-chosen init path).
//   - Per frame, transient systems bump-allocate scratch memory out of it.
//   - At the frame boundary the owner calls reset(); all scratch memory is
//     reclaimed in O(1). Individual free() is a no-op by design.
//
// Threading: allocate() is safe to call concurrently from any number of
// worker threads (lock-free CAS loop on an atomic head). reset() is NOT
// thread-safe — call it once per frame from the frame owner after all
// workers have joined (same contract as the Flecs pipeline sync point).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace engine::core {

// Engine-wide cache-line size. All frame-path allocations are rounded up
// to this so adjacent component columns never share a line (false-sharing
// guard for the M1 1M-transform benchmark).
inline constexpr std::size_t kCacheLineSize = 64;

class LinearFrameAllocator {
 public:
  LinearFrameAllocator() noexcept = default;
  LinearFrameAllocator(const LinearFrameAllocator&) = delete;
  auto operator=(const LinearFrameAllocator&) -> LinearFrameAllocator& = delete;

  // Back the allocator with caller-owned memory [base, base + capacity).
  // base SHOULD be kCacheLineSize-aligned; if it is not, the allocator
  // aligns the first bump up and reports the usable capacity accordingly.
  // init() must be called once before any allocate(); calling it twice
  // re-binds to the new buffer (previous scratch is abandoned).
  auto init(void* base, std::size_t capacity) noexcept -> void;

  // Bump-allocate `size` bytes with `alignment` (power of two, defaults to
  // the cache line). Returns nullptr when out of memory. Never throws,
  // never touches the heap. Lock-free: safe from any worker thread.
  [[nodiscard]] auto allocate(std::size_t size,
                              std::size_t alignment = kCacheLineSize) noexcept
      -> void*;

  // O(1) frame-boundary reclaim. NOT thread-safe: call only when no
  // worker is inside allocate().
  auto reset() noexcept -> void;

  // Bytes handed out since the last reset (for telemetry / overflow stats).
  [[nodiscard]] auto used() const noexcept -> std::size_t {
    return head_.load(std::memory_order_relaxed);
  }
  // Usable capacity in bytes (after base alignment).
  [[nodiscard]] auto capacity() const noexcept -> std::size_t {
    return capacity_;
  }

 private:
  // Round `v` up to a multiple of `alignment` (power of two, nonzero).
  static auto AlignUp(std::uintptr_t v, std::size_t alignment) noexcept
      -> std::uintptr_t {
    const auto mask = static_cast<std::uintptr_t>(alignment - 1U);
    return (v + mask) & ~mask;
  }

  std::uintptr_t base_aligned_{0};  // First usable address.
  std::size_t capacity_{0};         // Usable bytes from base_aligned_.
  std::atomic<std::size_t> head_{0};
};

}  // namespace engine::core

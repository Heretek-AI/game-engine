// linear_frame_allocator.cpp — lock-free bump allocator; see header.
#include "engine/core/linear_frame_allocator.hpp"

namespace engine::core {

auto LinearFrameAllocator::init(void* base, std::size_t capacity) noexcept
    -> void {
  head_.store(0, std::memory_order_relaxed);
  capacity_ = 0;
  base_aligned_ = 0;
  if (base == nullptr || capacity == 0) {
    return;
  }
  const auto raw = reinterpret_cast<std::uintptr_t>(base);  // NOLINT
  const auto aligned = AlignUp(raw, kCacheLineSize);
  const auto slack = static_cast<std::size_t>(aligned - raw);
  if (slack >= capacity) {
    return;
  }
  base_aligned_ = aligned;
  capacity_ = capacity - slack;
}

auto LinearFrameAllocator::allocate(std::size_t size,
                                    std::size_t alignment) noexcept -> void* {
  if (size == 0 || base_aligned_ == 0) {
    return nullptr;
  }
  if (alignment == 0 || (alignment & (alignment - 1U)) != 0U) {
    alignment = kCacheLineSize;
  }
  if (alignment < alignof(void*)) {
    alignment = alignof(void*);
  }
  std::size_t head = head_.load(std::memory_order_relaxed);
  for (;;) {
    const std::uintptr_t cur = base_aligned_ + head;
    const std::uintptr_t aligned = AlignUp(cur, alignment);
    const std::size_t pad = static_cast<std::size_t>(aligned - cur);
    const std::size_t next = head + pad + size;
    if (next > capacity_ || next < head) {  // OOM or wraparound.
      return nullptr;
    }
    if (head_.compare_exchange_weak(head, next, std::memory_order_relaxed,
                                    std::memory_order_relaxed)) {
      return reinterpret_cast<void*>(aligned);  // NOLINT
    }
    // CAS updated `head`; loop re-aligns from the fresh value.
  }
}

auto LinearFrameAllocator::reset() noexcept -> void {
  head_.store(0, std::memory_order_relaxed);
}

}  // namespace engine::core

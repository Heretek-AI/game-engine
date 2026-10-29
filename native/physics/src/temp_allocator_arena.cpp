// temp_allocator_arena.cpp — M4-01: LIFO bump region carved from PageArena.
//
// Reserve one aligned block at init (Jolt needs JPH_RVECTOR_ALIGNMENT).
// Allocate bumps atomically; Free rewinds after validating the pointer is
// the current stack top (foreign/wrong-order frees are ignored rather than
// corrupting the top — upstream aborts; we fail silent by design so a buggy
// caller can't tear down the arena). LIFO order is guaranteed by Jolt's job
// dependencies (same contract as TempAllocatorImpl, but the memory comes
// from our PageArena reservation instead of malloc-at-construct).
#include "engine/physics/temp_allocator_arena.hpp"

#include <Jolt/Core/Core.h>

namespace engine::physics {

ArenaTempAllocator::~ArenaTempAllocator() noexcept { destroy(); }

auto ArenaTempAllocator::init(core::PageArenaAllocator* arena,
                              std::size_t size) noexcept -> bool {
  destroy();
  if (size == 0 || arena == nullptr) {
    return false;
  }
  void* mem = arena->allocate(size, JPH_RVECTOR_ALIGNMENT);
  if (mem == nullptr) {
    return false;
  }
  base_ = reinterpret_cast<std::uintptr_t>(mem);  // NOLINT
  size_ = size;
  top_.store(0, std::memory_order_relaxed);
  return true;
}

auto ArenaTempAllocator::destroy() noexcept -> void {
  // Block is arena-owned (freed wholesale by the arena); just forget it.
  base_ = 0;
  size_ = 0;
  top_.store(0, std::memory_order_relaxed);
}

auto ArenaTempAllocator::Allocate(JPH::uint inSize) -> void* {
  if (inSize == 0 || base_ == 0) {
    return nullptr;
  }
  const std::size_t need =
      AlignUp(static_cast<std::size_t>(inSize), JPH_RVECTOR_ALIGNMENT);
  std::size_t cur = top_.load(std::memory_order_relaxed);
  for (;;) {
    if (cur + need > size_) {
      return nullptr;
    }
    if (top_.compare_exchange_weak(cur, cur + need, std::memory_order_relaxed,
                                   std::memory_order_relaxed)) {
      return reinterpret_cast<void*>(base_ + cur);  // NOLINT
    }
  }
}

auto ArenaTempAllocator::Free(void* inAddress, JPH::uint inSize) -> void {
  if (inAddress == nullptr || base_ == 0) {
    return;
  }
  const std::size_t need =
      AlignUp(static_cast<std::size_t>(inSize), JPH_RVECTOR_ALIGNMENT);
  // LIFO discipline: only rewind when the freed block is the stack top.
  // Compare-and-swap loop prevents concurrent frees from racing or corrupting top_.
  const auto addr = reinterpret_cast<std::uintptr_t>(inAddress);  // NOLINT
  for (;;) {
    std::size_t cur = top_.load(std::memory_order_relaxed);
    if (cur < need || addr != base_ + cur - need) {
      return;  // not current top (non-LIFO free or out-of-order)
    }
    if (top_.compare_exchange_weak(cur, cur - need, std::memory_order_relaxed,
                                   std::memory_order_relaxed)) {
      return;
    }
  }
}

}  // namespace engine::physics

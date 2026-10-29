// temp_allocator_arena.hpp — M4-01: Jolt TempAllocator on PageArenaAllocator.
//
// Jolt calls Allocate/Free as a strict LIFO stack (job dependencies
// guarantee reverse-order frees). Back it with the engine's fixed
// PageArenaAllocator reservation: init() carves one aligned block out of
// the caller-provided arena (arena must outlive this object); Allocate
// bumps atomically, Free rewinds. LIFO order mirrors upstream
// TempAllocatorImpl (which aborts on wrong-order free — we rewind-only,
// matching its stack discipline without the abort). No malloc/new on the
// frame path after init: the block is carved once, bump/rewind is
// lock-free. Free-before-base validation (address check) guards against
// foreign-pointer frees corrupting the top.
#pragma once

#include "engine/core/engine_core_export.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "engine/core/page_arena_allocator.hpp"

// WARNING: see job_system_enki.hpp — <Jolt/...> includes live only in the
// adapter TUs via the wrapper interface (SYSTEM + -Wno-conversion).
#include <Jolt/Jolt.h>
#include <Jolt/Core/TempAllocator.h>

namespace engine::physics {

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4251 4275)  // STL members + Jolt non-dll bases: same-toolchain, OBJECT lib linked into engine_core.
#endif

class ENGINE_CORE_API ArenaTempAllocator final : public JPH::TempAllocator {
 public:
  ArenaTempAllocator() noexcept = default;
  ArenaTempAllocator(const ArenaTempAllocator&) = delete;
  auto operator=(const ArenaTempAllocator&) -> ArenaTempAllocator& = delete;
  ~ArenaTempAllocator() noexcept override;

  // Carve `size` bytes (JPH_RVECTOR_ALIGNMENT-aligned) out of `arena`.
  // Idempotent destroy-first, single-shot otherwise (arena init is also
  // single-shot — a second arena init fails, so re-init here re-carves
  // from the same arena only after an explicit destroy()). Returns false
  // (leaving the object empty) on zero size, null arena, or exhaustion.
  // The arena must outlive this object (carved pointer is not owned).
  auto init(core::PageArenaAllocator* arena, std::size_t size) noexcept
      -> bool;
  auto destroy() noexcept -> void;

  auto Allocate(JPH::uint inSize) -> void* override;
  auto Free(void* inAddress, JPH::uint inSize) -> void override;

  [[nodiscard]] auto used() const noexcept -> std::size_t {
    return top_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] auto capacity() const noexcept -> std::size_t { return size_; }

  auto reset() noexcept -> void { top_.store(0, std::memory_order_relaxed); }

 private:
  static auto AlignUp(std::size_t v, std::size_t align) noexcept -> std::size_t {
    return (v + align - 1U) & ~(align - 1U);
  }

  std::uintptr_t base_{0};      // carved block (owned by the arena, not us)
  std::size_t size_{0};         // carved block size
  std::atomic<std::size_t> top_{0};  // bump offset (LIFO stack top)
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

}  // namespace engine::physics

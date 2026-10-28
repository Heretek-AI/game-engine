// page_arena_allocator.hpp — M1-02: OS-virtual-memory page arena.
//
// A grow-in-place arena that reserves a large virtual address range up
// front and commits physical pages lazily as `used` grows. Backing store:
//   - Windows: VirtualAlloc(MEM_RESERVE) + VirtualAlloc(MEM_COMMIT).
//   - POSIX:   mmap(PROT_NONE) + mprotect(PROT_READ|PROT_WRITE).
//
// Lifetime model: the arena is created once at startup (component tables,
// entity index), grows monotonically during the run, and is destroyed at
// shutdown. There is intentionally NO per-block free: the "free" half of
// the frame path is the LinearFrameAllocator; this arena is for
// long-lived ECS storage. Individual deallocation is a no-op (memory is
// reclaimed wholesale in the destructor).
//
// Threading: allocate() is safe from any number of worker threads
// (lock-free CAS loop). Commit (page fault handling / mprotect path) is
// serialized with a lightweight spin mutex so two threads never commit
// the same page twice.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "engine/core/engine_core_export.hpp"

namespace engine::core {

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)  // atomics/mutex are private, same-toolchain
#endif                           // builds both sides; no cross-compiler ABI.
class ENGINE_CORE_API PageArenaAllocator {
 public:
  PageArenaAllocator() noexcept = default;
  PageArenaAllocator(const PageArenaAllocator&) = delete;
  auto operator=(const PageArenaAllocator&) -> PageArenaAllocator& = delete;
  ~PageArenaAllocator() noexcept;

  // Reserve `reserve_bytes` of address space and pre-commit
  // `commit_bytes` of it. Both are rounded up to the OS page size.
  // Returns false (leaving the object empty) when the OS refuses.
  // May be called at most once; a second call fails and is a no-op.
  auto init(std::size_t reserve_bytes, std::size_t commit_bytes) noexcept
      -> bool;

  // Bump-allocate `size` bytes with `alignment` (power of two).
  // Commits more pages on demand. Returns nullptr on exhaustion.
  // Lock-free fast path; commit path takes an internal spin mutex.
  [[nodiscard]] auto allocate(std::size_t size,
                              std::size_t alignment) noexcept -> void*;

  // Individual free is a no-op by design (arena lifetime; see above).
  // Provided so generic allocator adapters compile unchanged.
  static auto deallocate(void* /*ptr*/, std::size_t /*size*/) noexcept -> void {
    // Intentionally empty: arena frees wholesale in destroy(); per-block
    // free is a no-op so generic allocator adapters compile unchanged.
  }

  // Release the whole reservation back to the OS. Idempotent; also
  // performed by the destructor. NOT thread-safe with allocate().
  auto destroy() noexcept -> void;

  [[nodiscard]] auto base() const noexcept -> void* { return base_; }
  [[nodiscard]] auto reserved() const noexcept -> std::size_t {
    return reserved_;
  }
  [[nodiscard]] auto committed() const noexcept -> std::size_t {
    return committed_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] auto used() const noexcept -> std::size_t {
    return used_.load(std::memory_order_relaxed);
  }

 private:
  auto CommitPages(std::size_t need_committed) noexcept -> bool;

  void* base_{nullptr};                       // Reservation start (page aligned).
  std::size_t reserved_{0};                   // Total reserved bytes.
  std::atomic<std::size_t> committed_{0};     // Bytes committed so far.
  std::atomic<std::size_t> used_{0};          // Bytes handed out so far.
  std::mutex commit_mutex_;                   // Serializes lazy commit.
  bool initialized_{false};
};
#ifdef _MSC_VER
#pragma warning(pop)  // C4251 scoped to PageArenaAllocator.
#endif

}  // namespace engine::core

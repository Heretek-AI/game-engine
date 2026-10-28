// page_arena_allocator.cpp — reserve/commit virtual-memory arena.
#include "engine/core/page_arena_allocator.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace engine::core {
namespace {

auto OsPageSize() noexcept -> std::size_t {
#if defined(_WIN32)
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  return static_cast<std::size_t>(si.dwPageSize != 0U ? si.dwPageSize : 4096U);
#else
  const long ps = sysconf(_SC_PAGESIZE);  // NOLINT(runtime/int)
  return ps > 0 ? static_cast<std::size_t>(ps) : std::size_t{4096};
#endif
}

auto RoundUpPage(std::size_t v, std::size_t page) noexcept -> std::size_t {
  const std::size_t mask = page - 1U;
  return (v + mask) & ~mask;
}

auto ReserveAddressSpace(std::size_t bytes) noexcept -> void* {
#if defined(_WIN32)
  return VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS);
#else
  void* p = ::mmap(nullptr, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS,
                   /*fd=*/-1, /*offset=*/0);
  return p == MAP_FAILED ? nullptr : p;
#endif
}

auto ReleaseAddressSpace(void* base, std::size_t bytes) noexcept -> void {
#if defined(_WIN32)
  (void)bytes;  // MEM_RELEASE frees the whole reservation.
  VirtualFree(base, 0, MEM_RELEASE);
#else
  ::munmap(base, bytes);
#endif
}

auto CommitMemory(void* addr, std::size_t bytes) noexcept -> bool {
#if defined(_WIN32)
  return VirtualAlloc(addr, bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
  return ::mprotect(addr, bytes, PROT_READ | PROT_WRITE) == 0;
#endif
}

}  // namespace

PageArenaAllocator::~PageArenaAllocator() noexcept { destroy(); }

auto PageArenaAllocator::init(std::size_t reserve_bytes,
                              std::size_t commit_bytes) noexcept -> bool {
  if (initialized_) {
    return false;
  }
  if (reserve_bytes == 0) {
    return false;
  }
  const std::size_t page = OsPageSize();
  reserve_bytes = RoundUpPage(reserve_bytes, page);
  commit_bytes = RoundUpPage(commit_bytes, page);
  if (commit_bytes > reserve_bytes) {
    commit_bytes = reserve_bytes;
  }
  void* base = ReserveAddressSpace(reserve_bytes);
  if (base == nullptr) {
    return false;
  }
  if (commit_bytes != 0U && !CommitMemory(base, commit_bytes)) {
    ReleaseAddressSpace(base, reserve_bytes);
    return false;
  }
  base_ = base;
  reserved_ = reserve_bytes;
  committed_.store(commit_bytes, std::memory_order_relaxed);
  used_.store(0, std::memory_order_relaxed);
  initialized_ = true;
  return true;
}

auto PageArenaAllocator::CommitPages(std::size_t need_committed) noexcept
    -> bool {
  std::lock_guard<std::mutex> lock(commit_mutex_);
  std::size_t have = committed_.load(std::memory_order_relaxed);
  if (need_committed <= have) {
    return true;  // Another thread committed while we waited.
  }
  if (need_committed > reserved_) {
    return false;
  }
  auto* base_byte = static_cast<unsigned char*>(base_);
  if (!CommitMemory(base_byte + have, need_committed - have)) {
    return false;
  }
  committed_.store(need_committed, std::memory_order_relaxed);
  return true;
}

auto PageArenaAllocator::allocate(std::size_t size,
                                  std::size_t alignment) noexcept -> void* {
  if (!initialized_ || size == 0) {
    return nullptr;
  }
  if (alignment == 0 || (alignment & (alignment - 1U)) != 0U) {
    alignment = alignof(void*);
  }
  const std::size_t page = OsPageSize();
  std::size_t cur = used_.load(std::memory_order_relaxed);
  for (;;) {
    const std::uintptr_t base_addr =
        reinterpret_cast<std::uintptr_t>(base_);  // NOLINT
    const std::uintptr_t cur_addr = base_addr + cur;
    const std::uintptr_t mask = static_cast<std::uintptr_t>(alignment - 1U);
    const std::uintptr_t aligned_addr = (cur_addr + mask) & ~mask;
    const std::size_t next =
        static_cast<std::size_t>(aligned_addr - base_addr) + size;
    if (next > reserved_ || next < cur) {
      return nullptr;
    }
    // Ensure backing pages are committed before publishing the bump.
    const std::size_t need_commit = RoundUpPage(next, page);
    std::size_t have = committed_.load(std::memory_order_acquire);
    if (need_commit > have && !CommitPages(need_commit)) {
      return nullptr;
    }
    if (used_.compare_exchange_weak(cur, next, std::memory_order_relaxed,
                                    std::memory_order_relaxed)) {
      return reinterpret_cast<void*>(aligned_addr);  // NOLINT
    }
  }
}

auto PageArenaAllocator::destroy() noexcept -> void {
  if (base_ != nullptr) {
    ReleaseAddressSpace(base_, reserved_);
    base_ = nullptr;
  }
  reserved_ = 0;
  committed_.store(0, std::memory_order_relaxed);
  used_.store(0, std::memory_order_relaxed);
  initialized_ = false;
}

}  // namespace engine::core

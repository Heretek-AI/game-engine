// aligned_flecs_hooks.cpp — 64B-aligned Flecs OS hooks; see header.
#include "engine/core/aligned_flecs_hooks.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "engine/core/linear_frame_allocator.hpp"  // kCacheLineSize
#include "flecs.h"

namespace engine::core {
namespace {

constexpr std::size_t kLine = kCacheLineSize;
static_assert(sizeof(AlignedHeader) == kLine,
              "AlignedHeader must be exactly one cache line; padding drifted.");

std::atomic<PageArenaAllocator*> g_arena{nullptr};

auto AlignUpPtr(std::uintptr_t v, std::size_t alignment) noexcept
    -> std::uintptr_t {
  const auto mask = static_cast<std::uintptr_t>(alignment - 1U);
  return (v + mask) & ~mask;
}

// Backend allocation: arena first, aligned heap as fallback.
struct BackendBlock {
  void* raw;          // Pointer the backend owns (free it / abandon if arena).
  void* aligned_raw;  // Aligned base the header is written at.
  std::size_t raw_size;
};

auto BackendAlloc(std::size_t need) noexcept -> BackendBlock {
  if (auto* arena = g_arena.load(std::memory_order_acquire)) {
    // Arena bumps are already suitably aligned for our header math when we
    // ask for 64B alignment; fall back to heap if the arena is exhausted.
    if (void* p = arena->allocate(need, kLine)) {
      return BackendBlock{nullptr, p, need};
    }
  }
#if defined(_WIN32)
  void* p = _aligned_malloc(need, kLine);
  return BackendBlock{p, p, p != nullptr ? need : 0};
#else
  void* p = std::aligned_alloc(kLine, need);
  return BackendBlock{p, p, p != nullptr ? need : 0};
#endif
}

auto BackendFree(BackendBlock blk) noexcept -> void {
  if (blk.raw == nullptr) {
    return;  // Arena-owned: reclaimed wholesale, nothing to do.
  }
#if defined(_WIN32)
  _aligned_free(blk.raw);
#else
  std::free(blk.raw);
#endif
}

// Wrap a backend block with a cache-line header + aligned payload.
auto WrapBlock(BackendBlock blk, std::size_t payload_size) noexcept -> void* {
  if (blk.aligned_raw == nullptr) {
    return nullptr;
  }
  const auto base =
      reinterpret_cast<std::uintptr_t>(blk.aligned_raw);  // NOLINT
  const auto hdr_addr = AlignUpPtr(base, kLine);
  auto* hdr = reinterpret_cast<AlignedHeader*>(hdr_addr);  // NOLINT
  hdr->raw = blk.raw;
  hdr->raw_size = blk.raw_size;
  hdr->payload_size = payload_size;
  // NOLINTNEXTLINE: payload immediately follows the 64B header.
  return reinterpret_cast<void*>(hdr_addr + kLine);
}

auto HeaderOf(void* payload) noexcept -> AlignedHeader* {
  const auto addr =
      reinterpret_cast<std::uintptr_t>(payload);  // NOLINT
  // NOLINTNEXTLINE: header sits exactly one line before the payload.
  return reinterpret_cast<AlignedHeader*>(addr - kLine);
}

auto HookMalloc(ecs_size_t size) -> void* {
  if (size <= 0) {
    return nullptr;
  }
  const auto payload = static_cast<std::size_t>(size);
  // need = header line + payload, rounded so aligned_alloc stays valid.
  const std::size_t need = ((payload + kLine + kLine - 1U) / kLine) * kLine;
  return WrapBlock(BackendAlloc(need), payload);
}

auto HookCalloc(ecs_size_t size) -> void* {
  void* p = HookMalloc(size);
  if (p != nullptr) {
    std::memset(p, 0, static_cast<std::size_t>(size));
  }
  return p;
}

auto HookFree(void* ptr) -> void {
  if (ptr == nullptr) {
    return;
  }
  AlignedHeader* hdr = HeaderOf(ptr);
  BackendFree(BackendBlock{hdr->raw, nullptr, hdr->raw_size});
}

auto HookRealloc(void* ptr, ecs_size_t size) -> void* {
  if (ptr == nullptr) {
    return HookMalloc(size);
  }
  if (size <= 0) {
    HookFree(ptr);
    return nullptr;
  }
  AlignedHeader* hdr = HeaderOf(ptr);
  const auto want = static_cast<std::size_t>(size);
  if (want <= hdr->payload_size) {
    hdr->payload_size = want;  // Shrinking: keep the block, note new size.
    return ptr;
  }
  void* grown = HookMalloc(size);
  if (grown == nullptr) {
    return nullptr;
  }
  std::memcpy(grown, ptr, hdr->payload_size);
  HookFree(ptr);
  return grown;
}

}  // namespace

auto InstallAlignedFlecsHooks(PageArenaAllocator* arena) noexcept -> void {
  g_arena.store(arena, std::memory_order_release);
  // ecs_os_set_api is one-shot: it only applies before the first init, so
  // fetch defaults first (populates the whole table incl. threading/log),
  // then overwrite just the heap hooks. Order matters — merge AFTER
  // defaults, then call set_api exactly once per process state.
  ecs_os_set_api_defaults();
  ecs_os_api_t api = ecs_os_get_api();
  api.malloc_ = HookMalloc;
  api.calloc_ = HookCalloc;
  api.realloc_ = HookRealloc;
  api.free_ = HookFree;
  ecs_os_set_api(&api);
}

auto EnsureAlignedFlecsHooks() noexcept -> bool {
  if (ecs_os_get_api().malloc_ != HookMalloc) {
    InstallAlignedFlecsHooks(nullptr);
  }
  return ecs_os_get_api().malloc_ == HookMalloc;
}

auto RemoveAlignedFlecsHooks() noexcept -> void {
  g_arena.store(nullptr, std::memory_order_release);
  // NOTE: ecs_os_set_api() is one-shot in Flecs (later calls are ignored
  // once initialized), so a true "restore" is only possible before the
  // first ecs_init/ecs_os_init. We intentionally do NOT call
  // ecs_os_set_api_defaults() here: at shutdown-time that would be a
  // silent no-op while implying hooks were removed. Callers must destroy
  // worlds created under our hooks before process teardown instead.
}

auto AlignedHooksArena() noexcept -> PageArenaAllocator* {
  return g_arena.load(std::memory_order_acquire);
}

}  // namespace engine::core

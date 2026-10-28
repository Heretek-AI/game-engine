// aligned_flecs_hooks.hpp — M1-02: 64-byte-aligned Flecs OS hooks.
//
// Flecs routes ALL of its dynamic memory through ecs_os_api_t
// (malloc_/calloc_/realloc_/free_). Column storage for tables, the entity
// index, query caches — everything lands in those four hooks (the built-in
// block allocator itself pages through them, so hooking at this level
// covers both FLECS_USE_OS_ALLOC and default builds).
//
// This header installs hooks that:
//   1. Round every allocation up to a 16-byte header + payload and return
//      a payload pointer aligned to kCacheLineSize (64). The header stores
//      the raw pointer + payload size so free()/realloc() can recover it.
//   2. Back allocations out of a caller-provided PageArenaAllocator when
//      one is installed (zero malloc on the frame path); otherwise fall
//      back to ::aligned_alloc/_aligned_malloc so behavior is correct in
//      unit tests and before the arena exists.
//
// Layout per allocation (all addresses 64B-aligned except raw):
//   raw = base_heap_ptr (whatever the backend returned)
//   hdr = AlignUp(raw, 64)          <- AlignedHeader lives here
//   payload = hdr + 64              <- returned pointer, 64B-aligned
// The 64-byte header doubles as a full cache-line guard: the bookkeeping
// bytes never share a line with user data, so allocator metadata writes
// cannot contend with hot component iteration.
//
// Usage (call once, before ecs_init):
//   engine::core::InstallAlignedFlecsHooks(&my_arena);  // or nullptr
#pragma once

#include <cstddef>

#include "engine/core/page_arena_allocator.hpp"

namespace engine::core {

// Install the aligned hooks into the global Flecs OS API. `arena` may be
// null (plain aligned-heap mode) or point to a live PageArenaAllocator
// that outlives all Flecs worlds using it. Not thread-safe: call once at
// startup before any world exists. Safe to call repeatedly with the same
// arena (subsequent calls re-apply the same hooks).
auto InstallAlignedFlecsHooks(PageArenaAllocator* arena) noexcept -> void;

// Remove our hooks and restore stock Flecs defaults. Worlds allocated
// through our hooks must be destroyed BEFORE calling this.
auto RemoveAlignedFlecsHooks() noexcept -> void;

// The live arena currently backing the hooks (null = heap mode). For
// diagnostics and tests.
auto AlignedHooksArena() noexcept -> PageArenaAllocator*;

// Header placed on the cache line immediately preceding each payload.
// Deliberately padded to exactly one cache line; keep it that way
// (static_assert in the TU).
struct AlignedHeader {
  void* raw;              // Pointer to release (heap mode) or nullptr (arena).
  std::size_t raw_size;   // Bytes taken from the backend (for telemetry).
  std::size_t payload_size;  // Bytes requested by the caller.
  std::size_t padding[5];    // Pad to 64 bytes total (8+8+8+40).
};

}  // namespace engine::core

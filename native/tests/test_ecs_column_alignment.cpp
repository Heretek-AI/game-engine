// test_ecs_column_alignment.cpp — P0-05 acceptance (native CTest).
//
// LocalTransform/WorldTransform are alignas(64), and Flecs allocates
// component columns through ecs_os_malloc/realloc. EcsWorld::construct()
// installs the 64-byte-aligned Flecs heap hooks first, so every column of
// the (Local, World, Velocity) archetype must start on a 64-byte line.
// Creates 1, 7 and 10 000 entities (each count grows the same table, so
// realloc paths are covered too) and checks the Local column base, every
// row, and EcsWorld::bulk_create on top.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "engine/ecs/ecs_world.hpp"
#include "engine/ecs/transform.hpp"
#include "flecs.h"

namespace {

auto failures = 0;

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}

auto Aligned(const void* p, std::size_t alignment) -> bool {
  return p != nullptr &&
         reinterpret_cast<std::uintptr_t>(p) % alignment == 0;  // NOLINT
}

// Every row of `ids[0..count)` has a LocalTransform on a 64B boundary.
auto AllRowsAligned(ecs_world_t* w, ecs_entity_t local,
                    const ecs_entity_t* ids, std::int32_t count) -> bool {
  for (std::int32_t i = 0; i < count; ++i) {
    if (!Aligned(ecs_get_id(w, ids[i], local),
                 alignof(engine::ecs::LocalTransform))) {
      return false;
    }
  }
  return true;
}

}  // namespace

auto main() -> int {
  using engine::ecs::LocalTransform;
  static_assert(alignof(LocalTransform) == 64, "P0-05 expects 64B Local");

  engine::ecs::EcsWorld world;
  check(world.construct(), "ecs: construct");
  check(world.aligned_hooks(), "ecs: aligned Flecs heap hooks live");
  ecs_world_t* w = world.raw();
  if (w == nullptr) {
    return 1;
  }

  // Velocity: 4 floats, same shape the C ABI registers.
  ecs_entity_desc_t ved{};
  ved.name = "Velocity";
  ecs_component_desc_t vcd{};
  vcd.entity = ecs_entity_init(w, &ved);
  vcd.type.size = static_cast<ecs_size_t>(sizeof(float) * 4);
  vcd.type.alignment = static_cast<ecs_size_t>(alignof(float) * 4);
  const ecs_entity_t vel = ecs_component_init(w, &vcd);
  check(vel != 0, "ecs: Velocity registered");

  const auto local = static_cast<ecs_entity_t>(world.local_id());
  const auto wt = static_cast<ecs_entity_t>(world.world_id());
  constexpr std::array<std::int32_t, 3> kCounts{1, 7, 10000};
  bool all_ok = true;
  for (const std::int32_t n : kCounts) {
    ecs_bulk_desc_t bd{};
    bd.count = n;
    bd.ids[0] = local;
    bd.ids[1] = wt;
    bd.ids[2] = vel;
    const ecs_entity_t* ids = ecs_bulk_init(w, &bd);
    const bool ok = ids != nullptr && world.LocalColumnAligned(ids[0]) &&
                    AllRowsAligned(w, local, ids, n);
    std::printf("[%s] ecs: %d entities, Local column 64B-aligned\n",
                ok ? "PASS" : "FAIL", n);
    if (!ok) {
      ++failures;
      all_ok = false;
    }
  }
  check(all_ok, "ecs: Local column 64B-aligned");

  // EcsWorld::bulk_create writes identity through LocalTransform* (the
  // store GCC 13 vectorizes to aligned vmovaps): it must succeed.
  check(world.bulk_create(10000) == 10000, "ecs: bulk_create 10000");

  world.destroy();
  if (failures == 0) {
    std::printf("ECS_COLUMN_ALIGNMENT_OK\n");
  }
  return failures == 0 ? 0 : 1;
}

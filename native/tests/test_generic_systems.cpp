// test_generic_systems.cpp — ABI 4.2 generic component/system tail.
//
// Drives the table entries exactly as generated C# does: register two
// user components by name, spawn entities, register a (Mover inout,
// Health in) system, run it, and read rows back. Also pins the error
// contract (size mismatch returns 0 instead of aborting, in-run guard).
// Also pins CopySystemName truncation (system name slots are 64 bytes).
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine/bridge/engine_abi.h"
#include "name_copy.hpp"

namespace {

auto check(bool ok, const char* name) -> int {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  return ok ? 0 : 1;
}

// Mirrors of the C# fixture structs (sequential, 4-byte aligned).
struct Mover {
  float speed;
  float x;
  float y;
};
struct Health {
  float value;
};
static_assert(sizeof(Mover) == 12, "Mover fixture is 12 bytes");

const EngineApiTable* g_api = nullptr;
EngineWorld* g_world = nullptr;
int32_t g_calls = 0;
int32_t g_rows = 0;
int32_t g_in_run_create = -1;
int32_t g_bad_sizes = 0;

// x += health * dt; also probes the in-run guard once.
void MoveByHealth(const EngineChunkColumns* view, float dt) {
  ++g_calls;
  g_rows += view->count;
  if (view->term_count != 2 || view->sizes[0] != 12 || view->sizes[1] != 4) {
    ++g_bad_sizes;
    return;
  }
  auto* movers = static_cast<Mover*>(view->columns[0]);
  const auto* health = static_cast<const Health*>(view->columns[1]);
  for (int32_t i = 0; i < view->count; ++i) {
    movers[i].x += health[i].value * dt;
  }
  if (g_in_run_create < 0) {
    g_in_run_create = static_cast<int32_t>(g_api->entity_create(g_world));
  }
}

// Empty, nullptr, exact fit, one over, and far over the 64-byte slot.
auto CheckCopySystemName() -> int {
  int failures = 0;
  char dst[64];
  std::memset(dst, 'x', sizeof(dst));
  engine::bridge::CopySystemName(dst, "");
  failures += check(dst[0] == '\0', "CopySystemName empty name");
  std::memset(dst, 'x', sizeof(dst));
  engine::bridge::CopySystemName(dst, nullptr);
  failures += check(dst[0] == '\0', "CopySystemName nullptr yields empty");

  char src[301];
  std::memset(src, 'a', sizeof(src) - 1);
  src[300] = '\0';
  src[63] = '\0';  // 63 chars: exactly fills the slot
  engine::bridge::CopySystemName(dst, src);
  failures += check(std::strlen(dst) == 63 && dst[62] == 'a', "CopySystemName exact fit");
  src[63] = 'a';
  src[64] = '\0';  // 64 chars: one over
  engine::bridge::CopySystemName(dst, src);
  failures += check(std::strlen(dst) == 63 && dst[63] == '\0', "CopySystemName truncates 64");
  src[64] = 'a';  // 300 chars
  engine::bridge::CopySystemName(dst, src);
  failures += check(std::strlen(dst) == 63 && std::memcmp(dst, src, 63) == 0,
                    "CopySystemName truncates 300");
  return failures;
}

}  // namespace

auto main() -> int {
  int failures = CheckCopySystemName();
  g_api = engine_get_api_table();
  failures += check(g_api != nullptr, "api table");
  if (g_api == nullptr) {
    return 1;
  }
  failures += check(g_api->component_register != nullptr && g_api->systems_run != nullptr,
                    "generic tail entries set");
  g_world = g_api->world_create();
  failures += check(g_world != nullptr, "world_create");
  if (g_world == nullptr) {
    return 1;
  }

  // --- components ---------------------------------------------------------
  const uint64_t mover = g_api->component_register(g_world, "Game.Mover", 12, 4);
  const uint64_t mover_again = g_api->component_register(g_world, "Game.Mover", 12, 4);
  const uint64_t health = g_api->component_register(g_world, "Game.Health", 4, 4);
  failures += check(mover != 0, "register Game.Mover");
  failures += check(mover_again == mover, "re-register same layout returns same id");
  failures += check(health != 0 && health != mover, "register Game.Health");
  failures += check(g_api->component_register(g_world, "Game.Mover", 16, 4) == 0,
                    "re-register with different size returns 0 (no abort)");
  failures += check(g_api->component_register(g_world, "", 4, 4) == 0, "empty name rejected");
  failures += check(g_api->component_register(g_world, "Game.Bad", 4, 3) == 0,
                    "non power-of-two alignment rejected");
  failures += check(g_api->component_register(g_world, "Game.Bad", 6, 4) == 0,
                    "size not a multiple of alignment rejected");

  // --- entities -----------------------------------------------------------
  uint64_t ents[6] = {};
  for (int i = 0; i < 6; ++i) {
    ents[i] = g_api->entity_create(g_world);
    const Mover m{static_cast<float>(i), 0.0F, 0.0F};
    failures += check(g_api->entity_set(g_world, ents[i], mover, &m, sizeof(m)) == 0,
                      "entity_set Mover");
    if (i % 2 == 0) {
      const Health h{10.0F * static_cast<float>(i + 1)};
      failures += check(g_api->entity_set(g_world, ents[i], health, &h, sizeof(h)) == 0,
                        "entity_set Health (even rows)");
    }
  }
  const Mover probe{};
  failures += check(g_api->entity_set(g_world, ents[0], mover, &probe, 16) ==
                        ENGINE_ABI_ERR_SIZE_MISMATCH,
                    "entity_set wrong size -> SIZE_MISMATCH");
  Health no_health{};
  failures += check(g_api->entity_get(g_world, ents[1], health, &no_health, sizeof(no_health)) ==
                        ENGINE_ABI_ERR_NOT_FOUND,
                    "entity_get missing component -> NOT_FOUND");

  // --- system -------------------------------------------------------------
  const uint64_t ids[2] = {mover, health};
  const uint32_t access[2] = {ENGINE_ABI_ACCESS_INOUT, ENGINE_ABI_ACCESS_IN};
  const int32_t sys = g_api->system_register(g_world, "MoveByHealth", ids, access, 2,
                                             &MoveByHealth, nullptr);
  failures += check(sys == 0, "system_register returns index 0");
  const uint64_t dup_ids[2] = {mover, mover};
  failures += check(g_api->system_register(g_world, "Dup", dup_ids, access, 2, &MoveByHealth,
                                           nullptr) == ENGINE_ABI_ERR_BAD_ARG,
                    "duplicate term rejected");
  failures += check(g_api->system_register(g_world, "Zero", ids, access, 0, &MoveByHealth,
                                           nullptr) == ENGINE_ABI_ERR_BAD_ARG,
                    "zero terms rejected");

  const FrameDispatch dispatch{0.5F};
  failures += check(g_api->systems_run(g_world, &dispatch) == 0, "systems_run ok");
  failures += check(g_rows == 3, "system saw exactly the 3 rows with Health");
  failures += check(g_bad_sizes == 0, "view carried term_count 2 and sizes 12/4");
  failures += check(g_in_run_create == 0, "entity_create inside a callback returns 0");

  for (int i = 0; i < 6; ++i) {
    Mover got{};
    failures += check(g_api->entity_get(g_world, ents[i], mover, &got, sizeof(got)) == 0,
                      "entity_get Mover");
    const float want_x = (i % 2 == 0) ? 10.0F * static_cast<float>(i + 1) * 0.5F : 0.0F;
    char label[96];
    std::snprintf(label, sizeof(label), "row %d: speed=%.1f x=%.1f (want speed=%d x=%.1f)", i,
                  static_cast<double>(got.speed), static_cast<double>(got.x), i,
                  static_cast<double>(want_x));
    failures += check(got.speed == static_cast<float>(i) && got.x == want_x, label);
  }

  failures += check(g_api->systems_run(g_world, nullptr) == 0, "systems_run with NULL dispatch");
  g_api->world_destroy(g_world);
  std::printf(failures == 0 ? "generic_systems: all checks passed\n"
                            : "generic_systems: %d FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}

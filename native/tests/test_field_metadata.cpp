// test_field_metadata.cpp — ABI 4.4 (P3-04) acceptance: field metadata
// read-back + generator-shaped Health fixture (float Current @0, int Max
// @4). Covers: describe stores rows, field_count/field_at read them back,
// unknown components return NOT_FOUND, bad offsets return BAD_ARG, and
// the built-in LocalTransform describes itself (position vec3 @0,
// rotation quat @16, scale vec3 @32).
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine/bridge/engine_abi.h"

namespace {

auto check(bool ok, const char* name) -> int {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  return ok ? 0 : 1;
}

struct Health {
  float current;
  int32_t max;
};
static_assert(sizeof(Health) == 8, "Health fixture is 8 bytes");
static_assert(offsetof(Health, current) == 0, "Current @0");
static_assert(offsetof(Health, max) == 4, "Max @4");

}  // namespace

auto main() -> int {
  int failures = 0;
  const EngineApiTable* api = engine_get_api_table();
  failures += check(api != nullptr, "field: api table");
  if (api == nullptr) {
    return 1;
  }
  failures += check(api->component_describe != nullptr, "field: describe set");
  failures +=
      check(api->component_field_count != nullptr, "field: count set");
  failures += check(api->component_field_at != nullptr, "field: at set");
  if (api->component_describe == nullptr ||
      api->component_field_count == nullptr ||
      api->component_field_at == nullptr) {
    return 1;
  }

  EngineWorld* world = api->world_create();
  failures += check(world != nullptr, "field: world_create");
  if (world == nullptr) {
    return 1;
  }

  // Generator-shaped registration: Health (float Current, int Max).
  const uint64_t health =
      api->component_register(world, "Health", sizeof(Health), 4);
  failures += check(health != 0, "field: register Health (8B, align 4)");
  const EngineFieldDesc kFields[2] = {{"Current", 0, 0}, {"Max", 4, 1}};
  failures += check(api->component_describe(world, "Health", kFields, 2) == 0,
                    "field: describe Health 2 rows");
  uint32_t count = 0;
  failures += check(api->component_field_count(world, "Health", &count) == 0 &&
                        count == 2,
                    "field: count Health == 2");
  char name[256]{};
  uint32_t off = 0;
  uint32_t type = 0;
  failures += check(api->component_field_at(world, "Health", 0, name,
                                            sizeof(name), &off, &type) == 0 &&
                        std::strcmp(name, "Current") == 0 && off == 0 &&
                        type == ENGINE_ABI_FIELD_F32,
                    "field: row 0 Current f32 @0");
  failures += check(api->component_field_at(world, "Health", 1, name,
                                            sizeof(name), &off, &type) == 0 &&
                        std::strcmp(name, "Max") == 0 && off == 4 &&
                        type == ENGINE_ABI_FIELD_I32,
                    "field: row 1 Max i32 @4");

  // Unknown component: describe + count return NOT_FOUND (-4).
  failures += check(api->component_describe(world, "Ghost", kFields, 2) ==
                        ENGINE_ABI_ERR_NOT_FOUND,
                    "field: describe unknown -> NOT_FOUND");
  failures += check(api->component_field_count(world, "Ghost", &count) ==
                        ENGINE_ABI_ERR_NOT_FOUND,
                    "field: count unknown -> NOT_FOUND");

  // Bad offset (past registered size): BAD_ARG (-2).
  const EngineFieldDesc kBad[1] = {{"X", 99, 0}};
  failures += check(api->component_describe(world, "Health", kBad, 1) ==
                        ENGINE_ABI_ERR_BAD_ARG,
                    "field: bad offset -> BAD_ARG");

  // Built-ins describe themselves: LocalTransform has 3 rows.
  uint32_t lt_count = 0;
  const int32_t lt_rc =
      api->component_field_count(world, "LocalTransform", &lt_count);
  failures += check(lt_rc == 0 && lt_count == 3,
                    "field: built-in LocalTransform 3 rows");
  failures += check(api->component_field_at(world, "LocalTransform", 0, name,
                                            sizeof(name), &off, &type) == 0 &&
                        std::strcmp(name, "position") == 0 && off == 0 &&
                        type == ENGINE_ABI_FIELD_VEC3,
                    "field: builtin row 0 position vec3 @0");
  failures += check(api->component_field_at(world, "LocalTransform", 1, name,
                                            sizeof(name), &off, &type) == 0 &&
                        std::strcmp(name, "rotation") == 0 && off == 16 &&
                        type == ENGINE_ABI_FIELD_QUAT,
                    "field: builtin row 1 rotation quat @16");

  api->world_destroy(world);
  std::printf(failures == 0 ? "field_metadata: all checks passed\n"
                            : "field_metadata: %d FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}

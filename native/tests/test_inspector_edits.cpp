// test_inspector_edits.cpp — P3-05 acceptance: float edit via the
// panel-edit path (entity_get/entity_set over MeshRenderer.base_color)
// plus undo-restore and entity_remove round-trip. Mirrors what
// SetFieldCommand does in managed code: read the whole component image,
// patch one lane, write back, undo by replaying the before-image.
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine/bridge/engine_abi.h"
#include "engine/ecs/ecs_render_components.hpp"

namespace {

auto check(bool ok, const char* name) -> int {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  return ok ? 0 : 1;
}

}  // namespace

auto main() -> int {
  int failures = 0;
  const EngineApiTable* api = engine_get_api_table();
  failures += check(api != nullptr, "inspector: api table");
  if (api == nullptr) {
    return 1;
  }
  failures += check(api->entity_remove != nullptr, "inspector: entity_remove set");
  if (api->entity_remove == nullptr) {
    return 1;
  }

  EngineWorld* world = api->world_create();
  failures += check(world != nullptr, "inspector: world_create");
  if (world == nullptr) {
    return 1;
  }

  const uint64_t mesh = api->component_register(
      world, "Engine.Runtime.MeshRenderer", sizeof(engine::ecs::MeshRenderer),
      alignof(engine::ecs::MeshRenderer));
  failures += check(mesh != 0, "inspector: MeshRenderer registered");
  const uint64_t ent = api->entity_create(world);
  failures += check(ent != 0, "inspector: entity created");

  // Panel edit: write a zeroed image (Add Component), then patch
  // base_color[0] to 2.5 and read it back.
  engine::ecs::MeshRenderer zero{};
  failures +=
      check(api->entity_set(world, ent, mesh, &zero, sizeof(zero)) == 0,
            "inspector: add MeshRenderer");
  engine::ecs::MeshRenderer before{};
  failures +=
      check(api->entity_get(world, ent, mesh, &before, sizeof(before)) == 0,
            "inspector: read before-image");
  engine::ecs::MeshRenderer after = before;
  after.base_color[0] = 2.5F;
  failures +=
      check(api->entity_set(world, ent, mesh, &after, sizeof(after)) == 0,
            "inspector: panel edit base_color[0] = 2.5");
  engine::ecs::MeshRenderer got{};
  failures +=
      check(api->entity_get(world, ent, mesh, &got, sizeof(got)) == 0 &&
                got.base_color[0] == 2.5F,
            "inspector: value reads back 2.5");

  // Undo: replay the before-image, assert the old value returns.
  failures +=
      check(api->entity_set(world, ent, mesh, &before, sizeof(before)) == 0,
            "inspector: undo replays before-image");
  failures +=
      check(api->entity_get(world, ent, mesh, &got, sizeof(got)) == 0 &&
                got.base_color[0] == 0.0F,
            "inspector: undo restores old value");

  // Remove Component + undo (re-add).
  failures += check(api->entity_remove(world, ent, mesh) == 0,
                    "inspector: entity_remove ok");
  failures += check(api->entity_get(world, ent, mesh, &got, sizeof(got)) != 0,
                    "inspector: removed row reads back missing");
  failures +=
      check(api->entity_set(world, ent, mesh, &after, sizeof(after)) == 0,
            "inspector: re-add restores row");
  failures +=
      check(api->entity_get(world, ent, mesh, &got, sizeof(got)) == 0 &&
                got.base_color[0] == 2.5F,
            "inspector: re-added value intact");

  // Double remove is a no-op success (undo-stack safe).
  failures += check(api->entity_remove(world, ent, mesh) == 0,
                    "inspector: remove present ok");
  failures += check(api->entity_remove(world, ent, mesh) == 0,
                    "inspector: remove absent is no-op");

  api->world_destroy(world);
  std::printf(failures == 0 ? "inspector_edits: all checks passed\n"
                            : "inspector_edits: %d FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}

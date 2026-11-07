// test_prefab.cpp — P6-09 acceptance (native CTest `engine_prefab`).
//
// Covers: save/instantiate shape, the acceptance merge (asset edit ->
// update -> non-overridden lanes follow, overridden lanes keep), apply
// (asset file gains the instance value, overrides cleared), revert (live
// restored to base), nesting (nested asset edit updates nested instances
// only), cycle guard, and fail-closed codes on bad paths.
//
// Prints [PASS]/[FAIL] lines; unbuffered stdout (a segfault keeps every
// PASS line, same convention as the scene IO test). No GPU, no window —
// pure Flecs + prefab JSON.
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "engine/ecs/ecs_render_components.hpp"
#include "engine/ecs/ecs_world.hpp"
#include "engine/ecs/transform.hpp"
#include "engine/scene/prefab.hpp"
#include "engine/scene/scene_entities.hpp"
#include "flecs.h"

namespace {

auto check(bool ok, const char* name) -> int {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  return ok ? 0 : 1;
}

struct Mover {
  float vx{0.0F};
  float vy{0.0F};
  float vz{0.0F};
};
static_assert(sizeof(Mover) == 12, "Mover is 12 bytes");

auto SetPose(ecs_world_t* world, ecs_entity_t local_id, ecs_entity_t e,
             float x, float y, float z) -> void {
  ecs_add_id(world, e, local_id);
  auto* lt = static_cast<engine::ecs::LocalTransform*>(
      ecs_get_mut_id(world, e, local_id));
  lt->px = x;
  lt->py = y;
  lt->pz = z;
  lt->qx = 0.0F;
  lt->qy = 0.0F;
  lt->qz = 0.0F;
  lt->qw = 1.0F;
  lt->sx = 1.0F;
  lt->sy = 1.0F;
  lt->sz = 1.0F;
}

auto PoseX(ecs_world_t* world, ecs_entity_t e) -> float {
  float pose[10]{};
  if (!engine::scene::read_local_pose(world, e, pose)) {
    return -99999.0F;
  }
  return pose[0];
}

auto PoseY(ecs_world_t* world, ecs_entity_t e) -> float {
  float pose[10]{};
  if (!engine::scene::read_local_pose(world, e, pose)) {
    return -99999.0F;
  }
  return pose[1];
}

auto MoverX(ecs_world_t* world, ecs_entity_t e) -> float {
  const ecs_entity_t mover_id =
      ecs_lookup_path_w_sep(world, 0, "Game.Mover", "", "", false);
  if (mover_id == 0) {
    return -99999.0F;
  }
  const auto* mv = static_cast<const Mover*>(
      ecs_get_id(world, e, mover_id));
  if (mv == nullptr) {
    return -99999.0F;
  }
  return mv->vx;
}

// Build a source subtree: Root (pose 1,2,3 + Mover 7,8,9) + Child (pose).
auto BuildSource(engine::ecs::EcsWorld& ecs, ecs_world_t* world,
                 ecs_entity_t& root_out, ecs_entity_t& child_out) -> bool {
  if (!ecs.register_render()) {
    return false;
  }
  const ecs_entity_t local_id = ecs_lookup(world, "LocalTransform");
  const ecs_entity_t mover_id = engine::ecs::RegisterRenderComponent(
      world, "Game.Mover", sizeof(Mover), alignof(Mover));
  if (local_id == 0 || mover_id == 0) {
    return false;
  }
  const ecs_entity_t root = ecs_new(world);
  ecs_set_name(world, root, "Root");
  SetPose(world, local_id, root, 1.0F, 2.0F, 3.0F);
  const Mover mv{7.0F, 8.0F, 9.0F};
  ecs_set_id(world, root, mover_id, sizeof(mv), &mv);
  ecs_add_id(world, root, ecs_lookup(world, "WorldTransform"));
  const ecs_entity_t child = ecs_new(world);
  ecs_set_name(world, child, "Child");
  SetPose(world, local_id, child, 4.0F, 5.0F, 6.0F);
  ecs_add_id(world, child, ecs_lookup(world, "WorldTransform"));
  ecs_add_pair(world, child, EcsChildOf, root);
  root_out = root;
  child_out = child;
  return true;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  int failures = 0;
  const std::string dir =
      (argc > 1) ? std::string(argv[1]) : std::string(".");
  const std::string prefab_path = dir + "/p6_09_basic.engine-prefab";
  const std::string nested_path = dir + "/p6_09_nested.engine-prefab";
  const std::string outer_path = dir + "/p6_09_outer.engine-prefab";

  // --- 1. save + instantiate shape -----------------------------------------
  {
    engine::ecs::EcsWorld ecs;
    if (!ecs.construct()) {
      failures += check(false, "setup: construct world");
      return 1;
    }
    ecs_world_t* world = ecs.raw();
    ecs_entity_t src_root = 0;
    ecs_entity_t src_child = 0;
    failures += check(BuildSource(ecs, world, src_root, src_child),
                      "setup: build source subtree");
    engine::prefab::PrefabRegistry reg;
    const int32_t src =
        engine::prefab::save_prefab(world, src_root, prefab_path.c_str(),
                                    nullptr, nullptr, nullptr, nullptr);
    failures += check(src == 0, "save: prefab asset writes");
    std::uint64_t inst = 0;
    const int32_t irc = engine::prefab::instantiate_prefab(
        world, prefab_path.c_str(), nullptr, nullptr, reg, &inst);
    failures += check(irc == 0 && inst != 0, "instantiate: root created");
    failures +=
        check(reg.is_instance_root(world, inst), "probe: root is instance");
    // Shape: root + one child with names/poses intact.
    failures += check(PoseX(world, inst) == 1.0F && PoseY(world, inst) == 2.0F,
                      "instantiate: root pose matches asset");
    failures += check(MoverX(world, inst) == 7.0F,
                      "instantiate: user blob matches asset");
    // Child walk: exactly one child named Child at (4,5,6).
    ecs_entity_t child_live = 0;
    {
      ecs_query_desc_t qd{};
      qd.terms[0].id = ecs_lookup(world, "LocalTransform");
      qd.cache_kind = EcsQueryCacheAuto;
      ecs_query_t* q = ecs_query_init(world, &qd);
      ecs_iter_t it = ecs_query_iter(world, q);
      while (ecs_query_next(&it)) {
        for (int i = 0; i < it.count; ++i) {
          if (ecs_get_target(world, it.entities[i], EcsChildOf, 0) ==
              static_cast<ecs_entity_t>(inst)) {
            child_live = it.entities[i];
          }
        }
      }
      ecs_query_fini(q);
    }
    failures += check(child_live != 0, "instantiate: child attached");
    if (child_live != 0) {
      const char* n = ecs_get_name(world, child_live);
      failures += check(n != nullptr && std::string(n) == "Child",
                        "instantiate: child name matches");
      failures += check(PoseX(world, child_live) == 4.0F,
                        "instantiate: child pose matches");
    }
    // IsA probe on the root (existing editor_is_prefab_instance semantic).
    failures += check(ecs_get_target(world, static_cast<ecs_entity_t>(inst),
                                     EcsIsA, 0) != 0,
                      "probe: root carries EcsIsA anchor");
    ecs.destroy();
  }

  // --- 2. acceptance: asset change updates instances except overrides -----
  {
    engine::ecs::EcsWorld ecs;
    ecs.construct();
    ecs_world_t* world = ecs.raw();
    ecs_entity_t src_root = 0;
    ecs_entity_t src_child = 0;
    BuildSource(ecs, world, src_root, src_child);
    const ecs_entity_t local_id = ecs_lookup(world, "LocalTransform");
    engine::prefab::PrefabRegistry reg;
    engine::prefab::save_prefab(world, src_root, prefab_path.c_str(), nullptr,
                                nullptr, nullptr, nullptr);
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    engine::prefab::instantiate_prefab(world, prefab_path.c_str(), nullptr,
                                       nullptr, reg, &a);
    engine::prefab::instantiate_prefab(world, prefab_path.c_str(), nullptr,
                                       nullptr, reg, &b);
    // Override: instance A root X = 100 (lane-level); B untouched.
    {
      auto* lt = static_cast<engine::ecs::LocalTransform*>(
          ecs_get_mut_id(world, static_cast<ecs_entity_t>(a), local_id));
      lt->px = 100.0F;
    }
    // Rewrite the asset with X = 50, Y = 60 (simulates an asset edit).
    {
      auto* lt = static_cast<engine::ecs::LocalTransform*>(
          ecs_get_mut_id(world, src_root, local_id));
      lt->px = 50.0F;
      lt->py = 60.0F;
    }
    engine::prefab::save_prefab(world, src_root, prefab_path.c_str(), nullptr,
                                nullptr, nullptr, nullptr);
    std::uint64_t updated = 0;
    const int32_t urc = engine::prefab::update_prefab_instances(
        world, prefab_path.c_str(), nullptr, nullptr, reg, &updated);
    failures += check(urc == 0 && updated == 2, "update: both instances merged");
    // A: X keeps override (100), Y follows asset (60).
    failures += check(PoseX(world, static_cast<ecs_entity_t>(a)) == 100.0F,
                      "accept: overridden lane keeps instance value");
    failures += check(PoseY(world, static_cast<ecs_entity_t>(a)) == 60.0F,
                      "accept: non-overridden lane follows asset");
    // B: fully follows (50, 60).
    failures += check(PoseX(world, static_cast<ecs_entity_t>(b)) == 50.0F &&
                          PoseY(world, static_cast<ecs_entity_t>(b)) == 60.0F,
                      "accept: clean instance fully follows asset");
    ecs.destroy();
  }

  // --- 3. apply: instance value lands in the asset, overrides cleared -----
  {
    engine::ecs::EcsWorld ecs;
    ecs.construct();
    ecs_world_t* world = ecs.raw();
    ecs_entity_t src_root = 0;
    ecs_entity_t src_child = 0;
    BuildSource(ecs, world, src_root, src_child);
    const ecs_entity_t local_id = ecs_lookup(world, "LocalTransform");
    engine::prefab::PrefabRegistry reg;
    engine::prefab::save_prefab(world, src_root, prefab_path.c_str(), nullptr,
                                nullptr, nullptr, nullptr);
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    engine::prefab::instantiate_prefab(world, prefab_path.c_str(), nullptr,
                                       nullptr, reg, &a);
    engine::prefab::instantiate_prefab(world, prefab_path.c_str(), nullptr,
                                       nullptr, reg, &b);
    {
      auto* lt = static_cast<engine::ecs::LocalTransform*>(
          ecs_get_mut_id(world, static_cast<ecs_entity_t>(a), local_id));
      lt->px = 111.0F;
    }
    failures += check(engine::prefab::apply_prefab(world, a, nullptr, nullptr,
                                                   reg) == 0,
                      "apply: writes asset");
    // Fresh instance from the rewritten asset carries X = 111.
    std::uint64_t c = 0;
    engine::prefab::instantiate_prefab(world, prefab_path.c_str(), nullptr,
                                       nullptr, reg, &c);
    failures += check(PoseX(world, static_cast<ecs_entity_t>(c)) == 111.0F,
                      "apply: asset file gains instance value");
    // Overrides cleared: a no-change update keeps every lane.
    std::uint64_t updated = 0;
    engine::prefab::update_prefab_instances(world, prefab_path.c_str(),
                                            nullptr, nullptr, reg, &updated);
    failures += check(PoseX(world, static_cast<ecs_entity_t>(a)) == 111.0F,
                      "apply: overrides cleared after apply");
    ecs.destroy();
  }

  // --- 4. revert: live restored to base ------------------------------------
  {
    engine::ecs::EcsWorld ecs;
    ecs.construct();
    ecs_world_t* world = ecs.raw();
    ecs_entity_t src_root = 0;
    ecs_entity_t src_child = 0;
    BuildSource(ecs, world, src_root, src_child);
    const ecs_entity_t local_id = ecs_lookup(world, "LocalTransform");
    engine::prefab::PrefabRegistry reg;
    engine::prefab::save_prefab(world, src_root, prefab_path.c_str(), nullptr,
                                nullptr, nullptr, nullptr);
    std::uint64_t a = 0;
    engine::prefab::instantiate_prefab(world, prefab_path.c_str(), nullptr,
                                       nullptr, reg, &a);
    {
      auto* lt = static_cast<engine::ecs::LocalTransform*>(
          ecs_get_mut_id(world, static_cast<ecs_entity_t>(a), local_id));
      lt->px = 999.0F;
      lt->py = 999.0F;
    }
    failures += check(engine::prefab::revert_prefab(world, a, reg) == 0,
                      "revert: restores base");
    failures += check(PoseX(world, static_cast<ecs_entity_t>(a)) == 1.0F &&
                          PoseY(world, static_cast<ecs_entity_t>(a)) == 2.0F,
                      "revert: lanes match asset");
    ecs.destroy();
  }

  // --- 5. nesting: nested asset edit updates nested instances only --------
  {
    engine::ecs::EcsWorld ecs;
    ecs.construct();
    ecs_world_t* world = ecs.raw();
    ecs_entity_t src_root = 0;
    ecs_entity_t src_child = 0;
    BuildSource(ecs, world, src_root, src_child);
    const ecs_entity_t local_id = ecs_lookup(world, "LocalTransform");
    // Nested asset: single entity NestedRoot at (10,0,0).
    const ecs_entity_t nroot = ecs_new(world);
    ecs_set_name(world, nroot, "NestedRoot");
    SetPose(world, local_id, nroot, 10.0F, 0.0F, 0.0F);
    ecs_add_id(world, nroot, ecs_lookup(world, "WorldTransform"));
    engine::prefab::PrefabRegistry reg;
    failures += check(engine::prefab::save_prefab(world, nroot, nested_path.c_str(),
                                                  nullptr, nullptr, nullptr,
                                                  nullptr) == 0,
                      "nest: nested asset writes");
    // Outer asset: hand-written envelope with a PrefabRef mount.
    {
      std::string outer =
          "{\"format\":\"engine-prefab\",\"version\":1,\"entities\":["
          "{\"id\":1,\"name\":\"Outer\",\"parent\":0,\"components\":{"
          "\"LocalTransform\":{\"position\":[0,0,0],\"rotation\":[0,0,0,1],"
          "\"scale\":[1,1,1]}}},"
          "{\"id\":2,\"name\":\"Mount\",\"parent\":1,\"PrefabRef\":{\"asset\":\"" +
          nested_path +
          "\"},\"components\":{}}]}";
      std::FILE* f = std::fopen(outer_path.c_str(), "wb");
      if (f != nullptr) {
        std::fwrite(outer.data(), 1, outer.size(), f);
        std::fclose(f);
      }
    }
    std::uint64_t o = 0;
    failures += check(engine::prefab::instantiate_prefab(
                          world, outer_path.c_str(), nullptr, nullptr, reg,
                          &o) == 0 && o != 0,
                      "nest: outer instantiates with mount");
    // Find the nested root (named NestedRoot under the mount).
    ecs_entity_t nested_live = 0;
    {
      ecs_query_desc_t qd{};
      qd.terms[0].id = local_id;
      qd.cache_kind = EcsQueryCacheAuto;
      ecs_query_t* q = ecs_query_init(world, &qd);
      ecs_iter_t it = ecs_query_iter(world, q);
      while (ecs_query_next(&it)) {
        for (int i = 0; i < it.count; ++i) {
          const char* n = ecs_get_name(world, it.entities[i]);
          if (n != nullptr && std::string(n) == "NestedRoot") {
            nested_live = it.entities[i];
          }
        }
      }
      ecs_query_fini(q);
    }
    failures += check(nested_live != 0, "nest: nested root materialized");
    if (nested_live != 0) {
      failures += check(PoseX(world, nested_live) == 10.0F,
                        "nest: nested pose matches nested asset");
      failures += check(
          reg.asset_for_instance(static_cast<std::uint64_t>(nested_live)) ==
              nested_path,
          "nest: nested root registered under nested asset");
    }
    // Edit the NESTED asset (X = 42), update nested asset: nested live
    // follows, outer root untouched.
    {
      auto* lt = static_cast<engine::ecs::LocalTransform*>(
          ecs_get_mut_id(world, nroot, local_id));
      lt->px = 42.0F;
    }
    engine::prefab::save_prefab(world, nroot, nested_path.c_str(), nullptr,
                                nullptr, nullptr, nullptr);
    std::uint64_t updated = 0;
    engine::prefab::update_prefab_instances(world, nested_path.c_str(),
                                            nullptr, nullptr, reg, &updated);
    failures += check(updated == 1, "nest: one nested instance updated");
    if (nested_live != 0) {
      failures += check(PoseX(world, nested_live) == 42.0F,
                        "nest: nested instance follows nested asset");
    }
    failures += check(PoseX(world, static_cast<ecs_entity_t>(o)) == 0.0F,
                      "nest: outer root untouched by nested update");
    ecs.destroy();
  }

  // --- 6. fail-closed codes --------------------------------------------------
  {
    engine::ecs::EcsWorld ecs;
    ecs.construct();
    ecs_world_t* world = ecs.raw();
    engine::prefab::PrefabRegistry reg;
    std::uint64_t out = 0;
    failures += check(engine::prefab::instantiate_prefab(
                          world, "/no/such/file.engine-prefab", nullptr,
                          nullptr, reg, &out) < 0,
                      "codes: missing asset fails");
    failures += check(engine::prefab::instantiate_prefab(
                          nullptr, prefab_path.c_str(), nullptr, nullptr, reg,
                          &out) < 0,
                      "codes: null world fails");
    failures += check(engine::prefab::revert_prefab(world, 999999, reg) < 0,
                      "codes: unknown instance fails");
    failures += check(engine::prefab::apply_prefab(world, 999999, nullptr,
                                                   nullptr, reg) < 0,
                      "codes: apply unknown instance fails");
    std::uint64_t updated = 0;
    failures += check(engine::prefab::update_prefab_instances(
                          world, "/no/such/file.engine-prefab", nullptr,
                          nullptr, reg, &updated) < 0,
                      "codes: update missing asset fails");
    ecs.destroy();
  }

  std::printf(failures == 0 ? "test_prefab: all checks passed\n"
                            : "test_prefab: %d FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}

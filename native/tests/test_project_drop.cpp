// test_project_drop.cpp — P3-08 acceptance: project_drop_asset.
//
// Stages assets/cube.glb through the M5-01 fast lane and asserts ONE new
// entity with a VALID mesh_id (> 0) within one call (the M5 gate:
// visible in one frame — the spawned MeshRenderer row carries the id
// the forward path keys draws off, so no second tick is needed).
//
// Fixture: the minimal valid GLB is synthesized in a scratch dir (same
// generator as test_asset_pipeline.cpp: JSON with 1 mesh / 1 primitive /
// 2 nodes + BIN chunk), so no binaries are checked in and no network is
// needed. The drop also mints the .meta sidecar next to the staged file
// (GUID DB path), which the test asserts exists.
//
// Failure matrix (fail-closed, outputs untouched):
//   missing file (-3), malformed bytes (-3), non-model kind (-4),
//   null session/args (-1/-2), unknown delete target (-2).
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "minimal_glb_fixture.hpp"
#include "engine/core/job_scheduler.hpp"
#include "engine/ecs/ecs_world.hpp"
#include "engine/ecs/editor_bridge.hpp"
#include "engine/ecs/transform.hpp"
#include "flecs.h"

namespace fs = std::filesystem;

namespace {

auto failures = 0;

auto check(bool ok, const char* name) -> int {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
  return ok ? 0 : 1;
}

}  // namespace

auto main() -> int {
  using engine::ecs::EcsWorld;
  using engine::editor::EditorSession;

  // Scratch lives in the build tree (never a shared tmp): cwd == the
  // test's binary dir under ctest, unique per run (Sonar S5443-safe).
  const std::string root = "project_drop_scratch";
  std::error_code ec;
  fs::remove_all(root, ec);
  const std::string assets = root + "/assets";
  fs::create_directories(assets, ec);
  const std::string glb = assets + "/cube.glb";
  check(engine::tests::WriteMinimalGlb(glb), "drop: fixture cube.glb written");

  engine::core::JobScheduler sched;
  check(sched.init(), "drop: scheduler init");
  EcsWorld world;
  world.set_scheduler(&sched);
  check(world.construct(), "drop: world construct");
  // Render components registered so the MeshRenderer row resolves
  // (drop fails -8 without them — fail-closed, no silent attach).
  check(world.register_render(), "drop: render components registered");

  EditorSession session;
  check(session.bind(world.raw()), "drop: session bind");

  uint64_t before = 0;
  check(session.count_roots(&before) == 0, "drop: baseline roots readable");

  // --- acceptance: one drop -> one entity, valid mesh_id ------------------
  uint64_t entity = 0;
  std::uint32_t mesh_id = 0;
  const int rc = session.drop_asset(glb.c_str(), 1.5F, -2.5F, &entity, &mesh_id);
  check(rc == 0, "drop: project_drop_asset returns 0");
  check(entity != 0, "drop: one new entity spawned");
  check(mesh_id != 0, "drop: mesh_id valid (> 0)");

  uint64_t after = 0;
  check(session.count_roots(&after) == 0 && after == before + 1,
        "drop: exactly one root added");

  // Pose lands on the ground point with identity rotation/scale.
  std::array<float, 10> pose{};
  check(session.get_local(entity, pose.data()) == 0, "drop: pose readable");
  check(pose[0] == 1.5F && pose[1] == 0.0F && pose[2] == -2.5F,
        "drop: ground point under cursor");
  check(pose[6] == 1.0F && pose[7] == 1.0F && pose[9] == 1.0F,
        "drop: identity rotation/scale");

  // MeshRenderer row resolves with the SAME mesh_id (one frame: the
  // forward path keys draws off this id, no second tick needed).
  const ecs_entity_t mesh_comp = ecs_lookup_path_w_sep(
      world.raw(), 0, "Engine.Runtime.MeshRenderer", "", "", false);
  check(mesh_comp != 0, "drop: MeshRenderer component resolves");
  const auto* mr = static_cast<const engine::ecs::MeshRenderer*>(
      ecs_get_id(world.raw(), static_cast<ecs_entity_t>(entity), mesh_comp));
  check(mr != nullptr, "drop: MeshRenderer row attached");
  check(mr != nullptr && mr->mesh_id == mesh_id && mr->mesh_id != 0,
        "drop: MeshRenderer mesh_id matches (M5 one-frame gate)");

  // Determinism: re-dropping the same file yields the same mesh_id.
  uint64_t entity2 = 0;
  std::uint32_t mesh_id2 = 0;
  check(session.drop_asset(glb.c_str(), 0.0F, 0.0F, &entity2, &mesh_id2) == 0,
        "drop: second drop succeeds");
  check(entity2 != 0 && entity2 != entity, "drop: second drop is a fresh entity");
  check(mesh_id2 == mesh_id, "drop: same file -> same mesh_id");

  // .meta sidecar minted by the fast lane (GUID DB path).
  check(fs::exists(glb + ".meta", ec), "drop: .meta sidecar minted");

  // --- failure matrix (fail-closed, outputs untouched) ---------------------
  {
    uint64_t e = 0xDEADu;
    std::uint32_t m = 0xBEAFu;
    check(session.drop_asset((assets + "/missing.glb").c_str(), 0, 0, &e, &m) != 0,
          "drop: missing file rejected");
    check(e == 0xDEADu && m == 0xBEAFu, "drop: outputs untouched on failure");
  }
  {
    const std::string bad = assets + "/bad.glb";
    std::ofstream f(bad, std::ios::binary | std::ios::trunc);
    f.write("not a glb", 9);
    f.close();
    uint64_t e = 0;
    std::uint32_t m = 0;
    check(session.drop_asset(bad.c_str(), 0, 0, &e, &m) != 0,
          "drop: malformed bytes rejected");
    check(e == 0 && m == 0, "drop: outputs zeroed on malformed input");
  }
  {
    const std::string txt = assets + "/note.txt";
    std::ofstream f(txt, std::ios::trunc);
    f << "hello";
    f.close();
    uint64_t e = 0;
    std::uint32_t m = 0;
    check(session.drop_asset(txt.c_str(), 0, 0, &e, &m) != 0,
          "drop: non-model kind rejected");
  }
  check(session.drop_asset(nullptr, 0, 0, nullptr, nullptr) != 0,
        "drop: null args rejected");
  check(session.delete_entity(999999001u) != 0, "drop: delete unknown rejected");

  // --- undo step: delete the dropped entity --------------------------------
  // Note: the second drop's entity stays alive, so roots go from
  // (before + 2) back to (before + 1) — undo removes exactly one.
  check(session.delete_entity(entity) == 0, "drop: undo delete succeeds");
  check(!ecs_is_alive(world.raw(), static_cast<ecs_entity_t>(entity)),
        "drop: entity gone after undo");
  uint64_t final_roots = 0;
  check(session.count_roots(&final_roots) == 0 && final_roots == after,
        "drop: one root removed by undo");

  fs::remove_all(root, ec);
  std::printf(failures == 0 ? "PROJECT_DROP_OK\n" : "PROJECT_DROP_FAILED\n");
  return failures == 0 ? 0 : 1;
}

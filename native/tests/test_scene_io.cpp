// test_scene_io.cpp — P2-06 acceptance (native CTest `engine_scene_io`).
//
// Builds a 3-level hierarchy of 10 entities with built-in + user components,
// saves, loads into a fresh world, saves again, and asserts the two files
// are byte-identical. A second probe loads a file with a bad $size user
// component and asserts the rest loads (one warning printed).
//
// Prints:
//   [PASS] scene: round-trip stable
//   [PASS] scene: bad user component skipped
//
// No GPU, no window — pure Flecs + scene JSON. Unbuffered stdout (a segfault
// keeps every PASS line, same convention as the physics bodies test).
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "engine/ecs/ecs_render_components.hpp"
#include "engine/ecs/ecs_world.hpp"
#include "engine/ecs/transform.hpp"
#include "engine/scene/scene_io.hpp"
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
             float x, float y, float z, float sx, float sy, float sz) -> void {
  // ecs_get_mut_id does NOT add the component — emplace it first (add +
  // then mutate), otherwise the pointer is null and the pose silently
  // stays zero.
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
  lt->sx = sx;
  lt->sy = sy;
  lt->sz = sz;
}

// Build the 10-entity scene: root Floor + 3 children, each child with 2
// grandchildren (1 + 3 + 6 = 10), 3 levels deep. Every entity gets a Mover
// blob; the floor gets a MeshRenderer; one child gets a Camera + light.
// `ecs` is the owning EcsWorld wrapper (register_render needs it), `world`
// is its raw handle.
auto BuildScene(engine::ecs::EcsWorld& ecs, ecs_world_t* world) -> bool {
  if (!ecs.register_render()) {
    return false;
  }
  const ecs_entity_t local_id = ecs_lookup(world, "LocalTransform");
  const ecs_entity_t world_id = ecs_lookup(world, "WorldTransform");
  const ecs_entity_t mesh_id = ecs_lookup_path_w_sep(
      world, 0, "Engine.Runtime.MeshRenderer", "", "", false);
  const ecs_entity_t cam_id = ecs_lookup_path_w_sep(
      world, 0, "Engine.Runtime.Camera", "", "", false);
  const ecs_entity_t light_id = ecs_lookup_path_w_sep(
      world, 0, "Engine.Runtime.DirectionalLight", "", "", false);
  const ecs_entity_t mover_id = engine::ecs::RegisterRenderComponent(
      world, "Game.Mover", sizeof(Mover), alignof(Mover));
  if (local_id == 0 || mesh_id == 0 || cam_id == 0 || light_id == 0 ||
      mover_id == 0) {
    return false;
  }
  engine::scene::register_mesh_source(7, "assets/cube.glb#0");

  const ecs_entity_t floor = ecs_new(world);
  ecs_set_name(world, floor, "Floor");
  SetPose(world, local_id, floor, 0, 0, 0, 10, 0.2F, 10);
  ecs_add_id(world, floor, world_id);
  const Mover floor_mv{1.0F, 0, 0};
  ecs_set_id(world, floor, mover_id, sizeof(floor_mv), &floor_mv);
  engine::ecs::MeshRenderer floor_mr;
  floor_mr.mesh_id = 7;
  floor_mr.base_color[0] = 0.8F;
  floor_mr.base_color[1] = 0.8F;
  floor_mr.base_color[2] = 0.8F;
  floor_mr.base_color[3] = 1.0F;
  ecs_set_id(world, floor, mesh_id, sizeof(floor_mr), &floor_mr);

  for (int c = 0; c < 3; ++c) {
    const ecs_entity_t child = ecs_new(world);
    std::string cname = "Child" + std::to_string(c);
    ecs_set_name(world, child, cname.c_str());
    SetPose(world, local_id, child, static_cast<float>(c), 1.0F, 0, 1, 1, 1);
    ecs_add_id(world, child, world_id);
    ecs_add_pair(world, child, EcsChildOf, floor);
    const Mover cmv{0, static_cast<float>(c), 0};
    ecs_set_id(world, child, mover_id, sizeof(cmv), &cmv);
    if (c == 0) {
      engine::ecs::Camera cam{1.047F, 0.1F, 100.0F, 1};
      ecs_set_id(world, child, cam_id, sizeof(cam), &cam);
      engine::ecs::DirectionalLight li;
      li.direction[0] = 0;
      li.direction[1] = -1.0F;
      li.direction[2] = 0;
      li.shadow = 1.0F;
      ecs_set_id(world, child, light_id, sizeof(li), &li);
    }
    for (int g = 0; g < 2; ++g) {
      const ecs_entity_t gc = ecs_new(world);
      std::string gname =
          "Grand" + std::to_string(c) + "_" + std::to_string(g);
      ecs_set_name(world, gc, gname.c_str());
      SetPose(world, local_id, gc, static_cast<float>(g), 2.0F,
              static_cast<float>(c), 0.5F, 0.5F, 0.5F);
      ecs_add_id(world, gc, world_id);
      ecs_add_pair(world, gc, EcsChildOf, child);
      const Mover gmv{0, 0, static_cast<float>(g)};
      ecs_set_id(world, gc, mover_id, sizeof(gmv), &gmv);
    }
  }
  return true;
}

auto ReadFile(const char* path, std::vector<std::uint8_t>& out) -> bool {
  // Sonar S5443: scratch paths come from the CTest build dir (argv[1]) or
  // the cwd fallback — never a shared world-writable dir — and this reader
  // only ever opens the files it just wrote (+ the in-test fixture).
  // fopen_s on MSVC (C4996 under /WX); std::fopen elsewhere.
  std::FILE* f = nullptr;
#if defined(_MSC_VER)
  if (fopen_s(&f, path, "rb") != 0) {
    return false;
  }
#else
  f = std::fopen(path, "rb");
#endif
  if (f == nullptr) {
    return false;
  }
  std::array<char, 4096> buf{};
  while (true) {
    const std::size_t got = std::fread(buf.data(), 1, buf.size(), f);
    out.insert(out.end(), reinterpret_cast<const std::uint8_t*>(buf.data()),
               reinterpret_cast<const std::uint8_t*>(buf.data()) + got);
    if (got < buf.size()) {
      break;
    }
  }
  std::fclose(f);
  return true;
}

// Count LocalTransform entities in a world.
auto CountLocal(ecs_world_t* world) -> int {
  const ecs_entity_t local_id = ecs_lookup(world, "LocalTransform");
  if (local_id == 0) {
    return -1;
  }
  ecs_query_desc_t qd{};
  qd.terms[0].id = local_id;
  qd.cache_kind = EcsQueryCacheAuto;
  ecs_query_t* q = ecs_query_init(world, &qd);
  if (q == nullptr) {
    return -1;
  }
  int n = 0;
  ecs_iter_t it = ecs_query_iter(world, q);
  while (ecs_query_next(&it)) {
    n += it.count;
  }
  ecs_query_fini(q);
  return n;
}

}  // namespace

// Resolve the scratch dir: argv[1] is the CTest binary dir (see
// native/tests/CMakeLists.txt — engine_scene_io ${CMAKE_CURRENT_BINARY_DIR}).
// When run by hand without argv, default to the process working directory
// ("."), never a shared world-writable path. argv[1] values containing ".."
// are rejected so the test can never be steered outside its build dir
// (Sonar cpp:S5443 — only use argv[1] inside a permission-checked dir).
auto ScratchDir(int argc, char** argv) -> std::string {
  std::string dir = ".";
  if (argc > 1 && argv[1] != nullptr) {
    dir = argv[1];
  }
  if (dir.find("..") != std::string::npos) {
    std::printf("[WARN] scene: rejecting scratch dir with '..', using cwd\n");
    return ".";
  }
  return dir;
}

auto main(int argc, char** argv) -> int {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  int failures = 0;
  const std::string dir = ScratchDir(argc, argv);
  const std::string file_a = std::string(dir) + "/p206_roundtrip_a.json";
  const std::string file_b = std::string(dir) + "/p206_roundtrip_b.json";
  const std::string file_bad = std::string(dir) + "/p206_bad.json";

  // --- round-trip stable --------------------------------------------------
  {
    engine::ecs::EcsWorld w1;
    failures += check(w1.construct(), "scene: world1 construct");
    failures +=
        check(BuildScene(w1, w1.raw()), "scene: build 10-entity scene");
    failures +=
        check(CountLocal(w1.raw()) == 10, "scene: 10 Local entities built");
    const int32_t save_rc =
        engine::scene::save_scene(w1.raw(), file_a.c_str());
    failures += check(save_rc == 0, "scene: save A ok");

    engine::ecs::EcsWorld w2;
    failures += check(w2.construct(), "scene: world2 construct");
    const int32_t load_rc =
        engine::scene::load_scene(w2.raw(), file_a.c_str());
    failures += check(load_rc >= 0, "scene: load into fresh world ok");
    failures +=
        check(CountLocal(w2.raw()) == 10, "scene: 10 entities after load");
    const int32_t save2_rc =
        engine::scene::save_scene(w2.raw(), file_b.c_str());
    failures += check(save2_rc == 0, "scene: save B ok");

    std::vector<std::uint8_t> a;
    std::vector<std::uint8_t> b;
    const bool read_ok = ReadFile(file_a.c_str(), a) &&
                         ReadFile(file_b.c_str(), b);
    failures += check(read_ok, "scene: both files readable");
    const bool identical = read_ok && a == b;
    if (!identical && read_ok) {
      std::printf("[INFO] scene: file A %zu bytes, file B %zu bytes\n",
                  a.size(), b.size());
    }
    failures += check(identical, "scene: round-trip stable");
    engine::scene::clear_mesh_sources();
    w1.destroy();
    w2.destroy();
  }

  // --- unicode escape parse (CodeRabbit #206 round 2) ---------------------
  // BMP escape (\u0041 -> 'A') and a surrogate pair (\uD83D\uDE00 -> U+1F600)
  // in an entity name must survive the scene parser cursor fix (removed the
  // two extra cur advances after ParseHex4 that skipped the char after each
  // \uXXXX escape). Scene files are written by AppendJsonString (control
  // chars as \u00XX) but hand-written scene files may carry any BMP escape,
  // so parse a hand-written payload directly.
  {
    const char* uni_json =
        "{\"format\":\"engine-scene\",\"version\":1,\"entities\":["
        "{\"id\":1,\"name\":\"U\\u0041name_\\uD83D\\uDE00\",\"parent\":0,"
        "\"components\":{}}]}";
    const std::string file_u = std::string(dir) + "/p206_unicode.json";
    std::FILE* fu = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&fu, file_u.c_str(), "wb") != 0) {
      fu = nullptr;
    }
#else
    fu = std::fopen(file_u.c_str(), "wb");
#endif
    failures += check(fu != nullptr, "scene: unicode fixture written");
    if (fu != nullptr) {
      std::fwrite(uni_json, 1, std::strlen(uni_json), fu);
      std::fclose(fu);
    }
    engine::ecs::EcsWorld w4;
    failures += check(w4.construct(), "scene: world4 construct");
    failures += check(w4.register_render(), "scene: world4 render regs");
    const int32_t load_u =
        engine::scene::load_scene(w4.raw(), file_u.c_str());
    failures += check(load_u >= 0, "scene: unicode escapes load ok");
    failures += check(load_u == 0, "scene: unicode escapes no skips");
    w4.destroy();
  }

  // --- bad user component skipped -----------------------------------------
  {
    // A file with one good entity + one entity whose Mover blob has a
    // wrong $size (4 vs live 12): load must keep the good rows and print
    // exactly one warning.
    const char* bad_json =
        "{\"format\":\"engine-scene\",\"version\":1,\"entities\":["
        "{\"id\":1,\"name\":\"Good\",\"parent\":0,\"components\":{"
        "\"LocalTransform\":{\"position\":[1,2,3],\"rotation\":[0,0,0,1],"
        "\"scale\":[1,1,1]}}},"
        "{\"id\":2,\"name\":\"Bad\",\"parent\":0,\"components\":{"
        "\"LocalTransform\":{\"position\":[0,0,0],\"rotation\":[0,0,0,1],"
        "\"scale\":[1,1,1]},"
        "\"Game.Mover\":{\"$bytes\":\"00000000\",\"$size\":4,"
        "\"$schema\":\"00000000\"}}}"
        "]}";
    const std::string_view bad_text(bad_json);
    std::FILE* f = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&f, file_bad.c_str(), "wb") != 0) {
      f = nullptr;
    }
#else
    f = std::fopen(file_bad.c_str(), "wb");
#endif
    failures += check(f != nullptr, "scene: bad fixture written");
    if (f != nullptr) {
      std::fwrite(bad_text.data(), 1, bad_text.size(), f);
      std::fclose(f);
    }
    engine::ecs::EcsWorld w3;
    failures += check(w3.construct(), "scene: world3 construct");
    // Pre-register the TRUE layout so the wrong-$size blob mismatches.
    const ecs_entity_t mover = engine::ecs::RegisterRenderComponent(
        w3.raw(), "Game.Mover", sizeof(Mover), alignof(Mover));
    failures += check(mover != 0, "scene: Mover pre-registered (12B)");
    const int32_t rc = engine::scene::load_scene(w3.raw(), file_bad.c_str());
    failures += check(rc == 1, "scene: bad user component skipped");
    failures +=
        check(CountLocal(w3.raw()) == 2, "scene: good rows still load");
    w3.destroy();
  }

  return failures;
}

// main.cpp — P2-07 engine-player executable.
//
// Usage:
//   engine-player <project_dir> [--scene <relative path>] [--frames N]
//                 [--screenshot out.png] [--hidden]
//
// A project dir holds project.json:
//   {"name":"...","startScene":"scenes/main.scene.json",
//    "gameAssembly":"bin/Game.dll"}
//
// Behavior:
//   1. Read project.json (minimal JSON string-field scan, no dependency).
//   2. Create the world via the ABI table, create a 640x360 renderer
//      (hidden when --hidden is passed).
//   3. Try to boot the game assembly through the existing dotnet_host:
//      runtime config = <assembly path, .dll -> .runtimeconfig.json>;
//      entry point = type "Game.Bootstrap, <assembly>" method "Main".
//      The bootstrap (when present) registers components/systems and
//      populates the world/scene. A missing assembly, missing config, or
//      missing entry is NOT fatal: the player falls back to the
//      built-in HelloCube-equivalent demo frame (one camera, one light,
//      one tan cube) so `samples/HelloCube` — which ships no project.json
//      and no bootstrap — renders the golden.
//   4. When a start scene exists, load it through the scene ABI tail
//      (scene_load); the loaded entities render like any other world.
//   5. Run the loop: [autoplay inject] -> world_frame (P2-03 fixed 1/60 s
//      wall step keeps the golden deterministic) -> world_propagate ->
//      renderer_frame, until the window closes or N frames pass.
//   6. --screenshot writes the last presented frame as PNG (RGBA; the
//      native screenshot path already swizzles BGRA — gotcha 23).
//   7. --autoplay <inputs.json> replays recorded key states per frame for
//      testing (P2-08): JSON is an object with a "frames" array; each
//      element is an object with an optional "keys" array of key names
//      held that frame (e.g. {"frames":[{"keys":["W"]},{"keys":[]}]}).
//      Key names are SDL key names (single ASCII letters map to their SDL
//      scancode; "Space" maps to SDL_SCANCODE_SPACE=44). Before each
//      world_frame the player pushes SDL key down/up events so the held
//      set matches the frame entry exactly (deterministic replay through
//      the same event-fed path as live input). After the last script
//      frame the player exits 0. A missing/unreadable file or malformed
//      JSON is a usage error (exit 1).
//
// Exit codes: 0 ok; 1 usage/argument error (incl. missing/malformed
// autoplay file); 2 engine setup failure
// (world/renderer/scene); 3 frame failure. ENGINE_REQUIRE_GPU=1 turns a
// missing Vulkan device into [FAIL] + nonzero exit (gotcha 2); without it
// the player prints [SKIP] and exits 0.
//
// Rules honored: gotcha 11 (no volk.c here; Vulkan resolves inside
// libengine_core), gotcha 21 (no strlen/snprintf/memset/C arrays —
// std::string/std::array/std::span only), gotcha 22 (no subprocesses
// spawned at all — argv array rule is vacuous but noted).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(ENGINE_PLAYER_HAS_SDL3)
#include <SDL3/SDL.h>
#endif

#include "engine/bridge/dotnet_host.hpp"
#include "engine/bridge/engine_abi.h"

// --- tiny JSON string-field scan -------------------------------------------
// project.json is a flat object with three string fields. A full parser is
// overkill; scan for `"key" : "value"` with whitespace tolerance. Returns
// empty when the key is absent or malformed.
namespace {

auto ReadFileBytes(const std::string& path, std::vector<char>& out) -> bool {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  const std::string s = ss.str();
  out.assign(s.begin(), s.end());
  return true;
}

auto ScanJsonStringField(std::string_view text, std::string_view key)
    -> std::string {
  std::string quoted = "\"";
  quoted += key;
  quoted += "\"";
  std::size_t pos = text.find(quoted);
  if (pos == std::string_view::npos) {
    return {};
  }
  pos += quoted.size();
  while (pos < text.size() &&
         (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' ||
          text[pos] == '\r')) {
    ++pos;
  }
  if (pos >= text.size() || text[pos] != ':') {
    return {};
  }
  ++pos;
  while (pos < text.size() &&
         (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' ||
          text[pos] == '\r')) {
    ++pos;
  }
  if (pos >= text.size() || text[pos] != '"') {
    return {};
  }
  ++pos;
  std::string out;
  while (pos < text.size() && text[pos] != '"') {
    if (text[pos] == '\\' && pos + 1 < text.size()) {
      ++pos;
      const char esc = text[pos];
      if (esc == 'n') {
        out.push_back('\n');
      } else if (esc == 't') {
        out.push_back('\t');
      } else if (esc == '/') {
        out.push_back('/');
      } else {
        out.push_back(esc);
      }
      ++pos;
    } else {
      out.push_back(text[pos]);
      ++pos;
    }
  }
  return out;
}

auto JoinPath(const std::string& dir, const std::string& rel) -> std::string {
  if (dir.empty()) {
    return rel;
  }
  if (!rel.empty() && (rel[0] == '/' || rel[0] == '\\')) {
    return rel;
  }
  std::string out = dir;
  if (!out.empty() && out.back() != '/' && out.back() != '\\') {
    out.push_back('/');
  }
  out += rel;
  return out;
}

auto FileExists(const std::string& path) -> bool {
  std::ifstream f(path, std::ios::binary);
  return static_cast<bool>(f);
}

auto ReplaceSuffix(const std::string& path, std::string_view from,
                   std::string_view to) -> std::string {
  if (path.size() >= from.size() &&
      path.compare(path.size() - from.size(), from.size(), from) == 0) {
    std::string out = path.substr(0, path.size() - from.size());
    out += to;
    return out;
  }
  std::string out = path;
  out += to;
  return out;
}

auto RequireGpu() -> bool {
#if defined(_WIN32)
  // MSVC treats getenv as a C4996 error under /WX: _dupenv_s instead.
  char* dup = nullptr;
  std::size_t dup_len = 0;
  const bool ok =
      _dupenv_s(&dup, &dup_len, "ENGINE_REQUIRE_GPU") == 0 && dup != nullptr;
  const bool required = ok && dup[0] == '1' && dup[1] == '\0';
  std::free(dup);
  return required;
#else
  const char* v = std::getenv("ENGINE_REQUIRE_GPU");
  return v != nullptr && v[0] == '1' && v[1] == '\0';
#endif
}

auto BaseName(const std::string& path) -> std::string {
  std::size_t pos = path.find_last_of("/\\");
  return (pos == std::string::npos) ? path : path.substr(pos + 1);
}

auto StripDllSuffix(const std::string& file) -> std::string {
  constexpr std::string_view kDll = ".dll";
  if (file.size() > kDll.size() &&
      file.compare(file.size() - kDll.size(), kDll.size(), kDll) == 0) {
    return file.substr(0, file.size() - kDll.size());
  }
  return file;
}

// --- --autoplay inputs.json replay (P2-08) ----------------------------------
// Script format: {"frames":[{"keys":["W","A"]}, {"keys":[]}, ...]}.
// Key names: single ASCII letters (case-insensitive, SDL scancode order:
// 'A'=4 ... 'Z'=29) plus "Space" (SDL_SCANCODE_SPACE=44). Unknown names
// are rejected. Replay pushes synthetic SDL key down/up events each frame
// so the held set matches the script exactly; game code reads the same
// event-fed tables as live input (deterministic across runs).

// SDL scancode for A..Z is 4 + (upper - 'A'); Space is 44 (SDL_scancode.h,
// mirrored by managed Engine.Runtime Key enum).
inline constexpr std::uint32_t kScancodeLetterBase = 4;
inline constexpr std::uint32_t kScancodeSpace = 44;

auto AutoplayKeyToScancode(std::string_view name) -> int {
  if (name.size() == 1) {
    const char c = name[0];
    if (c >= 'a' && c <= 'z') {
      return static_cast<int>(kScancodeLetterBase +
                              static_cast<std::uint32_t>(c - 'a'));
    }
    if (c >= 'A' && c <= 'Z') {
      return static_cast<int>(kScancodeLetterBase +
                              static_cast<std::uint32_t>(c - 'A'));
    }
    return -1;
  }
  if (name == "Space" || name == "space" || name == "SPACE") {
    return static_cast<int>(kScancodeSpace);
  }
  return -1;
}

struct AutoplayFrame {
  std::vector<std::uint32_t> keys;
};

struct AutoplayScript {
  std::vector<AutoplayFrame> frames;
};

// Minimal JSON scan for the {"frames":[{"keys":[...]}]} shape: locates
// the "frames" array, splits it into top-level {...} objects (brace
// depth tracked with string awareness), then reads each object's "keys"
// string array. Rejects malformed input (unbalanced brackets/quotes,
// unknown key names) so a corrupt script fails closed instead of
// replaying a truncated prefix.
auto ParseAutoplay(std::string_view text, AutoplayScript& out, std::string& err)
    -> bool {
  out.frames.clear();
  const std::size_t fpos = text.find("\"frames\"");
  if (fpos == std::string_view::npos) {
    err = "missing \"frames\" array";
    return false;
  }
  std::size_t arr = text.find('[', fpos);
  if (arr == std::string_view::npos) {
    err = "missing \"frames\" array";
    return false;
  }
  ++arr;
  // Find the matching close of the frames array (bracket depth, strings
  // skipped) so trailing garbage cannot hide a truncation.
  int depth = 1;
  bool in_str = false;
  bool esc = false;
  std::size_t arr_end = std::string_view::npos;
  for (std::size_t i = arr; i < text.size(); ++i) {
    const char c = text[i];
    if (in_str) {
      if (esc) {
        esc = false;
      } else if (c == '\\') {
        esc = true;
      } else if (c == '"') {
        in_str = false;
      }
      continue;
    }
    if (c == '"') {
      in_str = true;
    } else if (c == '[') {
      ++depth;
    } else if (c == ']') {
      if (--depth == 0) {
        arr_end = i;
        break;
      }
    }
  }
  if (arr_end == std::string_view::npos) {
    err = "unbalanced JSON";
    return false;
  }
  const std::string_view body = text.substr(arr, arr_end - arr);
  // Split body into top-level {...} frame objects.
  std::size_t i = 0;
  while (i < body.size()) {
    // Skip separators/whitespace between objects.
    while (i < body.size() &&
           (body[i] == ' ' || body[i] == '\t' || body[i] == '\n' ||
            body[i] == '\r' || body[i] == ',')) {
      ++i;
    }
    if (i >= body.size()) {
      break;
    }
    if (body[i] != '{') {
      err = "expected frame object";
      return false;
    }
    int bdepth = 0;
    bool s = false;
    bool e = false;
    std::size_t j = i;
    for (; j < body.size(); ++j) {
      const char c = body[j];
      if (s) {
        if (e) {
          e = false;
        } else if (c == '\\') {
          e = true;
        } else if (c == '"') {
          s = false;
        }
        continue;
      }
      if (c == '"') {
        s = true;
      } else if (c == '{') {
        ++bdepth;
      } else if (c == '}') {
        if (--bdepth == 0) {
          ++j;
          break;
        }
      }
    }
    if (bdepth != 0) {
      err = "unbalanced JSON";
      return false;
    }
    const std::string_view obj = body.substr(i, j - i);
    i = j;
    // Read the object's "keys" array when present (absent = no keys held).
    AutoplayFrame frame;
    const std::size_t kpos = obj.find("\"keys\"");
    if (kpos != std::string_view::npos) {
      std::size_t kb = obj.find('[', kpos);
      if (kb == std::string_view::npos) {
        err = "malformed \"keys\" array";
        return false;
      }
      ++kb;
      std::string cur;
      bool ks = false;
      bool ke = false;
      bool closed = false;
      for (std::size_t k = kb; k < obj.size(); ++k) {
        const char c = obj[k];
        if (ks) {
          if (ke) {
            cur.push_back(c);
            ke = false;
          } else if (c == '\\') {
            ke = true;
          } else if (c == '"') {
            ks = false;
            const int sc = AutoplayKeyToScancode(cur);
            if (sc < 0) {
              err = std::string("unknown key '") + cur + "'";
              return false;
            }
            frame.keys.push_back(static_cast<std::uint32_t>(sc));
            cur.clear();
          } else {
            cur.push_back(c);
          }
          continue;
        }
        if (c == '"') {
          ks = true;
          cur.clear();
        } else if (c == ']') {
          closed = true;
          break;
        }
      }
      if (!closed || ks) {
        err = "malformed \"keys\" array";
        return false;
      }
    }
    out.frames.push_back(frame);
  }
  if (out.frames.empty()) {
    err = "no frames";
    return false;
  }
  return true;
}

auto LoadAutoplay(const std::string& path, AutoplayScript& out, std::string& err)
    -> bool {
  std::vector<char> bytes;
  if (!ReadFileBytes(path, bytes)) {
    err = std::string("cannot read file: ") + path;
    return false;
  }
  if (bytes.empty()) {
    err = "empty file";
    return false;
  }
  return ParseAutoplay(std::string_view(bytes.data(), bytes.size()), out, err);
}

#if defined(ENGINE_PLAYER_HAS_SDL3)
// Push one synthetic key event. Key-down uses repeat=false so the engine
// records a fresh press edge; key-up clears the hold.
auto PushAutoplayKey(std::uint32_t scancode, bool down) -> void {
  SDL_Event ev{};
  ev.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
  ev.key.scancode = static_cast<SDL_Scancode>(scancode);
  ev.key.key = SDLK_UNKNOWN;
  ev.key.down = down;
  ev.key.repeat = false;
  SDL_PushEvent(&ev);
}
#endif

// Drive the held-key set to exactly `want` (sorted scan over prev/want).
// No-op without SDL3 (stub renderer builds have no event queue).
auto ApplyAutoplayFrame(const std::vector<std::uint32_t>& want,
                        std::vector<std::uint32_t>& held) -> void {
#if defined(ENGINE_PLAYER_HAS_SDL3)
  std::vector<std::uint32_t> sorted = want;
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  for (const std::uint32_t sc : held) {
    if (!std::binary_search(sorted.begin(), sorted.end(), sc)) {
      PushAutoplayKey(sc, false);
    }
  }
  for (const std::uint32_t sc : sorted) {
    if (!std::binary_search(held.begin(), held.end(), sc)) {
      PushAutoplayKey(sc, true);
    }
  }
  held = sorted;
#else
  (void)want;
  (void)held;
#endif
}

}  // namespace

namespace {

// --- HelloCube-equivalent demo frame ----------------------------------------
// Native mirror of samples/HelloCube (same eye/light/cube/tan): the C#
// sample registers no systems (Spinner never binds), so the golden is a
// static cube after one propagate. Rebuilt here through the ABI only so
// no sample sources move and no component header leaks past the ABI
// boundary (packed 16/20/16-byte payloads, field order per
// ecs_render_components.hpp + the P1-09 C# mirrors).

struct CameraPayload {
  float fov_y_rad{0.0F};
  float near_z{0.0F};
  float far_z{0.0F};
  std::uint32_t is_main{0};
};

struct MeshRendererPayload {
  std::uint32_t mesh_id{0};
  float base_color[4]{0.0F, 0.0F, 0.0F, 0.0F};
};

struct DirectionalLightPayload {
  float direction[3]{0.0F, 0.0F, 0.0F};
  float shadow{0.0F};
};

struct LocalPayload {
  // 64-byte aligned built-in LocalTransform row (gotcha 20: register by
  // exact native size/align, never a guessed number).
  alignas(64) float lanes[16]{};
};

struct WorldPayload {
  // Row-vector lookAt (P1-08 convention): basis rows + eye in row 3.
  // Camera looks down -Z; matches HelloCube LookAtWorld exactly.
  alignas(64) float m[16]{};
};

static_assert(sizeof(CameraPayload) == 16, "Camera payload is 16 bytes");
static_assert(sizeof(MeshRendererPayload) == 20,
              "MeshRenderer payload is 20 bytes");
static_assert(sizeof(DirectionalLightPayload) == 16,
              "DirectionalLight payload is 16 bytes");

auto SetEntity(const EngineApiTable* table, EngineWorld* world,
               std::uint64_t entity, std::uint64_t comp, const void* data,
               std::uint32_t size, const char* what) -> bool {
  const std::int32_t rc =
      table->entity_set(world, entity, comp, data, size);
  if (rc != 0) {
    std::printf("[FAIL] player: entity_set %s rc=%d\n", what, rc);
    return false;
  }
  return true;
}

// Uploads the 24-vertex / 36-index unit cube (face normals, same winding
// as P1-08 make_helper_cube / HelloCube Cube()). Values mirror the shared
// helper cube (kept local so the player binary has no test-helper
// dependency); the duplicated compare TU is gone (player compare reuses
// the hello-cube compare binary via CTest env).
auto UploadUnitCube(const EngineApiTable* table, EngineRenderer* renderer)
    -> std::uint32_t {
  // faces: normal + 4 corners (same values as mesh_test_helpers.hpp).
  constexpr float kFaces[6][5][3] = {
      {{1, 0, 0}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1}, {1, -1, 1}},
      {{-1, 0, 0}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}, {-1, -1, -1}},
      {{0, 1, 0}, {-1, 1, -1}, {-1, 1, 1}, {1, 1, 1}, {1, 1, -1}},
      {{0, -1, 0}, {-1, -1, 1}, {-1, -1, -1}, {1, -1, -1}, {1, -1, 1}},
      {{0, 0, 1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},
      {{0, 0, -1}, {1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}},
  };
  std::array<float, 24 * 8> verts{};
  std::array<std::uint32_t, 36> idx{};
  for (int f = 0; f < 6; ++f) {
    for (int v = 0; v < 4; ++v) {
      const std::size_t o = static_cast<std::size_t>(f * 4 + v) * 8;
      verts[o + 0] = kFaces[f][1 + v][0] * 0.5F;
      verts[o + 1] = kFaces[f][1 + v][1] * 0.5F;
      verts[o + 2] = kFaces[f][1 + v][2] * 0.5F;
      verts[o + 3] = kFaces[f][0][0];
      verts[o + 4] = kFaces[f][0][1];
      verts[o + 5] = kFaces[f][0][2];
      verts[o + 6] = (v == 0 || v == 3) ? 0.0F : 1.0F;
      verts[o + 7] = (v < 2) ? 0.0F : 1.0F;
    }
    const std::uint32_t b = static_cast<std::uint32_t>(f) * 4;
    const std::array<std::uint32_t, 6> quad{b,     b + 1, b + 2,
                                            b,     b + 2, b + 3};
    for (int k = 0; k < 6; ++k) {
      idx[static_cast<std::size_t>(f) * 6 + k] = quad[k];
    }
  }
  const std::uint32_t id = table->renderer_mesh_create(
      renderer, verts.data(), 24, idx.data(), 36);
  if (id == 0) {
    std::printf("[FAIL] player: mesh upload failed\n");
  }
  return id;
}

// Builds the HelloCube-equivalent frame through the ABI. The camera
// carries NO Local row (a root: keep the hand-written World row, same as
// the C# sample — propagate() would overwrite World from Local).
auto BuildDemoFrame(const EngineApiTable* table, EngineWorld* world,
                    std::uint32_t cube_mesh) -> bool {
  const std::uint64_t cam_id =
      table->component_register(world, "Engine.Runtime.Camera", 16, 4);
  const std::uint64_t mesh_id = table->component_register(
      world, "Engine.Runtime.MeshRenderer", 20, 4);
  const std::uint64_t light_id = table->component_register(
      world, "Engine.Runtime.DirectionalLight", 16, 4);
  const std::uint64_t local_id =
      table->component_register(world, "LocalTransform", 64, 64);
  const std::uint64_t world_id =
      table->component_register(world, "WorldTransform", 64, 64);
  if (cam_id == 0 || mesh_id == 0 || light_id == 0 || local_id == 0 ||
      world_id == 0) {
    std::printf("[FAIL] player: render component ids resolve\n");
    return false;
  }

  const std::uint64_t cam = table->entity_create(world);
  const std::uint64_t light = table->entity_create(world);
  const std::uint64_t cube = table->entity_create(world);
  if (cam == 0 || light == 0 || cube == 0) {
    std::printf("[FAIL] player: entity_create\n");
    return false;
  }

  CameraPayload c{};
  c.fov_y_rad = 60.0F * 3.14159265F / 180.0F;
  c.near_z = 0.1F;
  c.far_z = 100.0F;
  c.is_main = 1;
  // Eye (0,0,3) -> origin: side=(1,0,0)... rows: [1 0 0 0 / 0 1 0 0 /
  // 0 0 1 0 / 0 0 3 1] (same numbers HelloCube LookAtWorld produces).
  WorldPayload w{};
  w.m[0] = 1.0F;
  w.m[5] = 1.0F;
  w.m[10] = 1.0F;
  w.m[12] = 0.0F;
  w.m[13] = 0.0F;
  w.m[14] = 3.0F;
  w.m[15] = 1.0F;

  DirectionalLightPayload li{};
  {
    const float len = std::sqrt(0.25F + 0.81F + 0.4225F);
    li.direction[0] = -0.5F / len;
    li.direction[1] = -0.9F / len;
    li.direction[2] = -0.65F / len;
    li.shadow = 1.0F;  // P4-03 shadow toggle on (same as the C# sample).
  }

  LocalPayload ident{};
  // Identity local: pos (0,0,0), quat (0,0,0,1), scale (1,1,1).
  // Native LocalTransform lane order: px,py,pz lanes 0-2; qx..qw 3-6;
  // sx,sy,sz lanes 7-9.
  ident.lanes[6] = 1.0F;
  ident.lanes[7] = 1.0F;
  ident.lanes[8] = 1.0F;
  ident.lanes[9] = 1.0F;
  MeshRendererPayload mr{};
  mr.mesh_id = cube_mesh;
  mr.base_color[0] = 0.85F;
  mr.base_color[1] = 0.5F;
  mr.base_color[2] = 0.2F;
  mr.base_color[3] = 1.0F;

  return SetEntity(table, world, cam, cam_id, &c, sizeof(c), "camera") &&
         SetEntity(table, world, cam, world_id, &w, sizeof(w),
                   "camera world") &&
         SetEntity(table, world, light, light_id, &li, sizeof(li),
                   "light") &&
         SetEntity(table, world, cube, local_id, &ident, sizeof(ident),
                   "cube local") &&
         SetEntity(table, world, cube, world_id, &w, sizeof(w),
                   "cube world") &&
         SetEntity(table, world, cube, mesh_id, &mr, sizeof(mr),
                   "cube mesh");
}

}  // namespace

namespace {

// --- game-assembly bootstrap ------------------------------------------------
// Convention: the game assembly may export
//   [UnmanagedCallersOnly] static void Game.Bootstrap.Main()
// which registers components/systems and populates the world. Present in
// future game projects; absent in HelloCube (native demo frame instead).
//
// NOTE (CodeRabbit round 1): passing EngineWorld* into the UCO was
// considered and deliberately NOT done here. The managed side has no
// documented bootstrap contract yet (no Game.Bootstrap in managed/,
// samples/, or docs; P2-08 owns future scene/bootstrap work), and the
// hostfxr smoke path binds callbacks WITH explicit context params
// (EcsChunkView*, EngineChunkColumns*) — a void() entry cannot receive a
// world pointer. So the convention stays void(); a game that needs the
// world obtains it through the managed API-table binding (same as every
// other managed system), and the bootstrap path stays latent until a real
// game assembly exists. HelloCube ships none, so TryBootGame never fires
// on the acceptance path.
using BootstrapFn = void (*)();

auto TryBootGame(const std::string& assembly_path) -> bool {
  if (!FileExists(assembly_path)) {
    std::printf("[INFO] player: no game assembly at %s (demo frame)\n",
                assembly_path.c_str());
    return false;
  }
  const std::string config = ReplaceSuffix(assembly_path, ".dll",
                                           ".runtimeconfig.json");
  if (!FileExists(config)) {
    std::printf(
        "[INFO] player: no runtimeconfig at %s (demo frame)\n",
        config.c_str());
    return false;
  }
  engine::bridge::DotNetHost* host =
      engine::bridge::dotnet_host_create(config.c_str(), nullptr);
  if (host == nullptr) {
    std::printf("[INFO] player: hostfxr boot failed (demo frame)\n");
    return false;
  }
  const std::string stem = StripDllSuffix(BaseName(assembly_path));
  std::string type_name = "Game.Bootstrap, ";
  type_name += stem;
  void* fn = engine::bridge::dotnet_host_get_system_pointer(
      host, assembly_path.c_str(), type_name.c_str(), "Main");
  if (fn == nullptr) {
    // No bootstrap entry: destroy the host first (the pointer is null so
    // nothing escapes), then fall back to the demo frame.
    engine::bridge::dotnet_host_destroy(host);
    std::printf(
        "[INFO] player: no Game.Bootstrap.Main in %s (demo frame)\n",
        assembly_path.c_str());
    return false;
  }
  // Bootstrap present: call it BEFORE destroying the host. The pointer
  // comes from hostfxr's load_assembly_and_get_function_pointer and is
  // valid only until dotnet_host_destroy (hostfxr_close + dlclose), so
  // the call must happen while the host is alive (dotnet_host.hpp:
  // "Valid until dotnet_host_destroy()").
  auto* boot = reinterpret_cast<BootstrapFn>(fn);  // NOLINT
  boot();
  engine::bridge::dotnet_host_destroy(host);
  std::printf("[PASS] player: game bootstrap ran (%s)\n",
              assembly_path.c_str());
  return true;
}

}  // namespace

auto PrintHelp() -> void {
  std::printf(
      "usage: engine-player <project_dir> [--scene <rel>] [--frames N] "
      "[--screenshot out.png] [--hidden] [--autoplay inputs.json]\n"
      "  --autoplay inputs.json: replay per-frame key states from JSON "
      "({\"frames\":[{\"keys\":[\"W\"]}]}) deterministically; "
      "exits 0 after the last script frame\n");
}

auto main(int argc, char** argv) -> int {
  if (argc >= 2 && argv[1] != nullptr &&
      (std::string_view(argv[1]) == "--help" ||
       std::string_view(argv[1]) == "-h")) {
    PrintHelp();
    return 0;
  }
  if (argc < 2 || argv[1] == nullptr || argv[1][0] == '\0') {
    std::fprintf(stderr,
                 "usage: engine-player <project_dir> [--scene <rel>] "
                 "[--frames N] [--screenshot out.png] [--hidden] "
                 "[--autoplay inputs.json]\n");
    return 1;
  }
  const std::string project_dir = argv[1];
  std::string scene_override;
  int frames = -1;
  std::string screenshot;
  bool hidden = false;
  std::string autoplay_path;
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i] != nullptr ? argv[i] : "";
    if (a == "--scene" && i + 1 < argc && argv[i + 1] != nullptr) {
      scene_override = argv[++i];
    } else if (a == "--frames" && i + 1 < argc && argv[i + 1] != nullptr) {
      frames = std::atoi(argv[++i]);
    } else if (a == "--screenshot" && i + 1 < argc &&
               argv[i + 1] != nullptr) {
      screenshot = argv[++i];
    } else if (a == "--autoplay" && i + 1 < argc && argv[i + 1] != nullptr) {
      autoplay_path = argv[++i];
    } else if (a == "--hidden") {
      hidden = true;
    } else if (a == "--help" || a == "-h") {
      PrintHelp();
      return 0;
    } else {
      std::fprintf(stderr, "engine-player: unknown argument '%s'\n",
                   argv[i] != nullptr ? argv[i] : "");
      return 1;
    }
  }
  if (frames < 0) {
    frames = 60;
  }
  if (frames == 0) {
    frames = 1;
  }

  // Autoplay script (loaded before engine setup so a bad path fails fast
  // with usage exit 1, before any window/renderer work).
  AutoplayScript autoplay;
  bool use_autoplay = false;
  if (!autoplay_path.empty()) {
    std::string err;
    if (!LoadAutoplay(autoplay_path, autoplay, err)) {
      std::fprintf(stderr, "engine-player: --autoplay %s\n", err.c_str());
      return 1;
    }
    use_autoplay = true;
    std::printf("[INFO] player: autoplay %s (%zu frames)\n",
                autoplay_path.c_str(), autoplay.frames.size());
  }

  // --- project.json (optional: absent in samples/HelloCube) -----------------
  std::string start_scene;
  std::string game_assembly;
  {
    std::vector<char> bytes;
    if (ReadFileBytes(JoinPath(project_dir, "project.json"), bytes)) {
      const std::string_view text(bytes.data(), bytes.size());
      start_scene = ScanJsonStringField(text, "startScene");
      game_assembly = ScanJsonStringField(text, "gameAssembly");
      const std::string name = ScanJsonStringField(text, "name");
      std::printf("[INFO] player: project '%s'\n",
                  name.empty() ? project_dir.c_str() : name.c_str());
    } else {
      std::printf("[INFO] player: no project.json in %s (demo mode)\n",
                  project_dir.c_str());
    }
  }
  if (!scene_override.empty()) {
    start_scene = scene_override;
  }
  if (!game_assembly.empty()) {
    game_assembly = JoinPath(project_dir, game_assembly);
  }

  const EngineApiTable* table = engine_get_api_table();
  if (table == nullptr) {
    std::printf("[FAIL] player: engine_get_api_table returned null\n");
    return 2;
  }

  EngineWorld* world = table->world_create();
  if (world == nullptr) {
    std::printf("[FAIL] player: world_create\n");
    return 2;
  }

  std::uint32_t flags = 0;
  if (hidden) {
    flags |= ENGINE_RENDERER_HIDDEN;
  }
  EngineRenderer* renderer = table->renderer_create(world, 640, 360, flags);
  if (renderer == nullptr) {
    table->world_destroy(world);
    if (RequireGpu()) {
      std::printf(
          "[FAIL] player: no Vulkan device (ENGINE_REQUIRE_GPU=1)\n");
      return 2;
    }
    std::printf("[SKIP] player: no Vulkan device on this runner\n");
    return 0;
  }

  // Game code first (registers systems/components), then the scene file,
  // then the built-in demo frame when neither provided content. The demo
  // frame is skipped when a game bootstrap ran OR the scene loaded at
  // least one entity — hmm, scene_load returns skip counts, not entity
  // counts, so: skip the demo only when the bootstrap ran or the scene
  // file loaded cleanly (rc >= 0). An empty/missing start scene keeps the
  // demo so the HelloCube golden always has a cube.
  bool booted = false;
  if (!game_assembly.empty()) {
    booted = TryBootGame(game_assembly);
  } else {
    std::printf("[INFO] player: no gameAssembly (demo frame)\n");
  }

  bool scene_ok = false;
  if (!start_scene.empty()) {
    const std::string scene_path = JoinPath(project_dir, start_scene);
    const std::int32_t rc = table->scene_load(world, scene_path.c_str());
    if (rc < 0) {
      std::printf("[FAIL] player: scene_load %s rc=%d\n",
                  scene_path.c_str(), rc);
      table->renderer_destroy(renderer);
      table->world_destroy(world);
      return 2;
    }
    std::printf("[PASS] player: scene loaded (%s, %d skipped)\n",
                scene_path.c_str(), rc);
    scene_ok = true;
  }

  if (!booted && !scene_ok) {
    const std::uint32_t cube = UploadUnitCube(table, renderer);
    if (cube == 0) {
      if (RequireGpu()) {
        std::printf(
            "[FAIL] player: no GPU mesh pipeline (ENGINE_REQUIRE_GPU=1)\n");
        table->renderer_destroy(renderer);
        table->world_destroy(world);
        return 2;
      }
      std::printf("[SKIP] player: no GPU mesh pipeline on this runner\n");
      table->renderer_destroy(renderer);
      table->world_destroy(world);
      return 0;
    }
    if (!BuildDemoFrame(table, world, cube)) {
      table->renderer_destroy(renderer);
      table->world_destroy(world);
      return 2;
    }
    std::printf("[PASS] player: demo frame built (HelloCube-equivalent)\n");
  }

  if (table->world_propagate(world) != 0) {
    std::printf("[FAIL] player: world_propagate (pre)\n");
    table->renderer_destroy(renderer);
    table->world_destroy(world);
    return 2;
  }

  int shown = 0;
  std::vector<std::uint32_t> autoplay_held;
  // Autoplay bounds the run to the script length (a longer --frames is
  // clamped; --frames still caps a shorter replay when explicitly set).
  int run_frames = frames;
  if (use_autoplay &&
      static_cast<std::size_t>(frames) > autoplay.frames.size()) {
    run_frames = static_cast<int>(autoplay.frames.size());
  }
  for (int f = 0; f < run_frames; ++f) {
    if (use_autoplay) {
      ApplyAutoplayFrame(autoplay.frames[static_cast<std::size_t>(f)].keys,
                         autoplay_held);
    }
    const std::int32_t steps = table->world_frame(world, 1.0 / 60.0);
    if (steps < 0) {
      std::printf("[FAIL] player: world_frame rc=%d\n", steps);
      table->renderer_destroy(renderer);
      table->world_destroy(world);
      return 3;
    }
    if (table->world_propagate(world) != 0) {
      std::printf("[FAIL] player: world_propagate rc frame %d\n", f);
      table->renderer_destroy(renderer);
      table->world_destroy(world);
      return 3;
    }
    const std::int32_t frc = table->renderer_frame(renderer);
    if (frc == 1) {
      break;  // window closed.
    }
    if (frc != 0) {
      std::printf("[FAIL] player: renderer_frame rc=%d\n", frc);
      table->renderer_destroy(renderer);
      table->world_destroy(world);
      return 3;
    }
    shown = f + 1;
  }
  std::printf("[PASS] player: %d frames without close\n", shown);
  if (use_autoplay) {
    std::printf("[PASS] player: autoplay complete (%d frames)\n", shown);
  }

  if (!screenshot.empty()) {
    const std::int32_t src =
        table->renderer_screenshot(renderer, screenshot.c_str());
    if (src != 0) {
      std::printf("[FAIL] player: screenshot rc=%d\n", src);
      table->renderer_destroy(renderer);
      table->world_destroy(world);
      return 3;
    }
    std::printf("[PASS] player: screenshot written: %s\n",
                screenshot.c_str());
  }

  table->renderer_destroy(renderer);
  table->world_destroy(world);
  std::printf("PLAYER-OK\n");
  return 0;
}

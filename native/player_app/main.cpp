// main.cpp — P2-07 engine-player executable.
//
// Usage:
//   engine-player <project_dir> [--scene <relative path>] [--frames N]
//                 [--screenshot out.png] [--hidden] [--content <game.pak>]
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
//   7. --content <game.pak> cold-opens a content pack through PakReader
//      (M5-04 ENGPAK, P3-10 content): header/TOC bounds are verified at
//      open (fail closed, exit 2 on missing/corrupt). CRCs verify per
//      entry ON READ (PakReader contract) — the pack open itself does not
//      hash every entry: with 0-entry placeholder paks (asset-less games
//      like MarbleRun) there is nothing to hash, and with real content
//      the game owns interpretation entry-by-entry. The entry count is
//      logged; game code owns interpretation.
//   8. --autoplay <inputs.json> replays recorded key states per frame for
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
// (world/renderer/scene/content-pack); 3 frame failure. COLD_BOOT_S=<s>
// prints on stdout after the first presented frame (wall seconds from
// engine init to first frame). ENGINE_REQUIRE_GPU=1 turns a
// missing Vulkan device into [FAIL] + nonzero exit (gotcha 2); without it
// the player prints [SKIP] and exits 0.
//
// Rules honored: gotcha 11 (no volk.c here; Vulkan resolves inside
// libengine_core), gotcha 21 (no strlen/snprintf/memset/C arrays —
// std::string/std::array/std::span only), gotcha 22 (no subprocesses
// spawned at all — argv array rule is vacuous but noted).
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(ENGINE_PLAYER_HAS_SDL3)
#include <SDL3/SDL.h>
#endif

#include "engine/bridge/dotnet_host.hpp"
#include "engine/assets/asset_pak.hpp"
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
//
// Kept as small single-purpose helpers (Sonar S3776/S134) instead of one
// long scanner: each helper below handles exactly one grammar level.
namespace autoplay_scan {

// Advance over one JSON string starting at the opening quote at `at`;
// returns the index just past the closing quote, or npos when unterminated.
auto SkipString(std::string_view text, std::size_t at) -> std::size_t {
  // Caller guarantees text[at] == '"'.
  bool esc = false;
  for (std::size_t i = at + 1; i < text.size(); ++i) {
    const char c = text[i];
    if (esc) {
      esc = false;
      continue;
    }
    if (c == '\\') {
      esc = true;
    } else if (c == '"') {
      return i + 1;
    }
  }
  return std::string_view::npos;
}

// Find the index just past the matching close bracket of the '[' at
// `open`; strings are skipped so brackets inside quotes don't count.
// Returns npos when unbalanced.
auto MatchBracket(std::string_view text, std::size_t open) -> std::size_t {
  int depth = 1;
  std::size_t i = open + 1;
  while (i < text.size()) {
    const char c = text[i];
    if (c == '"') {
      i = SkipString(text, i);
      if (i == std::string_view::npos) {
        return std::string_view::npos;
      }
      continue;
    }
    if (c == '[') {
      ++depth;
    } else if (c == ']') {
      if (--depth == 0) {
        return i + 1;
      }
    }
    ++i;
  }
  return std::string_view::npos;
}

// Locate the "frames" array body (between its outer brackets) in `text`.
// Returns false with `err` set when the shape is absent or unbalanced.
auto FindFramesBody(std::string_view text, std::string_view& body,
                    std::string& err) -> bool {
  const std::size_t fpos = text.find("\"frames\"");
  if (fpos == std::string_view::npos) {
    err = "missing \"frames\" array";
    return false;
  }
  const std::size_t open = text.find('[', fpos);
  if (open == std::string_view::npos) {
    err = "missing \"frames\" array";
    return false;
  }
  const std::size_t end = MatchBracket(text, open);
  if (end == std::string_view::npos) {
    err = "unbalanced JSON";
    return false;
  }
  body = text.substr(open + 1, end - open - 2);
  return true;
}

// Advance over one balanced {...} object starting at `at` (which holds
// '{'); returns the index just past its closing brace, or npos.
auto MatchBrace(std::string_view text, std::size_t at) -> std::size_t {
  int depth = 0;
  std::size_t j = at;
  while (j < text.size()) {
    const char c = text[j];
    if (c == '"') {
      j = SkipString(text, j);
      if (j == std::string_view::npos) {
        return std::string_view::npos;
      }
      continue;
    }
    if (c == '{') {
      ++depth;
    } else if (c == '}') {
      if (--depth == 0) {
        return j + 1;
      }
    }
    ++j;
  }
  return std::string_view::npos;
}

auto SkipSeparators(std::string_view text, std::size_t& i) -> void {
  while (i < text.size()) {
    const char c = text[i];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != ',') {
      break;
    }
    ++i;
  }
}

// Append one decoded key name to `frame`; false + err on unknown names.
auto PushKeyName(std::string_view cur, AutoplayFrame& frame,
                 std::string& err) -> bool {
  const int sc = AutoplayKeyToScancode(cur);
  if (sc < 0) {
    err = std::string("unknown key '") + std::string(cur) + "'";
    return false;
  }
  frame.keys.push_back(static_cast<std::uint32_t>(sc));
  return true;
}

// Read one quoted key name starting at `at` (which holds '"'): on success
// appends the decoded scancode and returns the index past the closing
// quote; on failure returns npos with `err` set.
auto ParseKeyName(std::string_view obj, std::size_t at, AutoplayFrame& frame,
                  std::string& err) -> std::size_t {
  const std::size_t end = SkipString(obj, at);
  if (end == std::string_view::npos) {
    err = "malformed \"keys\" array";
    return std::string_view::npos;
  }
  // Inner string has no escapes that survive (key names are ASCII letters
  // or "Space"); the raw slice between the quotes is the name.
  if (!PushKeyName(obj.substr(at + 1, end - at - 2), frame, err)) {
    return std::string_view::npos;
  }
  return end;
}

// Parse the ["..."] value starting at `kb` (just past '[') inside `obj`;
// stops at the closing ']' and reports it via `closed`.
auto ParseKeysList(std::string_view obj, std::size_t kb, AutoplayFrame& frame,
                   bool& closed, std::string& err) -> bool {
  for (std::size_t k = kb; k < obj.size(); ++k) {
    const char c = obj[k];
    if (c == '"') {
      const std::size_t next = ParseKeyName(obj, k, frame, err);
      if (next == std::string_view::npos) {
        return false;
      }
      k = next - 1;
      continue;
    }
    if (c == ']') {
      closed = true;
      break;
    }
  }
  return true;
}

// Read the object's "keys" array when present (absent = no keys held).
auto ParseFrameKeys(std::string_view obj, AutoplayFrame& frame,
                    std::string& err) -> bool {
  if (const std::size_t kpos = obj.find("\"keys\"");
      kpos == std::string_view::npos) {
    return true;
  }
  std::size_t kb = obj.find('[', obj.find("\"keys\""));
  if (kb == std::string_view::npos) {
    err = "malformed \"keys\" array";
    return false;
  }
  ++kb;
  bool closed = false;
  if (!ParseKeysList(obj, kb, frame, closed, err)) {
    return false;
  }
  if (!closed) {
    err = "malformed \"keys\" array";
    return false;
  }
  return true;
}

// Parse one top-level {...} frame object starting at `at`; appends the
// frame and returns the index just past it (npos + err on failure).
auto ParseFrameObject(std::string_view body, std::size_t at,
                      AutoplayScript& out, std::string& err) -> std::size_t {
  if (body[at] != '{') {
    err = "expected frame object";
    return std::string_view::npos;
  }
  const std::size_t end = MatchBrace(body, at);
  if (end == std::string_view::npos) {
    err = "unbalanced JSON";
    return std::string_view::npos;
  }
  AutoplayFrame frame;
  if (!ParseFrameKeys(body.substr(at, end - at), frame, err)) {
    return std::string_view::npos;
  }
  out.frames.push_back(frame);
  return end;
}

}  // namespace autoplay_scan

auto ParseAutoplay(std::string_view text, AutoplayScript& out, std::string& err)
    -> bool {
  using namespace autoplay_scan;
  out.frames.clear();
  std::string_view body;
  if (!FindFramesBody(text, body, err)) {
    return false;
  }
  std::size_t i = 0;
  while (i < body.size()) {
    SkipSeparators(body, i);
    if (i >= body.size()) {
      break;
    }
    i = ParseFrameObject(body, i, out, err);
    if (i == std::string_view::npos) {
      return false;
    }
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
//
// NOTE (Sonar S6232): the event's `key` union member is activated by
// whole-member assignment from an SDL_KeyboardEvent aggregate (not by
// assigning .type first and then the .key fields), per SDL3's documented
// SDL_Event union usage.
auto PushAutoplayKey(std::uint32_t scancode, bool down) -> void {
  const SDL_EventType type =
      down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
  const SDL_KeyboardEvent key_event{
      .type = type,
      .reserved = 0,
      .timestamp = 0,
      .windowID = 0,
      .which = 0,
      .scancode = static_cast<SDL_Scancode>(scancode),
      .key = SDLK_UNKNOWN,
      .mod = SDL_KMOD_NONE,
      .raw = 0,
      .down = down,
      .repeat = false,
  };
  SDL_Event ev{};
  ev.key = key_event;
  SDL_PushEvent(&ev);
}
#endif

// Drive the held-key set to exactly `want` (sorted scan over prev/want).
// No-op without SDL3 (stub renderer builds have no event queue).
auto ApplyAutoplayFrame(const std::vector<std::uint32_t>& want,
                        std::vector<std::uint32_t>& held) -> void {
#if defined(ENGINE_PLAYER_HAS_SDL3)
  std::vector<std::uint32_t> sorted = want;
  std::ranges::sort(sorted);
  const auto uniq = std::ranges::unique(sorted);
  sorted.erase(uniq.begin(), uniq.end());
  const auto release_missing =
      [&sorted](std::uint32_t sc) {
        if (!std::ranges::binary_search(sorted, sc)) {
          PushAutoplayKey(sc, false);
        }
      };
  const auto press_new =
      [&held](std::uint32_t sc) {
        if (!std::ranges::binary_search(held, sc)) {
          PushAutoplayKey(sc, true);
        }
      };
  std::ranges::for_each(held, release_missing);
  std::ranges::for_each(sorted, press_new);
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
// P2-08 extension: the game may ALSO export
//   [UnmanagedCallersOnly] static int Game.Bootstrap.Boot(
//       void* api_table, void* world, void* renderer)
// which receives the player's live pointers and builds the game into the
// player's own world (course entities + systems). Boot runs BEFORE the
// host is destroyed (its UCO pointer dies with the host). When Boot
// exists and returns 0, the player keeps the host ALIVE for the whole
// run (the game's per-frame systems execute managed code every
// world_frame) and destroys it after the loop; the legacy void Main
// path (HelloCube-era, no live pointers) destroys the host immediately.
//
// NOTE (CodeRabbit round 1): the void() convention stays for games that
// need no world pointer. Boot is additive: tried first, Main as fallback.
using BootstrapFn = void (*)();
using BootstrapBootFn = int (*)(const EngineApiTable*, EngineWorld*,
                                 EngineRenderer*);

struct BootHandle {
  engine::bridge::DotNetHost* host{nullptr};
  // Set when Boot ran and returned nonzero: the bootstrap failed AFTER
  // the host was already destroyed, so the frame loop must not run (a
  // partial Boot may have registered systems whose managed code would
  // execute on the dead host). Distinct from "no bootstrap" (demo path).
  bool failed{false};
};

// Run the Boot(table, world, renderer) entry (P2-08): the game builds into
// the player's live world. Returns true when Boot ran; Boot's nonzero rc
// destroys the host and returns false.
//
// NOTE (Sonar S3630 false positive): the hostfxr delegate arrives as
// void*; converting it to a function pointer with reinterpret_cast is the
// POSIX-documented (dlsym) conversion — static_cast cannot perform it.
// NOSONAR marks the two reviewed conversions only.
auto RunBoot3(void* boot_fn, const EngineApiTable* table, EngineWorld* world,
              EngineRenderer* renderer, engine::bridge::DotNetHost* host,
              const std::string& assembly_path, BootHandle* out_boot) -> bool {
  auto* boot3 = reinterpret_cast<BootstrapBootFn>(boot_fn);  // NOSONAR(cpp:S3630) hostfxr void*->fn (POSIX dlsym idiom)
  const int rc = boot3(table, world, renderer);
  if (rc != 0) {
    std::fprintf(stderr, "engine-player: game Boot failed rc=%d\n", rc);
    engine::bridge::dotnet_host_destroy(host);
    if (out_boot != nullptr) {
      out_boot->failed = true;
    }
    return false;
  }
  // Keep the host alive: the game's systems run managed code every
  // world_frame. The caller destroys it after the run loop.
  if (out_boot != nullptr) {
    out_boot->host = host;
  }
  std::printf("[PASS] player: game bootstrap ran (%s)\n",
              assembly_path.c_str());
  return true;
}

// Run the legacy void Main() entry (HelloCube-era, no live pointers) and
// destroy the host. The pointer is valid only until dotnet_host_destroy
// (hostfxr_close + dlclose), so the call happens while the host is alive
// (dotnet_host.hpp: "Valid until dotnet_host_destroy()").
auto RunBoot0(void* fn, engine::bridge::DotNetHost* host,
              const std::string& assembly_path) -> bool {
  auto* boot = reinterpret_cast<BootstrapFn>(fn);  // NOSONAR(cpp:S3630) hostfxr void*->fn (POSIX dlsym idiom)
  boot();
  engine::bridge::dotnet_host_destroy(host);
  std::printf("[PASS] player: game bootstrap ran (%s)\n",
              assembly_path.c_str());
  return true;
}

auto TryBootGame(const std::string& assembly_path,
                 const EngineApiTable* table = nullptr,
                 EngineWorld* world = nullptr, EngineRenderer* renderer = nullptr,
                 BootHandle* out_boot = nullptr) -> bool {
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
  // P2-08: prefer Boot(table, world, renderer) — the game builds into the
  // player's live world. Fall back to legacy void Main (no live pointers).
  void* boot_fn = engine::bridge::dotnet_host_get_system_pointer(
      host, assembly_path.c_str(), type_name.c_str(), "Boot");
  if (boot_fn != nullptr && table != nullptr && world != nullptr) {
    return RunBoot3(boot_fn, table, world, renderer, host, assembly_path,
                    out_boot);
  }
  // Boot missing (or no live world yet — pre-renderer probe path): void Main.
  void* legacy_fn = engine::bridge::dotnet_host_get_system_pointer(
      host, assembly_path.c_str(), type_name.c_str(), "Main");
  if (legacy_fn == nullptr) {
    // No bootstrap entry: destroy the host first (nothing escapes), then
    // fall back to the demo frame.
    engine::bridge::dotnet_host_destroy(host);
    std::printf("[INFO] player: no Game.Bootstrap.Main in %s (demo frame)\n",
                assembly_path.c_str());
    return false;
  }
  return RunBoot0(legacy_fn, host, assembly_path);
}

// Release owned engine resources on a failure path: destroy the boot host
// (when kept alive for Boot games), then the renderer and world. One
// helper instead of repeating the triple at every fail site (Sonar S134).
auto ReleasePlayer(const EngineApiTable* table, EngineWorld* world,
                   EngineRenderer* renderer, BootHandle& boot) -> void {
  if (boot.host != nullptr) {
    engine::bridge::dotnet_host_destroy(boot.host);
    boot.host = nullptr;
  }
  table->renderer_destroy(renderer);
  table->world_destroy(world);
}

// --- --content game.pak -----------------------------------------------------
// RAII holder for the content pack (M5-04 ENGPAK, P3-10 content): opens
// (mmap + header/TOC bounds verify) before boot, stays mapped for the run
// (cold-open proves the mapping survives boot + frames), unmaps on scope
// exit — close() is noexcept, so every return path releases it.
struct ContentPack {
  engine::assets::PakReader reader;

  auto Open(const std::string& path) -> bool {
    std::string err;
    if (!reader.open(path, err)) {
      std::printf("[FAIL] player: content pack %s: %s\n", path.c_str(),
                  err.c_str());
      return false;
    }
    std::printf("[PASS] player: content pack %s (%zu entries)\n",
                path.c_str(), reader.entry_count());
    return true;
  }
};

}  // namespace

auto PrintHelp() -> void {
  std::printf(
      "usage: engine-player <project_dir> [--scene <rel>] [--frames N] "
      "[--screenshot out.png] [--hidden] [--autoplay inputs.json] "
      "[--content game.pak]\n"
      "  --autoplay inputs.json: replay per-frame key states from JSON "
      "({\"frames\":[{\"keys\":[\"W\"]}]}) deterministically; "
      "exits 0 after the last script frame\n"
      "  --content game.pak: cold-open a content pack (ENGPAK, P3-10/M5-04); "
      "missing or corrupt pack is fatal (exit 2)\n");
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
                 "[--autoplay inputs.json] [--content game.pak]\n");
    return 1;
  }
  const std::string project_dir = argv[1];
  std::string scene_override;
  int frames = -1;
  std::string screenshot;
  bool hidden = false;
  std::string autoplay_path;
  std::string content_pack;
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
    } else if (a == "--content" && i + 1 < argc && argv[i + 1] != nullptr) {
      content_pack = argv[++i];
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
  const bool frames_explicit = (frames >= 0);
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
    if (std::string err; !LoadAutoplay(autoplay_path, autoplay, err)) {
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

  // Cold-boot clock: engine init (ABI table acquired) to first presented
  // frame. Uses steady_clock (monotonic, never wall-adjusted).
  const auto boot_start = std::chrono::steady_clock::now();

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
  BootHandle boot_handle;
  ContentPack content;
  if (!content_pack.empty()) {
    if (!content.Open(content_pack)) {
      ReleasePlayer(table, world, renderer, boot_handle);
      return 2;
    }
  }
  if (!game_assembly.empty()) {
    booted = TryBootGame(game_assembly, table, world, renderer, &boot_handle);
    if (boot_handle.failed) {
      ReleasePlayer(table, world, renderer, boot_handle);
      return 2;
    }
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
      ReleasePlayer(table, world, renderer, boot_handle);
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
      ReleasePlayer(table, world, renderer, boot_handle);
        return 2;
      }
      std::printf("[SKIP] player: no GPU mesh pipeline on this runner\n");
      ReleasePlayer(table, world, renderer, boot_handle);
      return 0;
    }
    if (!BuildDemoFrame(table, world, cube)) {
      ReleasePlayer(table, world, renderer, boot_handle);
      return 2;
    }
    std::printf("[PASS] player: demo frame built (HelloCube-equivalent)\n");
  }

  if (table->world_propagate(world) != 0) {
    std::printf("[FAIL] player: world_propagate (pre)\n");
      ReleasePlayer(table, world, renderer, boot_handle);
    return 2;
  }

  int shown = 0;
  bool cold_boot_reported = false;
  std::vector<std::uint32_t> autoplay_held;
  // Autoplay bounds the run to the script length: an omitted --frames
  // replays the whole script; an explicit --frames caps the replay at the
  // smaller of the request and the script (the old default-60 silently
  // truncated scripts longer than 60 frames).
  int run_frames = frames;
  if (use_autoplay) {
    const std::size_t script_len = autoplay.frames.size();
    if (!frames_explicit ||
        static_cast<std::size_t>(frames) > script_len) {
      run_frames = static_cast<int>(script_len);
    }
  }
  for (int f = 0; f < run_frames; ++f) {
    if (use_autoplay) {
      ApplyAutoplayFrame(autoplay.frames[static_cast<std::size_t>(f)].keys,
                         autoplay_held);
    }
    const std::int32_t steps = table->world_frame(world, 1.0 / 60.0);
    if (steps < 0) {
      std::printf("[FAIL] player: world_frame rc=%d\n", steps);
      ReleasePlayer(table, world, renderer, boot_handle);
      return 3;
    }
    if (table->world_propagate(world) != 0) {
      std::printf("[FAIL] player: world_propagate rc frame %d\n", f);
      ReleasePlayer(table, world, renderer, boot_handle);
      return 3;
    }
    const std::int32_t frc = table->renderer_frame(renderer);
    if (frc == 1) {
      break;  // window closed.
    }
    if (frc != 0) {
      std::printf("[FAIL] player: renderer_frame rc=%d\n", frc);
      ReleasePlayer(table, world, renderer, boot_handle);
      return 3;
    }
    if (!cold_boot_reported) {
      const auto first_frame = std::chrono::steady_clock::now();
      const double secs =
          std::chrono::duration<double>(first_frame - boot_start).count();
      std::printf("COLD_BOOT_S=%.3f\n", secs);
      cold_boot_reported = true;
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
      ReleasePlayer(table, world, renderer, boot_handle);
      return 3;
    }
    std::printf("[PASS] player: screenshot written: %s\n",
                screenshot.c_str());
  }

  ReleasePlayer(table, world, renderer, boot_handle);
  std::printf("PLAYER-OK\n");
  return 0;
}

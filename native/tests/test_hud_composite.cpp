// test_hud_composite.cpp — P5-04-followup acceptance (native CTest).
//
// GPU test through the REAL frame path: the ABI EngineRenderer owns the
// frame loop (RenderOne in renderer_bridge.cpp). When a GameUi is bound
// via renderer_attach_ui, RenderOne re-renders the HUD into its CPU
// backbuffer, staging-uploads it, and composites it OVER the frame as a
// textured fullscreen triangle (LOAD pass). This TU proves:
//   1. HUD-ON composite differs from HUD-OFF clear (the overlay is not a
//      transparent no-op — same alpha-proof shape as the P5-05 plume
//      differs-from-clear gate).
//   2. HUD-ON matches the CPU backbuffer the UI renders (the composite
//      carries RmlUi pixels, not a placeholder): downsample the 640x360
//      swapchain readback against ui_copy_frame bytes within golden
//      tolerance (±2/channel, ≤0.1% differing pixels).
//
// World: one main Camera + one DirectionalLight, NO meshes (mesh pass
// clears; the HUD is the whole image). 640x360 so the composite is 1:1
// with the backbuffer — no upscale blur to justify.
//
// Frame capture: renderer_screenshot writes the last presented RGBA frame
// to a PNG; GoldenLoadPng decodes it back (same stb decode the harness
// uses for checked-in goldens). The CPU backbuffer comes from the same
// ABI (ui_create/ui_render/ui_copy_frame) the frame composites.
//
// Rules: SDL_VIDEO_DRIVER=offscreen (ctest property); ENGINE_REQUIRE_GPU=1
// turns no-device paths into [FAIL] (gpu_required.hpp), never [SKIP].
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "engine/bridge/engine_abi.h"
#include "engine/ecs/ecs_render_components.hpp"
#include "engine/ecs/ecs_world.hpp"
#include "engine/ecs/transform.hpp"
#include "engine/ui/game_ui.hpp"
#include "flecs.h"
#include "golden_compare.hpp"
#include "gpu_required.hpp"

namespace {

using engine::ecs::Camera;
using engine::ecs::DirectionalLight;
using engine::ecs::LocalTransform;
using engine::ecs::RegisterRenderComponent;
using engine::ecs::WorldTransform;

auto failures = 0;

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}

// Real-type layout guards (same static_asserts as test_ecs_render.cpp):
// the ABI component_register sizes below must match these exactly.
static_assert(sizeof(Camera) == 16, "Camera is 16 bytes");
static_assert(alignof(Camera) == 4, "Camera align drifted");
static_assert(sizeof(DirectionalLight) == 16, "DirectionalLight is 16 bytes");
static_assert(sizeof(LocalTransform) == 64, "LocalTransform is 64 bytes");
static_assert(sizeof(WorldTransform) == 64, "WorldTransform is 64 bytes");

auto make_local(float tx, float ty, float tz) -> LocalTransform {
  LocalTransform l{};
  l.px = tx;
  l.py = ty;
  l.pz = tz;
  // Value-init zeroes quat/scale, so set them explicitly: propagate()
  // composes World from these.
  l.qw = 1.0F;
  l.sx = 1.0F;
  l.sy = 1.0F;
  l.sz = 1.0F;
  return l;
}

// Minimal camera+light world (no meshes, no emitters). Returns nullptr on
// failure (check already recorded).
auto make_world(const EngineApiTable* api) -> EngineWorld* {
  EngineWorld* world = api->world_create();
  if (world == nullptr) {
    check(false, "hud-composite: world_create");
    return nullptr;
  }
  const std::uint64_t cam_id = api->component_register(
      world, "Engine.Runtime.Camera", sizeof(Camera), alignof(Camera));
  const std::uint64_t light_id = api->component_register(
      world, "Engine.Runtime.DirectionalLight", sizeof(DirectionalLight),
      alignof(DirectionalLight));
  const std::uint64_t local_id = api->component_register(
      world, "LocalTransform", sizeof(LocalTransform),
      alignof(LocalTransform));
  bool ok = cam_id != 0 && light_id != 0 && local_id != 0;
  if (!ok) {
    check(false, "hud-composite: component_register");
    api->world_destroy(world);
    return nullptr;
  }
  {
    const std::uint64_t e = api->entity_create(world);
    Camera c{};
    c.fov_y_rad = 60.0F * 3.14159265F / 180.0F;
    c.near_z = 0.1F;
    c.far_z = 100.0F;
    c.is_main = 1;
    ok = ok && api->entity_set(world, e, cam_id, &c, sizeof(c)) == 0;
    const LocalTransform lt = make_local(0.0F, 1.2F, 4.0F);
    ok = ok && api->entity_set(world, e, local_id, &lt, sizeof(lt)) == 0;
  }
  {
    const std::uint64_t e = api->entity_create(world);
    DirectionalLight l{};
    l.direction[0] = -0.4F;
    l.direction[1] = -0.7F;
    l.direction[2] = -0.5F;
    ok = ok && api->entity_set(world, e, light_id, &l, sizeof(l)) == 0;
  }
  ok = ok && api->world_propagate(world) == 0;
  if (!ok) {
    check(false, "hud-composite: world build");
    api->world_destroy(world);
    return nullptr;
  }
  return world;
}

// Present `frames` frames on `renderer`, then screenshot to `png_path` and
// decode it. Empty on failure (check already recorded); `no_device_out`
// stays false — renderer creation is the caller's no-device gate.
auto shot(const EngineApiTable* api, EngineRenderer* renderer,
          const std::string& png_path, std::uint32_t w, std::uint32_t h,
          int frames, const char* tag) -> std::vector<std::uint8_t> {
  for (int f = 0; f < frames; ++f) {
    if (api->renderer_frame(renderer) != 0) {
      check(false, tag);  // post-creation frame failure: hard FAIL
      return {};
    }
  }
  if (api->renderer_screenshot(renderer, png_path.c_str()) != 0) {
    check(false, tag);  // post-creation capture failure: hard FAIL
    return {};
  }
  std::vector<std::uint8_t> px;
  int pw = 0;
  int ph = 0;
  if (!engine::test::GoldenLoadPng(png_path, px, pw, ph) ||
      pw != static_cast<int>(w) || ph != static_cast<int>(h) ||
      px.size() != static_cast<std::size_t>(w) * h * 4) {
    check(false, tag);  // post-creation decode failure: hard FAIL
    return {};
  }
  return px;
}

void test_hud_composite() {
  // 640x360 = the HUD backbuffer size: the composite is 1:1, so the
  // screenshot compares against ui_copy_frame bytes with no resampling.
  constexpr std::uint32_t kW = engine::ui::kUiFrameW;
  constexpr std::uint32_t kH = engine::ui::kUiFrameH;
  constexpr int kFrames = 8;

  const EngineApiTable* api = engine_get_api_table();
  check(api != nullptr, "hud-composite: api table non-NULL");
  if (api == nullptr) {
    return;
  }
  check(api->renderer_attach_ui != nullptr,
        "hud-composite: renderer_attach_ui set");
  if (api->renderer_attach_ui == nullptr) {
    return;
  }

  std::string out_dir = ENGINE_GOLDEN_OUT_DIR;
  if (!out_dir.empty() && out_dir.back() != '/') {
    out_dir.push_back('/');
  }
  (void)engine::test::GoldenMkdirP(out_dir);
  const std::string on_png = out_dir + "p5_04_hud_composite_640x360.shot.png";
  const std::string off_png =
      out_dir + "p5_04_hud_composite_off.shot.png";

  EngineWorld* world = make_world(api);
  if (world == nullptr) {
    return;
  }
  EngineRenderer* renderer =
      api->renderer_create(world, kW, kH, ENGINE_RENDERER_HIDDEN);
  if (renderer == nullptr) {
    api->world_destroy(world);
    // Renderer creation itself failed: no device or no display is a
    // skip (gpu_missing returns 1 under ENGINE_REQUIRE_GPU=1).
    failures += gpu_missing("hud-composite: renderer created");
    return;
  }
  check(true, "hud-composite: renderer created");

  // ABI misuse gates (fail closed, no GPU touched).
  check(api->renderer_attach_ui(nullptr, nullptr) != 0,
        "hud-composite: null renderer fails");
#ifdef ENGINE_UI_FONT_DIR
  constexpr const char* kFontDir = ENGINE_UI_FONT_DIR;
#else
  constexpr const char* kFontDir = ".";
#endif
  EngineUi* ui = api->ui_create(kFontDir);
  check(ui != nullptr, "hud-composite: ui_create");
  if (ui == nullptr) {
    api->renderer_destroy(renderer);
    api->world_destroy(world);
    return;
  }

  // Golden fixture: the default HUD from P5-04 (SCORE 0 / START).
  check(api->ui_set_text(ui, "lbl", "SCORE 0") == 0,
        "hud-composite: golden label reset");
  check(api->ui_set_text(ui, "btn", "START") == 0,
        "hud-composite: golden button reset");
  std::vector<std::uint8_t> cpu(static_cast<std::size_t>(kW) * kH * 4, 0);
  check(api->ui_render(ui) == 0, "hud-composite: cpu hud renders");
  check(api->ui_copy_frame(ui, cpu.data(),
                           static_cast<std::uint32_t>(cpu.size())) == 0,
        "hud-composite: cpu hud copied");

  // 1. HUD-OFF baseline: cleared frame (mesh pass CLEARs to black; the
  // default HUD top bar #2e3440 is opaque so HUD-ON must differ).
  const std::vector<std::uint8_t> off =
      shot(api, renderer, off_png, kW, kH, kFrames,
           "hud-composite: hud-off frames presented");
  check(!off.empty(), "hud-composite: hud-off frames presented");

  // 2. Bind, then HUD-ON: RenderOne composites the HUD OVER the frame.
  check(api->renderer_attach_ui(renderer, ui) == 0,
        "hud-composite: renderer_attach_ui binds");
  const std::vector<std::uint8_t> on = shot(api, renderer, on_png, kW, kH,
                                            kFrames,
                                            "hud-composite: hud-on rendered");
  check(!on.empty(), "hud-composite: hud-on frames presented");

  if (!off.empty() && !on.empty()) {
    const bool differs =
        off.size() == on.size() &&
        std::memcmp(off.data(), on.data(), on.size()) != 0;
    check(differs, "hud-composite: hud-on differs from hud-off");
    if (differs) {
      // The composite must carry the RmlUi pixels: compare the readback
      // against the CPU backbuffer within golden tolerance (±2/channel,
      // ≤0.1% differing pixels — the same gate as golden_compare), RGB
      // channels only. Alpha is presentation-destroyed by design: the
      // swapchain is opaque (capture alpha is always 255) while the CPU
      // backbuffer carries real alpha (0 in empty regions, partial on
      // glyph edges), so the A channel can never round-trip. The blend
      // state (ONE/ONE_MINUS_SRC_ALPHA over the black mesh-pass clear)
      // reproduces the premultiplied CPU RGB exactly.
      std::size_t diff_px = 0;
      const std::size_t npx = static_cast<std::size_t>(kW) * kH;
      for (std::size_t p = 0; p < npx; ++p) {
        bool diff = false;
        for (int c = 0; c < 3; ++c) {
          const int a = static_cast<int>(on[p * 4 + c]);
          const int b = static_cast<int>(cpu[p * 4 + c]);
          const int d = a >= b ? a - b : b - a;
          if (d > engine::test::kGoldenChannelTol) {
            diff = true;
            break;
          }
        }
        if (diff) {
          ++diff_px;
        }
      }
      const double frac =
          static_cast<double>(diff_px) / static_cast<double>(npx);
      check(frac <= engine::test::kGoldenMaxFrac,
            "hud-composite: composite matches cpu backbuffer");
    }
    // 3. Unbind restores the cleared frame (binding is borrow + clear).
    check(api->renderer_attach_ui(renderer, nullptr) == 0,
          "hud-composite: renderer_attach_ui unbinds");
    const std::vector<std::uint8_t> cleared =
        shot(api, renderer, off_png, kW, kH, 2,
             "hud-composite: unbound frames presented");
    if (!cleared.empty()) {
      check(std::memcmp(cleared.data(), off.data(), off.size()) == 0,
            "hud-composite: unbind restores clear");
    }
  }

  api->ui_destroy(ui);
  api->renderer_destroy(renderer);
  api->world_destroy(world);
}

}  // namespace

auto main() -> int {
  // Unbuffered stdout: on a segfault the CI log keeps every PASS line,
  // pinpointing the crashing section (Windows gives no core dump).
  setvbuf(stdout, nullptr, _IONBF, 0);
  test_hud_composite();
  std::printf(failures == 0 ? "HUD_COMPOSITE_OK\n"
                            : "HUD_COMPOSITE_FAILURES=%d\n",
              failures);
  return failures == 0 ? 0 : 1;
}

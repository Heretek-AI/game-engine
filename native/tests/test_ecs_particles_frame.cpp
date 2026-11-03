// test_ecs_particles_frame.cpp — P5-05-followup acceptance (native CTest).
//
// GPU test through the REAL frame path: the ABI EngineRenderer owns the
// frame loop (RenderOne in renderer_bridge.cpp), which steps GPU particle
// emitters (ParticleGpuSim fence-waited one-shot) and composites their
// billboards OVER the mesh frame (LOAD particle pass). This TU builds the
// world via the ABI (world_create/entity_create/entity_set), creates a
// hidden-window renderer, runs 60 frames, and compares the last presented
// frame against golden p5_05_ecs_frame_320x240.
//
// Scene: one main Camera + one DirectionalLight + one emitter
// (Engine.Runtime.ParticleSystem, 120/s, 2 s life, seed 1234) + NO
// meshes (mesh pass draws nothing; the plume is the whole image).
// A second emitter-free run must render a background-clear frame that
// differs (proves the plume comes from the particle path).
//
// Frame capture: renderer_screenshot writes the last presented RGBA frame
// to a PNG; GoldenLoadPng decodes it back for golden_compare (same
// stb decode the harness uses for checked-in goldens).
//
// Rules: SDL_VIDEO_DRIVER=offscreen (ctest property); ENGINE_REQUIRE_GPU=1
// turns no-device paths into [FAIL] (gpu_required.hpp), never [SKIP].
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/bridge/engine_abi.h"
#include "golden_compare.hpp"
#include "gpu_required.hpp"

namespace {

auto failures = 0;

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}

// 16-byte Camera mirror (Engine.Runtime.Camera).
struct AbiCamera {
  float fov_y_rad{0.0F};
  float near_z{0.0F};
  float far_z{0.0F};
  std::uint32_t is_main{0};
};
static_assert(sizeof(AbiCamera) == 16, "camera mirror is 16 bytes");

// 16-byte DirectionalLight mirror.
struct AbiLight {
  float direction[3]{0.0F, 0.0F, 0.0F};
  float shadow{0.0F};
};
static_assert(sizeof(AbiLight) == 16, "light mirror is 16 bytes");

// 64-byte LocalTransform mirror: pos + identity quat + unit scale + pad.
struct AbiLocal {
  float px{0.0F};
  float py{0.0F};
  float pz{0.0F};
  float qx{0.0F};
  float qy{0.0F};
  float qz{0.0F};
  float qw{1.0F};
  float sx{1.0F};
  float sy{1.0F};
  float sz{1.0F};
  float pad[6]{};
};
static_assert(sizeof(AbiLocal) == 64, "local mirror is 64 bytes");

// 76-byte ParticleSystemComponent mirror.
struct AbiParticles {
  float emission_rate{0.0F};
  float burst_count{0.0F};
  float burst_interval{0.0F};
  float lifetime{1.0F};
  float start_speed{1.0F};
  float start_size{1.0F};
  float start_color[4]{1.0F, 1.0F, 1.0F, 1.0F};
  float gravity{0.0F};
  float end_color[4]{1.0F, 1.0F, 1.0F, 0.0F};
  float end_size{1.0F};
  std::uint32_t blend{0};
  std::uint32_t seed{0};
  std::uint32_t pad{0};
};
static_assert(sizeof(AbiParticles) == 76, "particles mirror is 76 bytes");

auto make_local(float tx, float ty, float tz) -> AbiLocal {
  AbiLocal l{};
  l.px = tx;
  l.py = ty;
  l.pz = tz;
  return l;
}

// Build + render one world; when `with_emitter` is false the world has
// camera + light only. Returns the decoded screenshot RGBA (empty on
// failure). `tag` prefixes check names; `shot_path` receives the PNG.
// `no_device_out` is set true ONLY when renderer creation itself failed
// (no GPU device/display) — post-creation frame/capture/decode failures
// return empty with the flag clear so the caller FAILs instead of
// skipping the golden comparison.
auto run_world(const EngineApiTable* api, bool with_emitter,
               const char* tag, const std::string& shot_path,
               std::uint32_t w, std::uint32_t h, int frames,
               bool& no_device_out)
    -> std::vector<std::uint8_t> {
  EngineWorld* world = api->world_create();
  no_device_out = false;
  if (world == nullptr) {
    check(false, tag);
    return {};
  }
  const std::uint64_t cam_id =
      api->component_register(world, "Engine.Runtime.Camera", 16, 4);
  const std::uint64_t light_id = api->component_register(
      world, "Engine.Runtime.DirectionalLight", 16, 4);
  const std::uint64_t particle_id = api->component_register(
      world, "Engine.Runtime.ParticleSystem", 76, 4);
  const std::uint64_t local_id =
      api->component_register(world, "LocalTransform", 64, 64);
  bool ok = cam_id != 0 && light_id != 0 && local_id != 0 &&
            (!with_emitter || particle_id != 0);
  if (!ok) {
    check(false, tag);
    api->world_destroy(world);
    return {};
  }
  {
    const std::uint64_t e = api->entity_create(world);
    AbiCamera c{};
    c.fov_y_rad = 60.0F * 3.14159265F / 180.0F;
    c.near_z = 0.1F;
    c.far_z = 100.0F;
    c.is_main = 1;
    ok = ok && api->entity_set(world, e, cam_id, &c, sizeof(c)) == 0;
    const AbiLocal lt = make_local(0.0F, 1.2F, 4.0F);
    ok = ok && api->entity_set(world, e, local_id, &lt, sizeof(lt)) == 0;
  }
  {
    const std::uint64_t e = api->entity_create(world);
    AbiLight l{};
    l.direction[0] = -0.4F;
    l.direction[1] = -0.7F;
    l.direction[2] = -0.5F;
    ok = ok && api->entity_set(world, e, light_id, &l, sizeof(l)) == 0;
  }
  if (with_emitter) {
    const std::uint64_t e = api->entity_create(world);
    AbiParticles p{};
    p.emission_rate = 120.0F;
    p.lifetime = 2.0F;
    p.start_speed = 3.0F;
    p.start_size = 0.5F;
    p.start_color[0] = 1.0F;
    p.start_color[1] = 0.9F;
    p.start_color[2] = 0.6F;
    p.start_color[3] = 1.0F;
    p.gravity = -2.0F;
    p.end_color[0] = 1.0F;
    p.end_color[1] = 0.5F;
    p.end_color[2] = 0.1F;
    p.end_color[3] = 0.0F;
    p.end_size = 0.5F;
    p.blend = 0;
    p.seed = 1234;
    ok = ok && api->entity_set(world, e, particle_id, &p, sizeof(p)) == 0;
    const AbiLocal lt = make_local(0.0F, 0.0F, 0.0F);
    ok = ok && api->entity_set(world, e, local_id, &lt, sizeof(lt)) == 0;
  }
  ok = ok && api->world_propagate(world) == 0;
  if (!ok) {
    check(false, tag);
    api->world_destroy(world);
    return {};
  }
  EngineRenderer* renderer =
      api->renderer_create(world, w, h, ENGINE_RENDERER_HIDDEN);
  if (renderer == nullptr) {
    api->world_destroy(world);
    no_device_out = true;  // no device/display: caller decides skip/fail
    return {};             // (gpu_missing honors ENGINE_REQUIRE_GPU)
  }
  for (int f = 0; f < frames && ok; ++f) {
    if (api->renderer_frame(renderer) != 0) {
      check(false, tag);  // post-creation frame failure: hard FAIL
      ok = false;
    }
  }
  std::vector<std::uint8_t> px;
  if (ok) {
    if (api->renderer_screenshot(renderer, shot_path.c_str()) != 0) {
      check(false, tag);  // post-creation capture failure: hard FAIL
      ok = false;
    } else {
      int pw = 0;
      int ph = 0;
      if (!engine::test::GoldenLoadPng(shot_path, px, pw, ph) ||
          pw != static_cast<int>(w) || ph != static_cast<int>(h) ||
          px.size() != static_cast<std::size_t>(w) * h * 4) {
        check(false, tag);  // post-creation decode failure: hard FAIL
        ok = false;
        px.clear();
      }
    }
  }
  api->renderer_destroy(renderer);
  api->world_destroy(world);
  if (!ok) {
    px.clear();
  }
  return px;
}

void test_ecs_particles_frame() {
  constexpr std::uint32_t kW = 320;
  constexpr std::uint32_t kH = 240;
  constexpr int kFrames = 60;

  const EngineApiTable* api = engine_get_api_table();
  check(api != nullptr, "ecs-particles: api table non-NULL");
  if (api == nullptr) {
    return;
  }

  std::string out_dir = ENGINE_GOLDEN_OUT_DIR;
  if (!out_dir.empty() && out_dir.back() != '/') {
    out_dir.push_back('/');
  }
  const std::string plume_png = out_dir + "p5_05_ecs_frame_320x240.shot.png";
  const std::string empty_png = out_dir + "p5_05_ecs_frame_empty.shot.png";
  (void)engine::test::GoldenMkdirP(out_dir);

  std::vector<std::uint8_t> plume;
  bool no_device = false;
  plume = run_world(api, true, "ecs-particles: plume world", plume_png, kW,
                    kH, kFrames, no_device);
  check(!plume.empty(), "ecs-particles: renderer created");
  if (plume.empty()) {
    if (no_device) {
      // Renderer creation itself failed: no device or no display is a
      // skip (gpu_missing returns 1 under ENGINE_REQUIRE_GPU=1).
      failures += gpu_missing("ecs-particles: plume render (no device)");
      return;
    }
    // Post-creation frame/capture/decode already recorded its FAIL
    // above; an empty image here must never pass the golden gate.
    check(false, "ecs-particles: plume world rendered");
    return;
  }
  check(true, "ecs-particles: 60 plume frames presented");
  failures += engine::test::golden_check(
      "p5_05_ecs_frame_320x240",
      std::span<const std::uint8_t>(plume.data(), plume.size()), kW, kH);

  std::vector<std::uint8_t> empty;
  bool empty_no_device = false;
  empty = run_world(api, false, "ecs-particles: empty world", empty_png, kW,
                    kH, kFrames, empty_no_device);
  if (empty_no_device) {
    // Same no-device path as the plume run (already skip-counted); the
    // empty run adds nothing when no renderer exists.
    return;
  }
  check(!empty.empty(), "ecs-particles: empty frames presented");
  if (!empty.empty()) {
    const bool differs =
        empty.size() == plume.size() &&
        std::memcmp(empty.data(), plume.data(), plume.size()) != 0;
    check(differs, "ecs-particles: plume differs from clear");
  }
}

}  // namespace

auto main() -> int {
  test_ecs_particles_frame();
  std::printf(failures == 0 ? "ECS_PARTICLES_FRAME_OK\n"
                            : "ECS_PARTICLES_FRAME_FAILURES=%d\n",
              failures);
  return failures == 0 ? 0 : 1;
}

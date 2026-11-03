// test_ecs_render.cpp — P1-08 acceptance (native CTest).
//
// GPU test: opens a 640x360 window, builds the ForwardRenderer mesh
// pipeline (D32_SFLOAT depth), uploads one shared unit cube, spawns a
// 10x10x10 grid of cubes (1000 entities) through Flecs — each with
// LocalTransform + WorldTransform + MeshRenderer — plus one main Camera
// and one DirectionalLight (registered via EcsWorld::register_render
// under Engine.Runtime.*), propagates, renders via
// ForwardRenderer::render_world, and compares against golden
// grid_1000_640x360. Moving one entity's LocalTransform and
// re-propagating must change the next frame's image. Prints:
//
//   [PASS] golden: grid_1000_640x360
//   [PASS] ecs-render: 1000 draws
//   [PASS] ecs-render: transform change visible
//
// Rules: SDL_VIDEO_DRIVER=offscreen (ctest property); ENGINE_REQUIRE_GPU=1
// turns no-device paths into [FAIL] (gpu_required.hpp), never [SKIP].
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#endif

#include "engine/ecs/ecs_world.hpp"
#include "engine/ecs/transform.hpp"
#include "engine/renderer/forward_renderer.hpp"
#include "engine/renderer/gpu_mesh.hpp"
#include "engine/renderer/mesh_registry.hpp"
#include "engine/renderer/renderer_window.hpp"
#include "engine/renderer/swapchain.hpp"
#include "engine/renderer/vulkan_device.hpp"
#include "flecs.h"
#include "golden_compare.hpp"
#include "gpu_required.hpp"
#include "mesh_test_helpers.hpp"

namespace {

auto failures = 0;

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}

using engine::ecs::Camera;
using engine::ecs::DirectionalLight;
using engine::ecs::EcsWorld;
using engine::ecs::LocalTransform;
using engine::ecs::MeshRenderer;
using engine::ecs::WorldTransform;
using engine::renderer::DepthBuffer;
using engine::renderer::ForwardRenderer;
using engine::renderer::FrameContext;
using engine::renderer::GpuMesh;
using engine::renderer::MeshPush;
using engine::renderer::MeshRegistry;
using engine::renderer::MeshVertex;
using engine::renderer::RendererWindow;
using engine::renderer::StatusCode;
using engine::renderer::Swapchain;
using engine::renderer::VulkanDevice;
using engine::renderer::WindowConfig;

// Spec §P1-08 layouts (SDK-free, checked in both modes).
static_assert(sizeof(Camera) == 16, "Camera is 16 bytes");
static_assert(alignof(Camera) == 4, "Camera align drifted");
static_assert(sizeof(MeshRenderer) == 20, "MeshRenderer is 20 bytes");
static_assert(alignof(MeshRenderer) == 4, "MeshRenderer align drifted");
static_assert(sizeof(DirectionalLight) == 16, "DirectionalLight is 16 bytes");
static_assert(alignof(DirectionalLight) == 4, "DirectionalLight align drifted");
static_assert(sizeof(WorldTransform) == 64, "WorldTransform is 64 bytes");

#if defined(ENGINE_RENDERER_HAS_VULKAN)

// P5-02: cube/barrier scaffolding now lives in mesh_test_helpers.hpp
// (shared with test_forward_mesh + test_pick_ids; Sonar gate).
using Cube = engine::test::HelperCube;
using engine::test::make_helper_cube;

void test_ecs_render_gpu() {
  constexpr std::uint32_t kW = 640;
  constexpr std::uint32_t kH = 360;

  engine::test::HelperGpuWindow gw{};
  auto missing = [](const char* what) -> int { return gpu_missing(what); };
  if (!engine::test::helper_gpu_window(
          "P1-08-test", kW, kH,
          "ecs-render: offscreen window unavailable (no video)",
          "ecs-render: window instance extensions query",
          "ecs-render: no physical device (no ICD)",
          "ecs-render: device init ok", "ecs-render: surface create ok",
          "ecs-render: swapchain create ok", check, missing, &failures,
          &gw)) {
    return;
  }
  RendererWindow& w = gw.w;
  VulkanDevice& dev = gw.dev;
  VkSurfaceKHR surface = gw.surface;
  Swapchain& sc = gw.sc;
  ForwardRenderer fr;
  auto mst = fr.create_mesh(dev, sc.format(), VK_FORMAT_D32_SFLOAT);
  check(mst.ok() && fr.mesh_ready(), "ecs-render: mesh pipeline built");
  if (!mst.ok()) {
    sc.destroy();
    dev.destroy_surface(surface);
    dev.shutdown();
    w.destroy();
    return;
  }

  DepthBuffer depth;
  auto dst = depth.create(dev, sc.extent(), VK_FORMAT_D32_SFLOAT);
  check(dst.ok() && depth.ready(), "ecs-render: depth buffer created");
  if (!dst.ok()) {
    fr.destroy();
    sc.destroy();
    dev.destroy_surface(surface);
    dev.shutdown();
    w.destroy();
    return;
  }

  Cube cube = make_helper_cube();
  GpuMesh mesh;
  auto ust = mesh.create(dev, cube.verts.data(), 24, cube.idx.data(), 36);
  check(ust.ok() && mesh.ready(), "ecs-render: cube uploaded (24v/36i)");
  if (!ust.ok()) {
    depth.destroy();
    fr.destroy();
    sc.destroy();
    dev.destroy_surface(surface);
    dev.shutdown();
    w.destroy();
    return;
  }
  MeshRegistry registry;
  const std::uint32_t cube_id = registry.add(&mesh);
  check(cube_id != 0, "ecs-render: mesh registered");

  // ---- Flecs world: 10x10x10 grid + camera + light -------------------
  EcsWorld world;
  check(world.construct(), "ecs-render: world constructed");
  if (!world.valid()) {
    mesh.destroy();
    depth.destroy();
    fr.destroy();
    sc.destroy();
    dev.destroy_surface(surface);
    dev.shutdown();
    w.destroy();
    return;
  }
  check(world.register_render(), "ecs-render: render components registered");
  // Idempotency: registering twice returns success (same ids, no abort).
  check(world.register_render(), "ecs-render: render register idempotent");

  const ecs_entity_t cam_comp = ecs_lookup_path_w_sep(
      world.raw(), 0, "Engine.Runtime.Camera", "", "", false);
  const ecs_entity_t mesh_comp = ecs_lookup_path_w_sep(
      world.raw(), 0, "Engine.Runtime.MeshRenderer", "", "", false);
  const ecs_entity_t light_comp = ecs_lookup_path_w_sep(
      world.raw(), 0, "Engine.Runtime.DirectionalLight", "", "", false);
  check(cam_comp != 0 && mesh_comp != 0 && light_comp != 0,
        "ecs-render: component ids resolve");
  check(world.local_id() != 0 && world.world_id() != 0,
        "ecs-render: transform ids valid");
  if (cam_comp == 0 || mesh_comp == 0 || light_comp == 0) {
    world.destroy();
    mesh.destroy();
    depth.destroy();
    fr.destroy();
    sc.destroy();
    dev.destroy_surface(surface);
    dev.shutdown();
    w.destroy();
    return;
  }
  const ecs_entity_t local_id =
      static_cast<ecs_entity_t>(world.local_id());
  const ecs_entity_t world_id =
      static_cast<ecs_entity_t>(world.world_id());

  // Camera entity: eye (27, 27, 44) looking at the grid centre
  // (9, 9, 9) — a 10-wide grid of unit cubes at 2-unit spacing spans
  // 0..18 per axis (cube faces reach +/-0.5 past the outer centres, so
  // the occupied box is roughly -0.5..18.5). The eye sits ~40 units back
  // along +Z and above the centre so the whole grid frames in the
  // 640x360 view at 60deg FOV. WorldTransform written directly (rigid
  // lookAt basis + eye translation); propagate() only recomputes World
  // from Local, so the eye must bypass it.
  const float kFov = 60.0F * 3.14159265F / 180.0F;
  {
    const ecs_entity_t e = ecs_new(world.raw());
    Camera c{};
    c.fov_y_rad = kFov;
    c.near_z = 0.1F;
    c.far_z = 200.0F;
    c.is_main = 1;
    ecs_set_id(world.raw(), e, cam_comp, sizeof(Camera), &c);
    // f = centre - eye.
    const float cx = 9.0F;
    const float cy = 9.0F;
    const float cz = 9.0F;
    const float ex = 27.0F;
    const float ey = 27.0F;
    const float ez = 44.0F;
    float fwd[3] = {cx - ex, cy - ey, cz - ez};
    const float fwd_l =
        std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
    fwd[0] /= fwd_l;
    fwd[1] /= fwd_l;
    fwd[2] /= fwd_l;
    float sd[3] = {fwd[1] * 0.0F - fwd[2] * 1.0F, fwd[2] * 0.0F - fwd[0] * 0.0F,
                   fwd[0] * 1.0F - fwd[1] * 0.0F};
    const float sd_l = std::sqrt(sd[0] * sd[0] + sd[1] * sd[1] + sd[2] * sd[2]);
    sd[0] /= sd_l;
    sd[1] /= sd_l;
    sd[2] /= sd_l;
    const float up[3] = {sd[1] * fwd[2] - sd[2] * fwd[1],
                         sd[2] * fwd[0] - sd[0] * fwd[2],
                         sd[0] * fwd[1] - sd[1] * fwd[0]};
    WorldTransform wt{};
    wt.m[0] = sd[0];
    wt.m[1] = up[0];
    wt.m[2] = -fwd[0];
    wt.m[3] = 0.0F;
    wt.m[4] = sd[1];
    wt.m[5] = up[1];
    wt.m[6] = -fwd[1];
    wt.m[7] = 0.0F;
    wt.m[8] = sd[2];
    wt.m[9] = up[2];
    wt.m[10] = -fwd[2];
    wt.m[11] = 0.0F;
    wt.m[12] = ex;
    wt.m[13] = ey;
    wt.m[14] = ez;
    wt.m[15] = 1.0F;
    ecs_set_id(world.raw(), e, world_id, sizeof(WorldTransform), &wt);
  }
  // Directional light: same convention as P1-07 (direction light
  // travels; shader uses dot(n, -light_dir)).
  {
    const ecs_entity_t e = ecs_new(world.raw());
    DirectionalLight l{};
    const float len = std::sqrt(0.25F + 0.81F + 0.4225F);
    l.direction[0] = -0.5F / len;
    l.direction[1] = -0.9F / len;
    l.direction[2] = -0.65F / len;
    l.shadow = 1.0F;  // P4-03: per-light shadow toggle on
    ecs_set_id(world.raw(), e, light_comp, sizeof(DirectionalLight), &l);
  }

  // 1000 grid entities: 2-unit spacing, unit cubes, tan base color.
  std::vector<ecs_entity_t> grid;
  grid.reserve(1000);
  for (int ix = 0; ix < 10; ++ix) {
    for (int iy = 0; iy < 10; ++iy) {
      for (int iz = 0; iz < 10; ++iz) {
        const ecs_entity_t e = ecs_new(world.raw());
        LocalTransform lt{};
        lt.px = static_cast<float>(ix) * 2.0F;
        lt.py = static_cast<float>(iy) * 2.0F;
        lt.pz = static_cast<float>(iz) * 2.0F;
        lt.qx = 0.0F;
        lt.qy = 0.0F;
        lt.qz = 0.0F;
        lt.qw = 1.0F;
        lt.sx = 1.0F;
        lt.sy = 1.0F;
        lt.sz = 1.0F;
        ecs_set_id(world.raw(), e, local_id, sizeof(LocalTransform), &lt);
        WorldTransform wt{};
        engine::ecs::ComposeLocal(lt, wt.m);
        ecs_set_id(world.raw(), e, world_id, sizeof(WorldTransform), &wt);
        MeshRenderer mr{};
        mr.mesh_id = cube_id;
        mr.base_color[0] = 0.85F;
        mr.base_color[1] = 0.5F;
        mr.base_color[2] = 0.2F;
        mr.base_color[3] = 1.0F;
        ecs_set_id(world.raw(), e, mesh_comp, sizeof(MeshRenderer), &mr);
        grid.push_back(e);
      }
    }
  }
  check(grid.size() == 1000, "ecs-render: 1000 entities spawned");
  check(world.propagate(), "ecs-render: world propagated");

  auto p_vkCmdPipelineBarrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(
      dev.proc("vkCmdPipelineBarrier2"));
  check(p_vkCmdPipelineBarrier2 != nullptr, "ecs-render: barrier entry resolves");
  if (p_vkCmdPipelineBarrier2 == nullptr) {
    world.destroy();
    mesh.destroy();
    depth.destroy();
    fr.destroy();
    sc.destroy();
    dev.destroy_surface(surface);
    dev.shutdown();
    w.destroy();
    return;
  }

  FrameContext fc;
  auto fcst = fc.create(dev, sc);
  check(fcst.ok(), "ecs-render: frame context create ok");
  if (!fcst.ok()) {
    world.destroy();
    mesh.destroy();
    depth.destroy();
    fr.destroy();
    sc.destroy();
    dev.destroy_surface(surface);
    dev.shutdown();
    w.destroy();
    return;
  }

  engine::test::HelperBarriers bars{};
  bars.cmd_barrier2 = p_vkCmdPipelineBarrier2;
  bars.depth_image = reinterpret_cast<VkImage>(
      static_cast<std::uintptr_t>(depth.image_bits()));

  // Render one frame through render_world; `first_use` drives the depth
  // image's UNDEFINED->ATTACHMENT transition exactly once.
  bool depth_live = false;
  ForwardRenderer::RenderWorldStats last_stats{};
  auto render_frame = [&](std::vector<std::uint8_t>& out) -> bool {
    engine::test::HelperFrame hf{};
    if (!engine::test::helper_frame_open(fr, fc, sc, depth, bars, depth_live,
                                         &hf)) {
      return false;
    }
    VkCommandBuffer cmd = hf.cmd;
    VkImage img = hf.img;
    ForwardRenderer::RenderWorldStats stats{};
    if (!fr.render_world(world.raw(), registry, cmd, nullptr, &stats)
             .ok()) {
      return false;
    }
    last_stats = stats;
    if (!fr.end_mesh_pass(cmd).ok()) {
      return false;
    }
    bars.color(
        cmd, img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_TRANSFER_READ_BIT);
    if (!fc.end_frame(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL).ok()) {
      return false;
    }
    if (!capture_rgba8(dev, img, sc.format(), sc.extent(),
                       VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, out)
             .ok()) {
      return false;
    }
    return out.size() == static_cast<std::size_t>(kW) * kH * 4;
  };

  std::vector<std::uint8_t> px_grid;
  check(render_frame(px_grid), "ecs-render: grid frame captured");
  bool grid_ok = !px_grid.empty();
  if (grid_ok) {
    failures += engine::test::golden_check(
        "grid_1000_640x360",
        std::span<const std::uint8_t>(px_grid.data(), px_grid.size()), kW,
        kH);
    check(last_stats.draws == 1000, "ecs-render: 1000 draws");
    if (last_stats.draws != 1000) {
      std::printf("       draws=%u\n", last_stats.draws);
    }
  } else {
    failures += 1;
    std::printf("[FAIL] golden: grid_1000_640x360 frame\n");
    std::printf("[FAIL] ecs-render: 1000 draws\n");
  }

  // Transform-change visibility: move entity 0 by +3 on X (1.5 cube
  // widths — well past any rasterization threshold), re-propagate, and
  // require at least one pixel to differ.
  bool moved_ok = false;
  if (grid_ok) {
    auto* lt = static_cast<LocalTransform*>(
        ecs_get_mut_id(world.raw(), grid[0], local_id));
    if (lt != nullptr) {
      lt->px += 3.0F;
      moved_ok = world.propagate();
    }
    check(moved_ok, "ecs-render: transform moved + propagated");
    std::vector<std::uint8_t> px_moved;
    check(render_frame(px_moved), "ecs-render: moved frame captured");
    if (!px_moved.empty() && moved_ok) {
      bool differs = px_moved.size() == px_grid.size() &&
                     std::memcmp(px_moved.data(), px_grid.data(),
                                 px_grid.size()) != 0;
      check(differs, "ecs-render: transform change visible");
    } else {
      check(false, "ecs-render: transform change visible");
    }
  } else {
    check(false, "ecs-render: transform change visible");
  }

  world.destroy();
  mesh.destroy();
  check(!mesh.ready(), "ecs-render: mesh destroy idles");
  fr.destroy();
  depth.destroy();
  fc.destroy();
  sc.destroy();
  dev.destroy_surface(surface);
  dev.shutdown();
  w.destroy();
}
#else
void test_ecs_render_stub() {
  EcsWorld world;
  check(world.construct(), "ecs-render: stub world constructed");
  check(world.register_render(), "ecs-render: stub render registered");
  check(world.register_render(), "ecs-render: stub register idempotent");
  world.destroy();
  MeshRegistry registry;
  check(registry.find(1) == nullptr, "ecs-render: stub registry empty");
  check(registry.add(nullptr) == 0, "ecs-render: stub registry null reject");
  ForwardRenderer fr;
  check(!fr.mesh_ready(), "ecs-render: stub renderer idle");
  ForwardRenderer::RenderWorldStats stats{};
  stats.draws = 0;
  check(stats.draws == 0, "ecs-render: stub stats idle");
  fr.destroy();
}
#endif

}  // namespace

auto main() -> int {
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  test_ecs_render_gpu();
#else
  test_ecs_render_stub();
#endif
  std::printf(failures == 0 ? "ECS_RENDER_OK\n" : "ECS_RENDER_FAILURES=%d\n",
              failures);
  return failures == 0 ? 0 : 1;
}

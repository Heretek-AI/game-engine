// test_pick_ids.cpp — P3-06 acceptance (native CTest).
//
// GPU test: builds the pick pipeline (pick.vert/pick.frag, 96-byte push,
// R32_UINT id target + D32_SFLOAT depth) on a headless Vulkan device — no
// window, no swapchain (PickIdBuffer is offscreen by construction).
// Spawns one camera + one unit cube through Flecs (same P1-08/P1-07 math:
// camera eye (2,2,3) looking at the origin, 60deg FOV), runs the id pass,
// and reads back pixels:
//
//   [PASS] pick: center selects entity   (projected cube center -> cube id)
//   [PASS] pick: background is zero      (corner pixel -> 0)
//   [PASS] pick: OOB is zero            (no GPU touch, same as managed)
//
// Rules: no SDL (offscreen — no window at all); ENGINE_REQUIRE_GPU=1
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
#include "engine/renderer/pick_buffer.hpp"
#include "engine/renderer/vulkan_device.hpp"
#include "flecs.h"
#include "gpu_required.hpp"

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
using engine::renderer::ForwardRenderer;
using engine::renderer::GpuMesh;
using engine::renderer::MeshRegistry;
using engine::renderer::MeshVertex;
using engine::renderer::PickIdBuffer;
using engine::renderer::PickPush;
using engine::renderer::StatusCode;
using engine::renderer::VulkanDevice;

// Spec P1-08/P3-06 layouts (SDK-free, checked in both modes).
static_assert(sizeof(Camera) == 16, "Camera is 16 bytes");
static_assert(alignof(Camera) == 4, "Camera align drifted");
static_assert(sizeof(MeshRenderer) == 20, "MeshRenderer is 20 bytes");
static_assert(alignof(MeshRenderer) == 4, "MeshRenderer align drifted");
static_assert(sizeof(DirectionalLight) == 16, "DirectionalLight is 16 bytes");
static_assert(sizeof(WorldTransform) == 64, "WorldTransform is 64 bytes");
static_assert(sizeof(PickPush) == 96, "pick push is 96 bytes");

#if defined(ENGINE_RENDERER_HAS_VULKAN)

// --- procedural unit cube: 24 verts (4/face, face normals), 36 indices --
// (Same generator as test_ecs_render.cpp so the pick silhouette matches
// the lit golden silhouette.)
struct Cube {
  std::array<MeshVertex, 24> verts{};
  std::array<std::uint32_t, 36> idx{};
};

auto make_cube() -> Cube {
  Cube c{};
  const float faces[6][7][3] = {
      // +X
      {{1, 0, 0}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1}, {1, -1, 1}},
      // -X
      {{-1, 0, 0}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}, {-1, -1, -1}},
      // +Y
      {{0, 1, 0}, {-1, 1, -1}, {-1, 1, 1}, {1, 1, 1}, {1, -1, 1}},
      // -Y
      {{0, -1, 0}, {-1, -1, 1}, {-1, -1, -1}, {1, -1, -1}, {1, -1, 1}},
      // +Z
      {{0, 0, 1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},
      // -Z
      {{0, 0, -1}, {1, -1, -1}, {-1, -1, -1}, {1, -1, -1}, {-1, 1, -1}},
  };
  for (int f = 0; f < 6; ++f) {
    for (int v = 0; v < 4; ++v) {
      MeshVertex& out = c.verts[static_cast<std::size_t>(f) * 4 + v];
      out.pos[0] = faces[f][1 + v][0] * 0.5F;
      out.pos[1] = faces[f][1 + v][1] * 0.5F;
      out.pos[2] = faces[f][1 + v][2] * 0.5F;
      out.normal[0] = faces[f][0][0];
      out.normal[1] = faces[f][0][1];
      out.normal[2] = faces[f][0][2];
      out.uv[0] = (v == 0 || v == 3) ? 0.0F : 1.0F;
      out.uv[1] = (v < 2) ? 0.0F : 1.0F;
    }
    const std::uint32_t b = static_cast<std::uint32_t>(f) * 4;
    const std::uint32_t quads[6] = {b, b + 1, b + 2, b, b + 2, b + 3};
    for (int k = 0; k < 6; ++k) {
      c.idx[static_cast<std::size_t>(f) * 6 + k] = quads[k];
    }
  }
  return c;
}

// --- minimal column-major mat4 (test-local; Vulkan clip space) ------------
// (Same helpers as test_forward_mesh.cpp: perspective + lookAt + mul.)
struct Mat4 {
  float m[16]{};
};

auto mat_perspective_vk(float fov_y_rad, float aspect, float near_z,
                        float far_z) -> Mat4 {
  Mat4 o{};
  const float f = 1.0F / std::tan(fov_y_rad * 0.5F);
  o.m[0] = f / aspect;
  o.m[5] = -f;
  o.m[10] = far_z / (near_z - far_z);
  o.m[11] = -1.0F;
  o.m[14] = -(far_z * near_z) / (far_z - near_z);
  return o;
}

auto mat_look_at(const float* eye, const float* center, const float* up)
    -> Mat4 {
  float f[3] = {center[0] - eye[0], center[1] - eye[1], center[2] - eye[2]};
  const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
  f[0] /= fl;
  f[1] /= fl;
  f[2] /= fl;
  float s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2],
                f[0] * up[1] - f[1] * up[0]};
  const float sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
  s[0] /= sl;
  s[1] /= sl;
  s[2] /= sl;
  float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2],
                s[0] * f[1] - s[1] * f[0]};
  Mat4 o{};
  o.m[0] = s[0];
  o.m[1] = u[0];
  o.m[2] = -f[0];
  o.m[3] = 0.0F;
  o.m[4] = s[1];
  o.m[5] = u[1];
  o.m[6] = -f[1];
  o.m[7] = 0.0F;
  o.m[8] = s[2];
  o.m[9] = u[2];
  o.m[10] = -f[2];
  o.m[11] = 0.0F;
  o.m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
  o.m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
  o.m[14] = (f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]);
  o.m[15] = 1.0F;
  return o;
}

auto mat_mul(const Mat4& a, const Mat4& b) -> Mat4 {
  Mat4 o{};
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      o.m[c * 4 + r] = a.m[r] * b.m[c * 4] + a.m[4 + r] * b.m[c * 4 + 1] +
                       a.m[8 + r] * b.m[c * 4 + 2] +
                       a.m[12 + r] * b.m[c * 4 + 3];
    }
  }
  return o;
}

void test_pick_ids_gpu() {
  constexpr std::uint32_t kW = 320;
  constexpr std::uint32_t kH = 240;

  // Headless device: no SDL window, no swapchain (offscreen pick path).
  VulkanDevice dev;
  engine::renderer::DeviceCreateInfo ci{};
  ci.app_name = "P3-06-pick";
  ci.enable_swapchain = false;
  auto st = dev.init(ci);
  if (!st.ok() && st.code == StatusCode::kNoPhysicalDevice) {
    failures += gpu_missing("pick: no physical device (no ICD)");
    return;
  }
  check(st.ok(), "pick: device init ok");
  if (!st.ok()) {
    std::printf("       init failed: %s\n", st.message);
    return;
  }

  ForwardRenderer fr;
  PickIdBuffer pick;
  VkExtent2D extent{};
  extent.width = kW;
  extent.height = kH;
  check(pick.create(dev, fr, extent).ok(), "pick: buffer created");
  check(fr.pick_ready(), "pick: pipeline built");
  check(pick.ready(), "pick: ready");
  if (!pick.ready() || !fr.pick_ready()) {
    fr.destroy();
    pick.destroy();
    dev.shutdown();
    return;
  }
  check(!pick.create(dev, fr, extent).ok(), "pick: double create rejected");

  // One-shot command pool for the id pass (pick() owns its own copy cmd).
  VkCommandPool pool = nullptr;
  VkCommandBuffer cmd = nullptr;
  {
    auto vkCreateCommandPool = reinterpret_cast<PFN_vkCreateCommandPool>(
        dev.instance_proc("vkCreateCommandPool"));
    auto vkAllocateCommandBuffers =
        reinterpret_cast<PFN_vkAllocateCommandBuffers>(
            dev.instance_proc("vkAllocateCommandBuffers"));
    auto vkBeginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(
        dev.instance_proc("vkBeginCommandBuffer"));
    check(vkCreateCommandPool != nullptr &&
          vkAllocateCommandBuffers != nullptr &&
          vkBeginCommandBuffer != nullptr,
          "pick: pool entries resolve");
    if (vkCreateCommandPool == nullptr ||
        vkAllocateCommandBuffers == nullptr ||
        vkBeginCommandBuffer == nullptr) {
      fr.destroy();
      pick.destroy();
      dev.shutdown();
      return;
    }
    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    cpi.queueFamilyIndex = dev.graphics_queue_family();
    check(vkCreateCommandPool(dev.device(), &cpi, nullptr, &pool) ==
              VK_SUCCESS &&
          pool != nullptr,
          "pick: pool created");
    if (pool == nullptr) {
      fr.destroy();
      pick.destroy();
      dev.shutdown();
      return;
    }
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(dev.device(), &cai, &cmd) == VK_SUCCESS &&
          cmd != nullptr,
          "pick: cmd allocated");
    if (cmd == nullptr) {
      auto vkDestroyCommandPool =
          reinterpret_cast<PFN_vkDestroyCommandPool>(
              dev.instance_proc("vkDestroyCommandPool"));
      if (vkDestroyCommandPool != nullptr) {
        vkDestroyCommandPool(dev.device(), pool, nullptr);
      }
      fr.destroy();
      pick.destroy();
      dev.shutdown();
      return;
    }
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd, &bi) == VK_SUCCESS,
          "pick: cmd begun");
  }

  // ---- Flecs world: one camera + one cube at the origin ----------------
  Cube cube = make_cube();
  GpuMesh mesh;
  check(mesh.create(dev, cube.verts.data(), 24, cube.idx.data(), 36).ok(),
        "pick: cube uploaded (24v/36i)");
  MeshRegistry registry;
  const std::uint32_t cube_id = registry.add(&mesh);
  check(cube_id != 0, "pick: mesh registered");

  EcsWorld world;
  check(world.construct(), "pick: world constructed");
  check(world.register_render(), "pick: render components registered");
  const ecs_entity_t cam_comp = ecs_lookup_path_w_sep(
      world.raw(), 0, "Engine.Runtime.Camera", "", "", false);
  const ecs_entity_t mesh_comp = ecs_lookup_path_w_sep(
      world.raw(), 0, "Engine.Runtime.MeshRenderer", "", "", false);
  const ecs_entity_t light_comp = ecs_lookup_path_w_sep(
      world.raw(), 0, "Engine.Runtime.DirectionalLight", "", "", false);
  check(cam_comp != 0 && mesh_comp != 0 && light_comp != 0,
        "pick: component ids resolve");
  check(world.local_id() != 0 && world.world_id() != 0,
        "pick: transform ids valid");
  const ecs_entity_t local_id =
      static_cast<ecs_entity_t>(world.local_id());
  const ecs_entity_t world_id =
      static_cast<ecs_entity_t>(world.world_id());

  // Camera (P1-07 convention): eye (2,2,3), origin target, 60deg FOV.
  constexpr float kFov = 60.0F * 3.14159265F / 180.0F;
  const float eye[3] = {2.0F, 2.0F, 3.0F};
  const float center[3] = {0.0F, 0.0F, 0.0F};
  {
    const ecs_entity_t e = ecs_new(world.raw());
    Camera c{};
    c.fov_y_rad = kFov;
    c.near_z = 0.1F;
    c.far_z = 100.0F;
    c.is_main = 1;
    ecs_set_id(world.raw(), e, cam_comp, sizeof(Camera), &c);
    const float fwd_in[3] = {center[0] - eye[0], center[1] - eye[1],
                             center[2] - eye[2]};
    const float fwd_l = std::sqrt(fwd_in[0] * fwd_in[0] +
                                  fwd_in[1] * fwd_in[1] +
                                  fwd_in[2] * fwd_in[2]);
    float fwd[3] = {fwd_in[0] / fwd_l, fwd_in[1] / fwd_l, fwd_in[2] / fwd_l};
    const float up_in[3] = {0.0F, 1.0F, 0.0F};
    float sd[3] = {fwd[1] * up_in[2] - fwd[2] * up_in[1],
                   fwd[2] * up_in[0] - fwd[0] * up_in[2],
                   fwd[0] * up_in[1] - fwd[1] * up_in[0]};
    const float sd_l =
        std::sqrt(sd[0] * sd[0] + sd[1] * sd[1] + sd[2] * sd[2]);
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
    wt.m[12] = eye[0];
    wt.m[13] = eye[1];
    wt.m[14] = eye[2];
    wt.m[15] = 1.0F;
    ecs_set_id(world.raw(), e, world_id, sizeof(WorldTransform), &wt);
  }
  // Directional light (any; the pick pass ignores it, but the component
  // must exist or render_ids fails closed like render_world).
  {
    const ecs_entity_t e = ecs_new(world.raw());
    DirectionalLight l{};
    l.direction[0] = -0.5F;
    l.direction[1] = -0.9F;
    l.direction[2] = -0.65F;
    l.intensity = 1.0F;
    ecs_set_id(world.raw(), e, light_comp, sizeof(DirectionalLight), &l);
  }

  // Cube at the origin (identity Local, composed World, tan base color —
  // the color is ignored by the pick pass; only the id matters).
  ecs_entity_t cube_entity = 0;
  {
    const ecs_entity_t e = ecs_new(world.raw());
    cube_entity = e;
    LocalTransform lt{};
    lt.px = 0.0F;
    lt.py = 0.0F;
    lt.pz = 0.0F;
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
  }
  check(cube_entity != 0, "pick: cube spawned");
  check(world.propagate(), "pick: world propagated");

  // Project the cube center (origin) with the same row-vector math the
  // renderer uses (RowWorldToColView path is identity-model here: the
  // center is the origin, so view_proj * (0,0,0,1) is the clip point).
  const float up[3] = {0.0F, 1.0F, 0.0F};
  const Mat4 proj =
      mat_perspective_vk(kFov, static_cast<float>(kW) / static_cast<float>(kH),
                         0.1F, 100.0F);
  const Mat4 view = mat_look_at(eye, center, up);
  const Mat4 pv = mat_mul(proj, view);
  // Clip = pv * (0,0,0,1) = translation column of pv.
  const float cx = pv.m[12];
  const float cy = pv.m[13];
  const float cw = pv.m[15];
  const float ndc_x = cx / cw;
  const float ndc_y = cy / cw;
  // Vulkan NDC is Y-down already in the projection (-f in m[5]) and the
  // viewport is full-extent top-left: px = (ndc+1)/2 * extent.
  auto px = static_cast<std::uint32_t>((ndc_x * 0.5F + 0.5F) *
                                       static_cast<float>(kW));
  auto py = static_cast<std::uint32_t>((ndc_y * 0.5F + 0.5F) *
                                       static_cast<float>(kH));
  if (px >= kW) {
    px = kW - 1;
  }
  if (py >= kH) {
    py = kH - 1;
  }
  std::printf("       cube center projects to (%u,%u)\n", px, py);

  // Id pass: record, submit, wait. Two modes: full render_ids via
  // PickIdBuffer, plus a direct record_pick probe below (fullscreen MVP).
  check(pick
            .record_id_pass(dev, fr, world.raw(), registry, cmd,
                            static_cast<float>(kW) / static_cast<float>(kH))
            .ok(),
        "pick: id pass recorded");
  {
    auto vkEndCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer>(
        dev.instance_proc("vkEndCommandBuffer"));
    auto vkCreateFence = reinterpret_cast<PFN_vkCreateFence>(
        dev.instance_proc("vkCreateFence"));
    auto vkDestroyFence = reinterpret_cast<PFN_vkDestroyFence>(
        dev.instance_proc("vkDestroyFence"));
    auto vkWaitForFences = reinterpret_cast<PFN_vkWaitForFences>(
        dev.instance_proc("vkWaitForFences"));
    auto vkQueueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(
        dev.instance_proc("vkQueueSubmit"));
    bool submitted = false;
    if (vkEndCommandBuffer != nullptr && vkCreateFence != nullptr &&
        vkWaitForFences != nullptr && vkQueueSubmit != nullptr) {
      if (vkEndCommandBuffer(cmd) == VK_SUCCESS) {
        VkFence fence = nullptr;
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(dev.device(), &fci, nullptr, &fence) ==
                VK_SUCCESS &&
            fence != nullptr) {
          VkSubmitInfo si{};
          si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
          si.commandBufferCount = 1;
          si.pCommandBuffers = &cmd;
          if (vkQueueSubmit(dev.graphics_queue(), 1, &si, fence) ==
              VK_SUCCESS) {
            submitted = vkWaitForFences(dev.device(), 1, &fence, VK_TRUE,
                                        UINT64_MAX) == VK_SUCCESS;
          }
          if (vkDestroyFence != nullptr) {
            vkDestroyFence(dev.device(), fence, nullptr);
          }
        }
      }
    }
    check(submitted, "pick: id pass submitted");
  }

  // Center pixel -> the cube entity; background corner -> 0.
  {
    std::uint32_t lo = 0;
    std::uint32_t hi = 0;
    check(pick.pick(dev, px, py, lo, hi).ok(), "pick: center read ok");
    const std::uint64_t got =
        (static_cast<std::uint64_t>(hi) << 32) | lo;
    const std::uint64_t want = static_cast<std::uint64_t>(cube_entity);
    if (got != want) {
      std::printf("       center id=%llu want=%llu\n",
                  static_cast<unsigned long long>(got),
                  static_cast<unsigned long long>(want));
    }
    check(got == want, "pick: center selects entity");
  }
  // Corner far from the centered cube: the cube fills the frame middle
  // at 320x240 with a (2,2,3) eye, so (2,2) is background.
  {
    std::uint32_t lo = 0;
    std::uint32_t hi = 0;
    check(pick.pick(dev, 2, 2, lo, hi).ok(), "pick: corner read ok");
    const std::uint64_t got =
        (static_cast<std::uint64_t>(hi) << 32) | lo;
    check(got == 0, "pick: background is zero");
  }
  {
    // OOB reads return 0 without touching the GPU.
    std::uint32_t lo = 0;
    std::uint32_t hi = 0;
    check(pick.pick(dev, kW + 10, kH + 10, lo, hi).ok(),
          "pick: OOB read ok");
    const std::uint64_t got =
        (static_cast<std::uint64_t>(hi) << 32) | lo;
    check(got == 0, "pick: OOB is zero");
  }

  // Teardown (pool first, then buffer/renderer/device).
  {
    auto vkDestroyCommandPool = reinterpret_cast<PFN_vkDestroyCommandPool>(
        dev.instance_proc("vkDestroyCommandPool"));
    if (vkDestroyCommandPool != nullptr) {
      vkDestroyCommandPool(dev.device(), pool, nullptr);
    }
  }
  world.destroy();
  mesh.destroy();
  check(!mesh.ready(), "pick: mesh destroy idles");
  fr.destroy();
  pick.destroy();
  dev.shutdown();
}
#else
void test_pick_ids_stub() {
  ForwardRenderer fr;
  check(!fr.pick_ready(), "pick: stub renderer idle");
  check(fr.pick_module_count() == 0, "pick: stub modules idle");
  PickIdBuffer pick;
  check(!pick.ready(), "pick: stub buffer idle");
  ForwardRenderer::RenderWorldStats stats{};
  stats.draws = 0;
  check(stats.draws == 0, "pick: stub stats idle");
  fr.destroy();
  pick.destroy();
}
#endif

}  // namespace

auto main() -> int {
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  test_pick_ids_gpu();
#else
  test_pick_ids_stub();
#endif
  std::printf(failures == 0 ? "PICK_IDS_OK\n" : "PICK_IDS_FAILURES=%d\n",
              failures);
  return failures == 0 ? 0 : 1;
}

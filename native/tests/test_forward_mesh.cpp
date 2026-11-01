// test_forward_mesh.cpp — P1-07 acceptance (native CTest).
//
// GPU test: opens a 320x240 window, builds the ForwardRenderer mesh
// pipeline (mesh.vert/mesh.frag, 160-byte push, D32_SFLOAT depth), uploads
// a procedural unit cube (24 vertices, 36 indices) through VMA, renders it
// lit (camera (2,2,3), 60deg FOV, Vulkan clip space) and compares against
// golden cube_lit_320x240.png. A second frame draws the same cube plus a
// copy offset behind it along the view ray; pixel-identical output proves
// the depth test hides the back faces. Prints:
//
//   [PASS] golden: cube_lit_320x240        (via golden_check)
//   [PASS] mesh: depth test hides back faces
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

#include "engine/renderer/forward_renderer.hpp"
#include "engine/renderer/gpu_mesh.hpp"
#include "engine/renderer/renderer_window.hpp"
#include "engine/renderer/swapchain.hpp"
#include "engine/renderer/vulkan_device.hpp"
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

using engine::renderer::DepthBuffer;
using engine::renderer::ForwardRenderer;
using engine::renderer::FrameContext;
using engine::renderer::GpuMesh;
using engine::renderer::MeshPush;
using engine::renderer::MeshVertex;
using engine::renderer::RendererWindow;
using engine::renderer::StatusCode;
using engine::renderer::Swapchain;
using engine::renderer::VulkanDevice;
using engine::renderer::WindowConfig;

// Spec §P1-07 layouts (SDK-free, checked in both modes).
static_assert(sizeof(MeshVertex) == 32, "mesh vertex is 32 bytes");
static_assert(sizeof(MeshPush) == 160, "mesh push is 160 bytes");
static_assert(offsetof(MeshVertex, normal) == 12, "normal offset");
static_assert(offsetof(MeshVertex, uv) == 24, "uv offset");

#if defined(ENGINE_RENDERER_HAS_VULKAN)

// --- minimal column-major mat4 (test-local; Vulkan clip space) ------------
struct Mat4 {
  float m[16]{};
};

auto mat_identity() -> Mat4 {
  Mat4 o{};
  o.m[0] = o.m[5] = o.m[10] = o.m[15] = 1.0F;
  return o;
}

auto mat_translate(float x, float y, float z) -> Mat4 {
  Mat4 o = mat_identity();
  o.m[12] = x;
  o.m[13] = y;
  o.m[14] = z;
  return o;
}

// Vulkan perspective: Y flipped, depth 0..1 (spec P1-07).
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

// Right-handed lookAt.
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
  Mat4 o = mat_identity();
  o.m[0] = s[0];
  o.m[1] = u[0];
  o.m[2] = -f[0];
  o.m[4] = s[1];
  o.m[5] = u[1];
  o.m[6] = -f[1];
  o.m[8] = s[2];
  o.m[9] = u[2];
  o.m[10] = -f[2];
  o.m[12] = -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]);
  o.m[13] = -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]);
  o.m[14] = (f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2]);
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

// --- procedural unit cube: 24 verts (4/face, face normals), 36 indices ----
struct Cube {
  std::array<MeshVertex, 24> verts{};
  std::array<std::uint32_t, 36> idx{};
};

auto make_cube() -> Cube {
  Cube c{};
  // faces: normal + 4 CCW corners (viewed from outside).
  const float faces[6][7][3] = {
      // +X
      {{1, 0, 0}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1}, {1, -1, 1}},
      // -X
      {{-1, 0, 0}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}, {-1, -1, -1}},
      // +Y
      {{0, 1, 0}, {-1, 1, -1}, {-1, 1, 1}, {1, 1, 1}, {1, 1, -1}},
      // -Y
      {{0, -1, 0}, {-1, -1, 1}, {-1, -1, -1}, {1, -1, -1}, {1, -1, 1}},
      // +Z
      {{0, 0, 1}, {-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},
      // -Z
      {{0, 0, -1}, {1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}},
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

void fill_push(MeshPush& p, const Mat4& mvp, const Mat4& model,
               const float* light_dir, const float* base_color) {
  std::memcpy(p.mvp, mvp.m, sizeof(p.mvp));
  std::memcpy(p.model, model.m, sizeof(p.model));
  std::memcpy(p.light_dir, light_dir, sizeof(p.light_dir));
  std::memcpy(p.base_color, base_color, sizeof(p.base_color));
}

void test_forward_mesh_gpu() {
  RendererWindow w;
  WindowConfig cfg{};
  cfg.hidden = true;
  cfg.width = 320;
  cfg.height = 240;
  auto wst = w.create(cfg);
  if (!wst.ok()) {
    failures +=
        gpu_missing("mesh: offscreen window unavailable (no video device)");
    return;
  }

  std::array<const char*, 64> exts{};
  std::uint32_t got = 0;
  auto xst = w.required_instance_extensions(exts.data(), 64, &got);
  if (!xst.ok() || got == 0) {
    failures += gpu_missing("mesh: window instance extensions query");
    w.destroy();
    return;
  }

  VulkanDevice dev;
  engine::renderer::DeviceCreateInfo ci{};
  ci.app_name = "P1-07-test";
  ci.extra_instance_extensions = exts.data();
  ci.extra_instance_extension_count = got;
  ci.enable_swapchain = true;
  auto st = dev.init(ci);
  if (!st.ok() && st.code == StatusCode::kNoPhysicalDevice) {
    failures += gpu_missing("mesh: no physical device (no ICD)");
    w.destroy();
    return;
  }
  check(st.ok(), "mesh: device init ok");
  if (!st.ok()) {
    std::printf("       init failed: %s\n", st.message);
    w.destroy();
    return;
  }

  VkSurfaceKHR surface = nullptr;
  auto sst = dev.create_surface(w, &surface);
  check(sst.ok() && surface != nullptr, "mesh: surface create ok");
  if (!sst.ok() || surface == nullptr) {
    dev.shutdown();
    w.destroy();
    return;
  }

  Swapchain sc;
  engine::renderer::SwapchainDesc desc{};
  desc.width = 320;
  desc.height = 240;
  desc.vsync = true;
  auto cst = sc.create(dev, surface, desc);
  check(cst.ok(), "mesh: swapchain create ok");
  if (!cst.ok()) {
    dev.destroy_surface(surface);
    dev.shutdown();
    w.destroy();
    return;
  }

  ForwardRenderer fr;
  GpuMesh empty_mesh;
  MeshPush empty_push{};
  // CPU-side negative gates (run before any GPU work).
  check(!fr.record_mesh(nullptr, empty_mesh, empty_push).ok(),
        "mesh: record before build rejected");
  check(!fr.begin_mesh_pass(nullptr, nullptr, nullptr, sc.extent()).ok(),
        "mesh: pass before build rejected");
  check(!fr.create_mesh(dev, sc.format(), VK_FORMAT_UNDEFINED).ok(),
        "mesh: non-D32 depth rejected");
  {
    const std::uint32_t bad[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    check(!fr.create_mesh(dev, sc.format(), VK_FORMAT_D32_SFLOAT, bad, 8, bad,
                          8)
               .ok(),
          "mesh: bad blob rejected");
  }
  DepthBuffer depth;
  {
    VkExtent2D zero{};
    zero.width = 0;
    zero.height = 0;
    check(!depth.create(dev, zero, VK_FORMAT_D32_SFLOAT).ok(),
          "mesh: zero depth extent rejected");
    check(!depth.create(dev, sc.extent(), VK_FORMAT_R8G8B8A8_UNORM).ok(),
          "mesh: non-D32 depth buffer rejected");
  }
  GpuMesh mesh;
  check(!mesh.create(dev, nullptr, 24, nullptr, 36).ok(),
        "mesh: null upload rejected");
  {
    Cube cube = make_cube();
    check(!mesh.create(dev, cube.verts.data(), 0, cube.idx.data(), 36).ok(),
          "mesh: zero verts rejected");
  }

  auto dst = depth.create(dev, sc.extent(), VK_FORMAT_D32_SFLOAT);
  check(dst.ok() && depth.ready(), "mesh: depth buffer created");
  if (!dst.ok()) {
    sc.destroy();
    dev.destroy_surface(surface);
    dev.shutdown();
    w.destroy();
    return;
  }
  auto mst = fr.create_mesh(dev, sc.format(), VK_FORMAT_D32_SFLOAT);
  check(mst.ok() && fr.mesh_ready() && fr.mesh_module_count() == 2,
        "mesh: pipeline built");
  if (!mst.ok()) {
    std::printf("       mesh create failed: %s\n", mst.message);
    depth.destroy();
    sc.destroy();
    dev.destroy_surface(surface);
    dev.shutdown();
    w.destroy();
    return;
  }
  check(!fr.create_mesh(dev, sc.format(), VK_FORMAT_D32_SFLOAT).ok(),
        "mesh: double build rejected");

  Cube cube = make_cube();
  auto ust = mesh.create(dev, cube.verts.data(), 24, cube.idx.data(), 36);
  check(ust.ok() && mesh.ready() && mesh.vertex_count() == 24 &&
            mesh.index_count() == 36,
        "mesh: cube uploaded (24v/36i)");
  if (!ust.ok()) {
    fr.destroy();
    depth.destroy();
    sc.destroy();
    dev.destroy_surface(surface);
    dev.shutdown();
    w.destroy();
    return;
  }
  check(!mesh.create(dev, cube.verts.data(), 24, cube.idx.data(), 36).ok(),
        "mesh: double upload rejected");
  check(!fr.record_mesh(nullptr, mesh, empty_push).ok(),
        "mesh: null cmd rejected");

  // Camera (spec P1-07): eye (2,2,3), origin target, 60deg FOV.
  const float eye[3] = {2.0F, 2.0F, 3.0F};
  const float center[3] = {0.0F, 0.0F, 0.0F};
  const float up[3] = {0.0F, 1.0F, 0.0F};
  const float kFov = 60.0F * 3.14159265F / 180.0F;
  const Mat4 proj = mat_perspective_vk(kFov, 320.0F / 240.0F, 0.1F, 100.0F);
  const Mat4 view = mat_look_at(eye, center, up);
  const Mat4 pv = mat_mul(proj, view);
  const Mat4 front_model = mat_identity();
  // Back cube: same axis as the view ray, 1.5 units behind the front
  // centre — projects fully inside the front silhouette, so any pixel
  // difference means the depth test failed.
  const float ray[3] = {-2.0F / 4.1231056F, -2.0F / 4.1231056F,
                        -3.0F / 4.1231056F};
  const Mat4 back_model =
      mat_translate(1.5F * ray[0], 1.5F * ray[1], 1.5F * ray[2]);
  float light_len =
      std::sqrt(0.25F + 0.81F + 0.4225F);  // (0.5, 0.9, 0.65)
  // light_dir is the direction light TRAVELS (shader uses dot(n,
  // -light_dir)): negated so the camera-facing +X/+Y/+Z faces are lit.
  const float light_dir[4] = {-0.5F / light_len, -0.9F / light_len,
                              -0.65F / light_len, 0.0F};
  const float base_color[4] = {0.85F, 0.5F, 0.2F, 1.0F};
  MeshPush front_push{};
  fill_push(front_push, mat_mul(pv, front_model), front_model, light_dir,
            base_color);
  MeshPush back_push{};
  fill_push(back_push, mat_mul(pv, back_model), back_model, light_dir,
            base_color);

  auto p_vkCmdPipelineBarrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(
      dev.proc("vkCmdPipelineBarrier2"));
  check(p_vkCmdPipelineBarrier2 != nullptr, "mesh: barrier entry resolves");
  if (p_vkCmdPipelineBarrier2 == nullptr) {
    mesh.destroy();
    fr.destroy();
    depth.destroy();
    sc.destroy();
    dev.destroy_surface(surface);
    dev.shutdown();
    w.destroy();
    return;
  }

  FrameContext fc;
  auto fcst = fc.create(dev, sc);
  check(fcst.ok(), "mesh: frame context create ok");
  if (!fcst.ok()) {
    mesh.destroy();
    fr.destroy();
    depth.destroy();
    sc.destroy();
    dev.destroy_surface(surface);
    dev.shutdown();
    w.destroy();
    return;
  }

  VkImage depth_img = reinterpret_cast<VkImage>(
      static_cast<std::uintptr_t>(depth.image_bits()));
  auto barrier_color = [&](VkCommandBuffer cmd, VkImage img,
                           VkImageLayout old_l, VkImageLayout new_l,
                           VkPipelineStageFlags2 src_stage,
                           VkAccessFlags2 src_access,
                           VkPipelineStageFlags2 dst_stage,
                           VkAccessFlags2 dst_access) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = src_stage;
    b.dstStageMask = dst_stage;
    b.srcAccessMask = src_access;
    b.dstAccessMask = dst_access;
    b.oldLayout = old_l;
    b.newLayout = new_l;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = 1;
    VkDependencyInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    p_vkCmdPipelineBarrier2(cmd, &di);
  };
  auto barrier_depth = [&](VkCommandBuffer cmd, VkImageLayout old_l,
                           VkImageLayout new_l) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = (old_l == VK_IMAGE_LAYOUT_UNDEFINED)
                         ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
                         : (VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT);
    b.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    b.srcAccessMask = (old_l == VK_IMAGE_LAYOUT_UNDEFINED)
                          ? 0
                          : VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.oldLayout = old_l;
    b.newLayout = new_l;
    b.image = depth_img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = 1;
    VkDependencyInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    p_vkCmdPipelineBarrier2(cmd, &di);
  };

  // Render one frame: when `with_back` is true the back cube is drawn in
  // a second LOAD pass after the front cube (depth must hide it).
  // `first_use` must be true exactly once per process: it drives the
  // depth image's UNDEFINED->ATTACHMENT transition.
  bool depth_live = false;
  auto render_frame = [&](bool with_back,
                          std::vector<std::uint8_t>& out) -> bool {
    auto begin = fc.begin_frame();
    if (!begin.ok || begin.cmd == nullptr) {
      return false;
    }
    VkImage img = sc.image(begin.image_index);
    VkImageView view_h = sc.view(begin.image_index);
    if (img == nullptr || view_h == nullptr) {
      return false;
    }
    const bool first_use = !depth_live;
    barrier_color(begin.cmd, img, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    barrier_depth(begin.cmd,
                  first_use ? VK_IMAGE_LAYOUT_UNDEFINED
                            : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    depth_live = true;
    if (!fr.begin_mesh_pass(begin.cmd, view_h, depth.view(), sc.extent())
             .ok()) {
      return false;
    }
    if (!fr.record_mesh(begin.cmd, mesh, front_push).ok()) {
      return false;
    }
    if (!fr.end_mesh_pass(begin.cmd).ok()) {
      return false;
    }
    if (with_back) {
      if (!fr.continue_mesh_pass(begin.cmd, view_h, depth.view(), sc.extent())
               .ok()) {
        return false;
      }
      if (!fr.record_mesh(begin.cmd, mesh, back_push).ok()) {
        return false;
      }
      if (!fr.end_mesh_pass(begin.cmd).ok()) {
        return false;
      }
    }
    barrier_color(
        begin.cmd, img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
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
    return out.size() == 320u * 240u * 4u;
  };

  std::vector<std::uint8_t> px_front;
  check(render_frame(false, px_front), "mesh: front-only frame captured");
  std::vector<std::uint8_t> px_both;
  bool both_ok = render_frame(true, px_both);
  check(both_ok, "mesh: front+back frame captured");
  if (both_ok && !px_both.empty()) {
    failures += engine::test::golden_check(
        "cube_lit_320x240",
        std::span<const std::uint8_t>(px_both.data(), px_both.size()), 320,
        240);
    // Depth proof: the back cube projects fully inside the front cube's
    // silhouette. Without a working depth test the second (LOAD-pass) draw
    // would overwrite those pixels; identical frames mean it was hidden.
    bool identical = px_front.size() == px_both.size() &&
                     std::memcmp(px_front.data(), px_both.data(),
                                 px_both.size()) == 0;
    check(identical, "mesh: depth test hides back faces");
  } else {
    failures += 1;
    std::printf("[FAIL] golden: cube_lit_320x240 frame\n");
    std::printf("[FAIL] mesh: depth test hides back faces\n");
  }

  mesh.destroy();
  check(!mesh.ready(), "mesh: destroy idles");
  mesh.destroy();
  fr.destroy();
  depth.destroy();
  fc.destroy();
  sc.destroy();
  dev.destroy_surface(surface);
  dev.shutdown();
  w.destroy();
}
#else
void test_forward_mesh_stub() {
  ForwardRenderer fr;
  GpuMesh mesh;
  DepthBuffer depth;
  check(!fr.ready() && !fr.mesh_ready() && fr.mesh_module_count() == 0,
        "mesh: stub idle");
  check(!mesh.ready() && mesh.vertex_count() == 0 &&
            mesh.index_count() == 0,
        "mesh: stub mesh idle");
  check(!depth.ready(), "mesh: stub depth idle");
  fr.destroy();
  mesh.destroy();
  depth.destroy();
}
#endif

}  // namespace

auto main() -> int {
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  test_forward_mesh_gpu();
#else
  test_forward_mesh_stub();
#endif
  std::printf(failures == 0 ? "FORWARD_MESH_OK\n"
                            : "FORWARD_MESH_FAILURES=%d\n",
              failures);
  return failures == 0 ? 0 : 1;
}

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
#include "mesh_test_helpers.hpp"

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
using engine::renderer::SkinnedVertexExtra;
using engine::renderer::StatusCode;
using engine::renderer::Swapchain;
using engine::renderer::VulkanDevice;
using engine::renderer::WindowConfig;
using engine::renderer::kSkinMaxJoints;

// Spec §P1-07 layouts (SDK-free, checked in both modes).
static_assert(sizeof(MeshVertex) == 32, "mesh vertex is 32 bytes");
static_assert(sizeof(MeshPush) == 160, "mesh push is 160 bytes");
static_assert(offsetof(MeshVertex, normal) == 12, "normal offset");
static_assert(offsetof(MeshVertex, uv) == 24, "uv offset");
// P5-02: skin payload rides a SEPARATE binding-1 buffer so the legacy
// 32-byte MeshVertex layout (above) stays byte-identical.
static_assert(sizeof(SkinnedVertexExtra) == 32, "skin extra is 32 bytes");
static_assert(offsetof(SkinnedVertexExtra, weights) == 16,
              "weights offset");
static_assert(kSkinMaxJoints == 64, "joint palette cap");

#if defined(ENGINE_RENDERER_HAS_VULKAN)

// P5-02: cube/mat4/barrier scaffolding now lives in mesh_test_helpers.hpp
// (shared with test_ecs_render + test_pick_ids; Sonar gate). Local aliases
// keep the GPU sections readable.
using Cube = engine::test::HelperCube;
using Mat4 = engine::test::HelperMat4;
using engine::test::make_helper_cube;
using engine::test::helper_mat_identity;
using engine::test::helper_mat_translate;
using engine::test::helper_mat_perspective_vk;
using engine::test::helper_mat_look_at;
using engine::test::helper_mat_mul;
using engine::test::helper_fill_push;

void test_forward_mesh_gpu() {
  engine::test::HelperGpuWindow gw{};
  auto missing = [](const char* what) -> int { return gpu_missing(what); };
  if (!engine::test::helper_gpu_window(
          "P1-07-test", 320, 240,
          "mesh: offscreen window unavailable (no video device)",
          "mesh: window instance extensions query",
          "mesh: no physical device (no ICD)", "mesh: device init ok",
          "mesh: surface create ok", "mesh: swapchain create ok", check,
          missing, &failures, &gw)) {
    return;
  }
  RendererWindow& w = gw.w;
  VulkanDevice& dev = gw.dev;
  VkSurfaceKHR surface = gw.surface;
  Swapchain& sc = gw.sc;
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
    Cube cube = make_helper_cube();
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

  Cube cube = make_helper_cube();
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
  const Mat4 proj = helper_mat_perspective_vk(kFov, 320.0F / 240.0F, 0.1F, 100.0F);
  const Mat4 view = helper_mat_look_at(eye, center, up);
  const Mat4 pv = helper_mat_mul(proj, view);
  const Mat4 front_model = helper_mat_identity();
  // Back cube: same axis as the view ray, 1.5 units behind the front
  // centre — projects fully inside the front silhouette, so any pixel
  // difference means the depth test failed.
  const float ray[3] = {-2.0F / 4.1231056F, -2.0F / 4.1231056F,
                        -3.0F / 4.1231056F};
  const Mat4 back_model =
      helper_mat_translate(1.5F * ray[0], 1.5F * ray[1], 1.5F * ray[2]);
  float light_len =
      std::sqrt(0.25F + 0.81F + 0.4225F);  // (0.5, 0.9, 0.65)
  // light_dir is the direction light TRAVELS (shader uses dot(n,
  // -light_dir)): negated so the camera-facing +X/+Y/+Z faces are lit.
  const float light_dir[4] = {-0.5F / light_len, -0.9F / light_len,
                              -0.65F / light_len, 0.0F};
  const float base_color[4] = {0.85F, 0.5F, 0.2F, 1.0F};
  MeshPush front_push{};
  helper_fill_push(front_push.mvp, front_push.model, front_push.light_dir, front_push.base_color, helper_mat_mul(pv, front_model), front_model, light_dir,
            base_color);
  MeshPush back_push{};
  helper_fill_push(back_push.mvp, back_push.model, back_push.light_dir, back_push.base_color, helper_mat_mul(pv, back_model), back_model, light_dir,
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

  engine::test::HelperBarriers bars{};
  bars.cmd_barrier2 = p_vkCmdPipelineBarrier2;
  bars.depth_image = reinterpret_cast<VkImage>(
      static_cast<std::uintptr_t>(depth.image_bits()));

  // Render one frame: when `with_back` is true the back cube is drawn in
  // a second LOAD pass after the front cube (depth must hide it).
  // `first_use` must be true exactly once per process: it drives the
  // depth image's UNDEFINED->ATTACHMENT transition.
  bool depth_live = false;
  auto render_frame = [&](bool with_back,
                          std::vector<std::uint8_t>& out) -> bool {
    engine::test::HelperFrame hf{};
    if (!engine::test::helper_frame_open(fr, fc, sc, depth, bars, depth_live,
                                         &hf)) {
      return false;
    }
    VkCommandBuffer cmd = hf.cmd;
    VkImage img = hf.img;
    VkImageView view_h = hf.view;
    if (!fr.record_mesh(cmd, mesh, front_push).ok()) {
      return false;
    }
    if (!fr.end_mesh_pass(cmd).ok()) {
      return false;
    }
    if (with_back) {
      if (!fr.continue_mesh_pass(cmd, view_h, depth.view(), sc.extent())
               .ok()) {
        return false;
      }
      if (!fr.record_mesh(cmd, mesh, back_push).ok()) {
        return false;
      }
      if (!fr.end_mesh_pass(cmd).ok()) {
        return false;
      }
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

  // --- P5-02 GPU skinning verification ----------------------------------
  // Bent-pose check: the SAME quad drawn twice — once with an identity
  // joint palette, once with joint 1 translated +0.5 X — must differ in
  // the skinned half, while a zero-weight half keeps the bind pose.
  // Weight-normalization check: halved weights (sum 0.5) render identical
  // to full weights (sum 1.0). Uses a second renderer instance so the
  // skinned pipeline (binding-1 skin attributes + set-0 joint storage
  // buffer) coexists with the legacy pipeline undisturbed.
  {
    ForwardRenderer sfr;
    GpuMesh skinned_mesh;
    MeshPush empty{};
    // CPU-side negative gates first (no GPU work).
    check(!sfr.record_mesh_skinned(nullptr, skinned_mesh, 0, empty, 0, 0, 2)
               .ok(),
          "skin: record before build rejected");
    check(!sfr.create_mesh_skinned(dev, sc.format(), VK_FORMAT_D32_SFLOAT,
                                   0, 0x1000, 128)
               .ok(),
          "skin: zero joints rejected");
    check(!sfr.create_mesh_skinned(dev, sc.format(), VK_FORMAT_D32_SFLOAT,
                                   kSkinMaxJoints + 1, 0x1000, 64ull * 4096)
               .ok(),
          "skin: too many joints rejected");
    check(!sfr.create_mesh_skinned(dev, sc.format(),
                                   VK_FORMAT_R8G8B8A8_UNORM, 2, 0x1000,
                                   128)
               .ok(),
          "skin: non-D32 depth rejected");
    {
      const std::uint32_t bad[8] = {0, 1, 2, 3, 4, 5, 6, 7};
      check(!sfr.create_mesh_skinned(dev, sc.format(),
                                     VK_FORMAT_D32_SFLOAT, 2, 0x1000, 128,
                                     bad, 8, bad, 8)
                 .ok(),
            "skin: bad blob rejected");
    }

    // Quad: left half skinned to joint 1 (weight 1), right half pinned
    // to joint 0 (identity). NDC-space positions; identity MVP.
    constexpr std::uint32_t kSkinJoints = 2;
    constexpr std::uint64_t kUboBytes = kSkinJoints * 64;
    std::array<MeshVertex, 4> qv{};
    const float qx[4] = {-0.8F, -0.05F, 0.05F, 0.8F};
    for (int i = 0; i < 4; ++i) {
      qv[static_cast<std::size_t>(i)].pos[0] = qx[i];
      qv[static_cast<std::size_t>(i)].pos[1] =
          (i == 0 || i == 3) ? -0.8F : 0.8F;
      qv[static_cast<std::size_t>(i)].pos[2] = 0.0F;
      qv[static_cast<std::size_t>(i)].normal[0] = 0.0F;
      qv[static_cast<std::size_t>(i)].normal[1] = 0.0F;
      qv[static_cast<std::size_t>(i)].normal[2] = 1.0F;
      qv[static_cast<std::size_t>(i)].uv[0] = 0.0F;
      qv[static_cast<std::size_t>(i)].uv[1] = 0.0F;
    }
    const std::array<std::uint32_t, 6> qi = {0, 1, 2, 0, 2, 3};
    std::array<SkinnedVertexExtra, 4> qskin{};
    for (int i = 0; i < 4; ++i) {
      auto& s = qskin[static_cast<std::size_t>(i)];
      // Left half (verts 0,1) -> joint 1; right half (2,3) -> joint 0.
      const bool left = (i < 2);
      s.joints[0] = left ? 1u : 0u;
      s.joints[1] = 0u;
      s.joints[2] = 0u;
      s.joints[3] = 0u;
      s.weights[0] = 1.0F;
      s.weights[1] = 0.0F;
      s.weights[2] = 0.0F;
      s.weights[3] = 0.0F;
    }
    // Weight-normalization twin: same joints, halved weights (sum 0.5 —
    // the shader divides by the live sum, so output must match).
    std::array<SkinnedVertexExtra, 4> qskin_half = qskin;
    for (auto& s : qskin_half) {
      s.weights[0] = 0.5F;
    }
    // Unskinned-fallback twin: zero weights everywhere -> bind pose.
    std::array<SkinnedVertexExtra, 4> qskin_zero{};
    for (auto& s : qskin_zero) {
      s.joints[0] = 1u;
      s.weights[0] = 0.0F;
    }

    auto qust = skinned_mesh.create(dev, qv.data(), 4, qi.data(), 6);
    check(qust.ok() && skinned_mesh.ready(), "skin: quad uploaded (4v/6i)");
    GpuMesh skin_buf;
    auto skst = skin_buf.create_raw(dev, qskin.data(), sizeof(qskin[0]), 4,
                                    qi.data(), 6);
    check(skst.ok() && skin_buf.ready(), "skin: skin buffer uploaded");
    GpuMesh skin_buf_half;
    auto skst2 = skin_buf_half.create_raw(dev, qskin_half.data(),
                                          sizeof(qskin_half[0]), 4, qi.data(),
                                          6);
    check(skst2.ok() && skin_buf_half.ready(),
          "skin: half-weight buffer uploaded");
    GpuMesh skin_buf_zero;
    auto skst3 = skin_buf_zero.create_raw(dev, qskin_zero.data(),
                                          sizeof(qskin_zero[0]), 4, qi.data(),
                                          6);
    check(skst3.ok() && skin_buf_zero.ready(),
          "skin: zero-weight buffer uploaded");

    // Joint palette: host-visible buffer (STORAGE usage) via the device proc
    // table — the same alloc_host pattern as test_gpu_indirect. Local
    // struct: golden_vk_helper.hpp drags in the whole PBR pipeline, so
    // this TU keeps its own minimal buffer helper.
    struct HostBuf {
      VkBuffer buf{nullptr};
      VkDeviceMemory mem{nullptr};
    };
    auto load_proc = [&](const char* name) -> void* {
      void* p = dev.proc(name);
      return (p != nullptr) ? p : dev.instance_proc(name);
    };
    auto p_create_buffer = reinterpret_cast<PFN_vkCreateBuffer>(
        load_proc("vkCreateBuffer"));
    auto p_destroy_buffer = reinterpret_cast<PFN_vkDestroyBuffer>(
        load_proc("vkDestroyBuffer"));
    auto p_get_buffer_reqs =
        reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(
            load_proc("vkGetBufferMemoryRequirements"));
    auto p_get_mem_props =
        reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            load_proc("vkGetPhysicalDeviceMemoryProperties"));
    auto p_alloc_mem = reinterpret_cast<PFN_vkAllocateMemory>(
        load_proc("vkAllocateMemory"));
    auto p_free_mem =
        reinterpret_cast<PFN_vkFreeMemory>(load_proc("vkFreeMemory"));
    auto p_bind_mem = reinterpret_cast<PFN_vkBindBufferMemory>(
        load_proc("vkBindBufferMemory"));
    auto p_map_mem =
        reinterpret_cast<PFN_vkMapMemory>(load_proc("vkMapMemory"));
    auto p_unmap_mem =
        reinterpret_cast<PFN_vkUnmapMemory>(load_proc("vkUnmapMemory"));
    check(p_create_buffer != nullptr && p_destroy_buffer != nullptr &&
              p_get_buffer_reqs != nullptr && p_get_mem_props != nullptr &&
              p_alloc_mem != nullptr && p_free_mem != nullptr &&
              p_bind_mem != nullptr && p_map_mem != nullptr &&
              p_unmap_mem != nullptr,
          "skin: procs resolve");
    HostBuf ubo{};
    bool ubo_ok = false;
    if (p_create_buffer != nullptr && p_get_buffer_reqs != nullptr &&
        p_get_mem_props != nullptr && p_alloc_mem != nullptr &&
        p_bind_mem != nullptr) {
      VkBufferCreateInfo bc{};
      bc.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      bc.size = kUboBytes;
      bc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      bc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      if (p_create_buffer(dev.device(), &bc, nullptr, &ubo.buf) ==
          VK_SUCCESS) {
        VkMemoryRequirements req{};
        p_get_buffer_reqs(dev.device(), ubo.buf, &req);
        VkPhysicalDeviceMemoryProperties mp{};
        p_get_mem_props(dev.physical_device(), &mp);
        std::uint32_t mi = 0xFFFFFFFFu;
        for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
          if ((req.memoryTypeBits & (1u << i)) != 0u &&
              (mp.memoryTypes[i].propertyFlags &
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0u &&
              (mp.memoryTypes[i].propertyFlags &
               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0u) {
            mi = i;
            break;
          }
        }
        if (mi != 0xFFFFFFFFu) {
          VkMemoryAllocateInfo ai{};
          ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
          ai.allocationSize = req.size;
          ai.memoryTypeIndex = mi;
          if (p_alloc_mem(dev.device(), &ai, nullptr, &ubo.mem) ==
                  VK_SUCCESS &&
              p_bind_mem(dev.device(), ubo.buf, ubo.mem, 0) == VK_SUCCESS) {
            ubo_ok = true;
          }
        }
      }
    }
    auto free_ubo = [&]() {
      if (ubo.buf != nullptr && p_destroy_buffer != nullptr) {
        p_destroy_buffer(dev.device(), ubo.buf, nullptr);
        ubo.buf = nullptr;
      }
      if (ubo.mem != nullptr && p_free_mem != nullptr) {
        p_free_mem(dev.device(), ubo.mem, nullptr);
        ubo.mem = nullptr;
      }
    };
    check(ubo_ok, "skin: joint UBO allocated");
    bool skin_gpu_ok = qust.ok() && skst.ok() && skst2.ok() && skst3.ok() &&
                       ubo_ok;
    // Column-major joint matrices: joint 0 = identity, joint 1 =
    // translate +0.5 X (bind pose) — rewritten to -0.5 X for the bent
    // pose between frames. Sized to the 2-joint UBO (kUboBytes).
    std::array<float, kSkinJoints * 16> joints{};
    auto write_joints = [&](float tx1) {
      for (int i = 0; i < 16; ++i) {
        joints[static_cast<std::size_t>(i)] =
            (i == 0 || i == 5 || i == 10 || i == 15) ? 1.0F : 0.0F;
        joints[16 + static_cast<std::size_t>(i)] =
            (i == 0 || i == 5 || i == 10 || i == 15) ? 1.0F : 0.0F;
      }
      joints[16 + 12] = tx1;
      if (p_map_mem == nullptr || p_unmap_mem == nullptr) {
        return false;
      }
      void* p = nullptr;
      if (p_map_mem(dev.device(), ubo.mem, 0, sizeof(float) * joints.size(),
                    0, &p) != VK_SUCCESS ||
          p == nullptr) {
        return false;
      }
      const auto* src = reinterpret_cast<const std::uint8_t*>(joints.data());
      auto* bytes = static_cast<std::uint8_t*>(p);
      for (std::size_t i = 0; i < kUboBytes; ++i) {
        bytes[i] = src[i];
      }
      p_unmap_mem(dev.device(), ubo.mem);
      return true;
    };
    if (skin_gpu_ok) {
      check(write_joints(0.5F), "skin: bind-pose joints uploaded");
      const std::uint64_t ubo_bits =
          static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
              ubo.buf));
      auto skpl = sfr.create_mesh_skinned(dev, sc.format(),
                                          VK_FORMAT_D32_SFLOAT, kSkinJoints,
                                          ubo_bits, kUboBytes);
      check(skpl.ok() && sfr.mesh_skinned_ready() &&
                sfr.mesh_skinned_module_count() == 2 &&
                sfr.mesh_skinned_joints() == kSkinJoints,
            "skin: skinned pipeline built (2 joints)");
      if (!skpl.ok()) {
        std::printf("       skinned create failed: %s\n", skpl.message);
        skin_gpu_ok = false;
      } else {
        check(!sfr.create_mesh_skinned(dev, sc.format(),
                                       VK_FORMAT_D32_SFLOAT, kSkinJoints,
                                       ubo_bits, kUboBytes)
                   .ok(),
              "skin: double build rejected");
        GpuMesh bad_mesh;
        auto probe = fc.begin_frame();
        check(probe.ok && probe.cmd != nullptr, "skin: probe frame acquired");
        if (probe.ok && probe.cmd != nullptr) {
          check(!sfr.record_mesh_skinned(nullptr, skinned_mesh,
                                         skin_buf.vertex_buffer_bits(), empty,
                                         ubo_bits, kUboBytes, kSkinJoints)
                     .ok(),
                "skin: null cmd rejected");
          check(!sfr.record_mesh_skinned(probe.cmd, bad_mesh,
                                         skin_buf.vertex_buffer_bits(), empty,
                                         ubo_bits, kUboBytes, kSkinJoints)
                     .ok(),
                "skin: empty mesh rejected");
          check(!sfr.record_mesh_skinned(probe.cmd, skinned_mesh, 0, empty,
                                         ubo_bits, kUboBytes, kSkinJoints)
                     .ok(),
                "skin: null skin buffer rejected");
          check(!sfr.record_mesh_skinned(probe.cmd, skinned_mesh,
                                         skin_buf.vertex_buffer_bits(), empty,
                                         0, kUboBytes, kSkinJoints)
                     .ok(),
                "skin: null UBO rejected");
          check(!sfr.record_mesh_skinned(probe.cmd, skinned_mesh,
                                         skin_buf.vertex_buffer_bits(), empty,
                                         ubo_bits, 64, kSkinJoints)
                     .ok(),
                "skin: short UBO rejected");
          check(!sfr.record_mesh_skinned(probe.cmd, skinned_mesh,
                                         skin_buf.vertex_buffer_bits(), empty,
                                         ubo_bits, kUboBytes, kSkinJoints + 1)
                     .ok(),
                "skin: joint-count mismatch rejected");
          // Drain the probe frame (begin_frame acquires a swapchain image
          // and starts a command buffer; end it without submitting draws).
          (void)fc.end_frame(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        }
      }
    }
    // Render one skinned frame with the CURRENT joint contents; returns
    // false on any failure. Identity MVP/model: quad NDC verts map
    // straight to the 320x240 target.
    auto render_skinned = [&](std::uint64_t skin_bits,
                              std::vector<std::uint8_t>& out) -> bool {
      // Single CLEAR pass with the skinned draw directly (no empty pass +
      // LOAD continuation: the empty-encoder + pipeline-switch shape
      // renders blank on MoltenVK while validating clean on Lavapipe —
      // keep the pass shape structurally identical to the proven legacy
      // single-pass draw).
      engine::test::HelperFrame hf{};
      if (!engine::test::helper_frame_open(fr, fc, sc, depth, bars, depth_live,
                                           &hf, true)) {
        return false;
      }
      VkCommandBuffer cmd = hf.cmd;
      VkImage img = hf.img;
      MeshPush push{};
      const Mat4 ident = helper_mat_identity();
      const float ldir[4] = {0.0F, 0.0F, -1.0F, 0.0F};
      const float bcol[4] = {0.85F, 0.5F, 0.2F, 1.0F};
      helper_fill_push(push.mvp, push.model, push.light_dir, push.base_color, ident, ident, ldir, bcol);
      const std::uint64_t ubo_bits =
          static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
              ubo.buf));
      if (!sfr.record_mesh_skinned(cmd, skinned_mesh, skin_bits, push,
                                   ubo_bits, kUboBytes, kSkinJoints)
               .ok()) {
        return false;
      }
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
      return out.size() == 320u * 240u * 4u;
    };
    if (skin_gpu_ok) {
      auto lit_pixels = [](const std::vector<std::uint8_t>& px) {
        std::size_t n = 0;
        for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
          if (px[i] > 8 || px[i + 1] > 8 || px[i + 2] > 8) {
            ++n;
          }
        }
        return n;
      };
      auto diff_pixels = [](const std::vector<std::uint8_t>& a,
                            const std::vector<std::uint8_t>& b) {
        std::size_t n = 0;
        const std::size_t m = (a.size() < b.size()) ? a.size() : b.size();
        for (std::size_t i = 0; i + 3 < m; i += 4) {
          int dr = static_cast<int>(a[i]) - static_cast<int>(b[i]);
          int dg =
              static_cast<int>(a[i + 1]) - static_cast<int>(b[i + 1]);
          int db =
              static_cast<int>(a[i + 2]) - static_cast<int>(b[i + 2]);
          if (dr < -2 || dr > 2 || dg < -2 || dg > 2 || db < -2 || db > 2) {
            ++n;
          }
        }
        return n;
      };
      std::vector<std::uint8_t> px_bind;
      check(render_skinned(skin_buf.vertex_buffer_bits(), px_bind),
            "skin: bind-pose frame captured");
      bool have_bind = !px_bind.empty();
      if (have_bind) {
        check(lit_pixels(px_bind) > 500, "skin: bind pose covers pixels");
      }
      // Bent pose: joint 1 moves -0.5 X (left half swings left).
      bool bent_ok = write_joints(-0.5F);
      check(bent_ok, "skin: bent-pose joints uploaded");
      std::vector<std::uint8_t> px_bent;
      bool bent_captured =
          bent_ok &&
          render_skinned(skin_buf.vertex_buffer_bits(), px_bent);
      check(bent_captured, "skin: bent-pose frame captured");
      if (have_bind && bent_captured && !px_bent.empty()) {
        // Bent pose deforms: a large pixel block must differ, and the
        // right (joint-0) half must stay put — compare the right-center
        // 40x40 sample identical, full frame different.
        const std::size_t diff = diff_pixels(px_bind, px_bent);
        check(diff > 500, "skin: bent pose deforms the mesh");
        std::size_t right_same = 0;
        std::size_t right_total = 0;
        for (std::uint32_t y = 100; y < 140; ++y) {
          for (std::uint32_t x = 220; x < 260; ++x) {
            const std::size_t o =
                (static_cast<std::size_t>(y) * 320u + x) * 4u;
            if (o + 3 >= px_bind.size() || o + 3 >= px_bent.size()) {
              continue;
            }
            ++right_total;
            int dr =
                static_cast<int>(px_bind[o]) - static_cast<int>(px_bent[o]);
            int dg = static_cast<int>(px_bind[o + 1]) -
                     static_cast<int>(px_bent[o + 1]);
            int db = static_cast<int>(px_bind[o + 2]) -
                     static_cast<int>(px_bent[o + 2]);
            if (dr >= -2 && dr <= 2 && dg >= -2 && dg <= 2 && db >= -2 &&
                db <= 2) {
              ++right_same;
            }
          }
        }
        check(right_total > 0 && right_same == right_total,
              "skin: joint-0 half unmoved by bent pose");
      }
      // Weight normalization: halved weights == full weights.
      std::vector<std::uint8_t> px_half;
      bool half_captured =
          render_skinned(skin_buf_half.vertex_buffer_bits(), px_half);
      check(half_captured, "skin: half-weight frame captured");
      if (bent_captured && half_captured && !px_half.empty()) {
        check(diff_pixels(px_bent, px_half) == 0,
              "skin: halved weights match (normalization)");
      }
      // Unskinned fallback: zero weights + joint-1 translate = bind pose
      // of the ORIGINAL quad (no +0.5 shift either) — so it must differ
      // from the bent frame but equal a freshly rendered zero-weight
      // bind frame regardless of joint contents.
      // Pool recycle (CodeRabbit round-3): per-draw allocation without
      // a reset point stops after kSkinPoolSets draws. Reset after the
      // completed frames, then draw again — must succeed and match.
      bool recycled = false;
      if (bent_captured) {
        recycled = sfr.reset_mesh_skinned_pool().ok();
        check(recycled, "skin: pool reset ok");
        std::vector<std::uint8_t> px_again;
        if (recycled &&
            render_skinned(skin_buf.vertex_buffer_bits(), px_again)) {
          check(!px_again.empty() && px_again.size() == px_bent.size() &&
                    diff_pixels(px_bent, px_again) == 0,
                "skin: post-reset draw matches");
        }
      }
      std::vector<std::uint8_t> px_zero;
      bool zero_captured =
          render_skinned(skin_buf_zero.vertex_buffer_bits(), px_zero);
      check(zero_captured, "skin: zero-weight frame captured");
      if (bent_captured && zero_captured && !px_zero.empty()) {
        check(diff_pixels(px_bent, px_zero) > 500,
              "skin: zero weights fall back to bind pose");
      }
    }
    if (ubo.buf != nullptr || ubo.mem != nullptr) {
      free_ubo();
    }
    skin_buf_zero.destroy();
    skin_buf_half.destroy();
    skin_buf.destroy();
    skinned_mesh.destroy();
    sfr.destroy();
    check(!sfr.mesh_skinned_ready(), "skin: destroy idles");
  }

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

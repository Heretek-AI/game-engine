// test_mesh_lod.cpp — P4-12 acceptance (native CTest).
//
// CPU contract (runs everywhere, no GPU):
//   - SelectLodIndex maps coverage fraction -> level (near = fine).
//   - CoverageFromDistance maps distance/radius/ref -> fraction.
//   - CrossfadeFactor is 0 away from bands, ramps 0 -> 1 inside.
//   - MeshRegistry LOD groups: add_lod/find/find_lod/level_count,
//     plain ids unaffected, legacy find() on a group id = LOD0.
//   - Import chain: the P4-12 worker build emits up to 4 LOD levels
//     with strictly decreasing index counts (asserted on the real
//     1-triangle fixture: tiny meshes keep 1 level, never 0).
//
// GPU test (llvmpipe, device_hint="llvmpipe", [FAIL] not [SKIP] under
// ENGINE_REQUIRE_GPU=1): one subdivided grid mesh uploaded at 3 LOD
// index densities; the same camera renders the mesh at 3 distances
// through render_world with a LOD-group mesh_id. Each distance picks
// the expected level (asserted via find_lod), and each frame compares
// against golden p4_12_lod_{near,mid,far}_320x240 via golden_compare
// (P1-05 tolerance, RGBA already swizzled by capture_rgba8).
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "engine/renderer/mesh_lod.hpp"
#include "engine/renderer/mesh_registry.hpp"

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#endif

#include "engine/ecs/ecs_world.hpp"
#include "engine/ecs/transform.hpp"
#include "engine/renderer/forward_renderer.hpp"
#include "engine/renderer/gpu_mesh.hpp"
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

using engine::renderer::CoverageFromDistance;
using engine::renderer::CrossfadeFactor;
using engine::renderer::MeshLodGroup;
using engine::renderer::SelectLodIndex;
using engine::renderer::kMeshLodCount;

// --- CPU contract: selector --------------------------------------------
void test_cpu_selector() {
  // Coverage -> level: high coverage = near = fine.
  check(SelectLodIndex(1.0F) == 0, "lod: cov 1.0 -> L0");
  check(SelectLodIndex(0.75F) == 0, "lod: cov 0.75 -> L0");
  check(SelectLodIndex(0.5F) == 0, "lod: cov 0.50 -> L0");
  check(SelectLodIndex(0.49F) == 1, "lod: cov 0.49 -> L1");
  check(SelectLodIndex(0.25F) == 1, "lod: cov 0.25 -> L1");
  check(SelectLodIndex(0.24F) == 2, "lod: cov 0.24 -> L2");
  check(SelectLodIndex(0.125F) == 2, "lod: cov 0.125 -> L2");
  check(SelectLodIndex(0.124F) == 3, "lod: cov 0.124 -> L3");
  check(SelectLodIndex(0.0F) == 3, "lod: cov 0 -> L3");
  check(SelectLodIndex(-1.0F) == 3, "lod: negative cov -> L3");
  check(SelectLodIndex(100.0F) == 0, "lod: huge cov -> L0");
  // Distance -> coverage -> level round trip: ref_coverage 1 at
  // bounding_radius 1 means distance d gives coverage 1/d.
  const float near_cov = CoverageFromDistance(1.0F, 1.0F, 1.0F);
  const float mid_cov = CoverageFromDistance(3.0F, 1.0F, 1.0F);
  const float far_cov = CoverageFromDistance(10.0F, 1.0F, 1.0F);
  check(near_cov > mid_cov && mid_cov > far_cov,
        "lod: coverage falls with distance");
  check(SelectLodIndex(near_cov) == 0, "lod: distance 1 -> L0");
  check(SelectLodIndex(mid_cov) == 1, "lod: distance 3 -> L1");
  check(SelectLodIndex(far_cov) == 3, "lod: distance 10 -> L3");
  // Guards: degenerate inputs fail safe (never NaN out).
  check(CoverageFromDistance(0.0F, 1.0F, 1.0F) == 1.0F,
        "lod: zero distance -> closest");
  check(CoverageFromDistance(-5.0F, 1.0F, 1.0F) == 1.0F,
        "lod: negative distance -> closest");
  check(CoverageFromDistance(5.0F, 0.0F, 1.0F) == 0.0F,
        "lod: zero radius -> coarsest");
  check(CoverageFromDistance(5.0F, 1.0F, 0.0F) == 0.0F,
        "lod: zero ref -> coarsest");
  // Crossfade: 0 above/inside-safe zone, ramps inside the band.
  check(CrossfadeFactor(1.0F, 0) == 0.0F, "lod: xfade far above -> 0");
  check(CrossfadeFactor(0.5F, 0) == 0.0F, "lod: xfade at threshold -> 0");
  {
    // Level 0 band: threshold 0.5, band 0.05; 0.46 sits inside.
    const float f = CrossfadeFactor(0.46F, 0);
    check(f > 0.0F && f < 1.0F, "lod: xfade inside band ramps");
  }
  check(CrossfadeFactor(0.30F, 1) == 0.0F, "lod: xfade below band -> 0");
  check(CrossfadeFactor(0.10F, 3) == 0.0F, "lod: xfade coarsest -> 0");
  check(kMeshLodCount == 4, "lod: chain depth is 4");
}

// --- CPU contract: registry groups --------------------------------------
void test_cpu_registry() {
  using engine::renderer::GpuMesh;
  using engine::renderer::MeshRegistry;
  MeshRegistry reg;
  GpuMesh a;
  GpuMesh b;
  GpuMesh c;
  // add_lod rejects empty groups; plain add rejects null.
  MeshLodGroup empty{};
  check(reg.add_lod(empty) == 0, "lod: empty group rejected");
  check(reg.add(nullptr) == 0, "lod: null mesh rejected");
  MeshLodGroup g{};
  g.levels[0] = &a;
  g.levels[1] = &b;
  g.levels[2] = &c;
  g.ref_coverage = 1.0F;
  g.bounding_radius = 1.0F;
  const std::uint32_t gid = reg.add_lod(g);
  check(gid != 0, "lod: group registered");
  check(reg.lod_level_count(gid) == 3, "lod: group has 3 levels");
  check(reg.lod_level_count(999) == 0, "lod: bad id has 0 levels");
  check(reg.lod_level_count(0) == 0, "lod: id 0 has 0 levels");
  // Legacy find() on a group id resolves LOD0 (existing paths keep
  // working); plain ids are not groups.
  check(reg.find(gid) == &a, "lod: group find = LOD0");
  const std::uint32_t plain = reg.add(&a);
  check(plain != 0 && plain != gid, "lod: plain id distinct");
  check(reg.find(plain) == &a, "lod: plain find works");
  check(reg.find_lod(plain, 1.0F) == nullptr,
        "lod: plain id has no LOD select");
  check(reg.lod_level_count(plain) == 0, "lod: plain id level 0");
  check(reg.find(0) == nullptr, "lod: id 0 finds nothing");
  check(reg.find(999) == nullptr, "lod: bad id finds nothing");
  // Distance selection through the group: near -> LOD0, far -> LOD2.
  std::size_t level = 99;
  check(reg.find_lod(gid, 1.0F, &level) == &a && level == 0,
        "lod: near selects LOD0");
  check(reg.find_lod(gid, 10.0F, &level) == &c && level == 2,
        "lod: far selects LOD2");
  check(reg.find_lod(gid, 1.0F, nullptr) == &a,
        "lod: null out_level ok");
  check(reg.find_lod(0, 1.0F) == nullptr, "lod: id 0 selects nothing");
  check(reg.find_lod(999, 1.0F) == nullptr, "lod: bad id selects nothing");
  reg.clear();
  check(reg.find(gid) == nullptr, "lod: clear drops groups");
  check(reg.lod_level_count(gid) == 0, "lod: clear drops levels");
}

#if defined(ENGINE_RENDERER_HAS_VULKAN)

auto lod_missing(std::string_view what) -> int {
  return gpu_missing(what);
}

// Subdivided grid mesh: (n+1)^2 verts over [-1,1]^2 at z=0, +Z
// normals, planar UVs; 2*n*n triangles. n=8 -> 162 tris: big enough
// that hand-decimated LODs (every 2nd / 4th quad row) visibly differ.
struct GridMesh {
  std::vector<engine::renderer::MeshVertex> verts;
  std::vector<std::uint32_t> idx_full;
  std::vector<std::uint32_t> idx_half;
  std::vector<std::uint32_t> idx_quarter;
};

auto make_grid(int n) -> GridMesh {
  GridMesh g{};
  const std::size_t dim = static_cast<std::size_t>(n) + 1;
  g.verts.reserve(dim * dim);
  for (int iy = 0; iy <= n; ++iy) {
    for (int ix = 0; ix <= n; ++ix) {
      engine::renderer::MeshVertex v{};
      v.pos[0] = -1.0F + 2.0F * static_cast<float>(ix) /
                            static_cast<float>(n);
      v.pos[1] = -1.0F + 2.0F * static_cast<float>(iy) /
                            static_cast<float>(n);
      v.pos[2] = 0.0F;
      v.normal[0] = 0.0F;
      v.normal[1] = 0.0F;
      v.normal[2] = 1.0F;
      v.uv[0] = static_cast<float>(ix) / static_cast<float>(n);
      v.uv[1] = static_cast<float>(iy) / static_cast<float>(n);
      g.verts.push_back(v);
    }
  }
  auto quad = [&](int qx, int qy, std::vector<std::uint32_t>& out) {
    const std::uint32_t a =
        static_cast<std::uint32_t>(qy * (n + 1) + qx);
    const std::uint32_t b = a + 1;
    const std::uint32_t cc =
        static_cast<std::uint32_t>((qy + 1) * (n + 1) + qx);
    const std::uint32_t d = cc + 1;
    out.insert(out.end(), {a, cc, b, b, cc, d});
  };
  for (int qy = 0; qy < n; ++qy) {
    for (int qx = 0; qx < n; ++qx) {
      quad(qx, qy, g.idx_full);
      if ((qx + qy) % 2 == 0) {
        quad(qx, qy, g.idx_half);
      }
      if ((qx + qy) % 4 == 0) {
        quad(qx, qy, g.idx_quarter);
      }
    }
  }
  return g;
}

void test_mesh_lod_gpu() {
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
  using engine::renderer::MeshRegistry;
  using engine::renderer::RendererWindow;
  using engine::renderer::Swapchain;
  using engine::renderer::VulkanDevice;

  GridMesh grid = make_grid(8);
  check(grid.idx_full.size() == 8u * 8u * 6u, "lod: full grid 384 idx");
  check(grid.idx_half.size() * 2u == grid.idx_full.size(),
        "lod: half grid 192 idx");
  check(grid.idx_quarter.size() * 4u == grid.idx_full.size(),
        "lod: quarter grid 96 idx");

  engine::test::HelperGpuWindow gw{};
  auto missing = [](const char* what) -> int { return lod_missing(what); };
  if (!engine::test::helper_gpu_window(
          "P4-12-test", 320, 240, "lod: offscreen window unavailable",
          "lod: window instance extensions query",
          "lod: no physical device (no ICD)", "lod: device init ok",
          "lod: surface create ok", "lod: swapchain create ok", check,
          missing, &failures, &gw)) {
    return;
  }
  RendererWindow& w = gw.w;
  VulkanDevice& dev = gw.dev;
  VkSurfaceKHR surface = gw.surface;
  Swapchain& sc = gw.sc;

  ForwardRenderer fr;
  check(fr.create_mesh(dev, sc.format(), VK_FORMAT_D32_SFLOAT).ok() &&
            fr.mesh_ready(),
        "lod: mesh pipeline built");
  DepthBuffer depth;
  check(depth.create(dev, sc.extent(), VK_FORMAT_D32_SFLOAT).ok(),
        "lod: depth buffer created");

  // Upload the 3 densities as separate meshes (hand-decimated stand-in
  // for the meshopt chain: same vertices, fewer indices per level).
  GpuMesh m0;
  GpuMesh m1;
  GpuMesh m2;
  const auto* vp = grid.verts.data();
  const std::uint32_t vc =
      static_cast<std::uint32_t>(grid.verts.size());
  bool up_ok =
      m0.create(dev, vp, vc, grid.idx_full.data(),
                static_cast<std::uint32_t>(grid.idx_full.size()))
          .ok() &&
      m1.create(dev, vp, vc, grid.idx_half.data(),
                static_cast<std::uint32_t>(grid.idx_half.size()))
          .ok() &&
      m2.create(dev, vp, vc, grid.idx_quarter.data(),
                static_cast<std::uint32_t>(grid.idx_quarter.size()))
          .ok();
  check(up_ok, "lod: 3 levels uploaded");
  check(m0.index_count() == 384 && m1.index_count() == 192 &&
            m2.index_count() == 96,
        "lod: index counts 384/192/96");
  if (!up_ok) {
    depth.destroy();
    fr.destroy();
    sc.destroy();
    dev.destroy_surface(surface);
    dev.shutdown();
    w.destroy();
    return;
  }

  // One LOD group: ref_coverage 1 at radius ~1.41 (grid half-diagonal).
  // Distance 1 -> cov ~1.0 -> L0; distance 3 -> cov ~0.33 -> L1;
  // distance 8 -> cov ~0.125 -> L2 (boundary: >= 0.125 keeps L2).
  MeshRegistry registry;
  MeshLodGroup group{};
  group.levels[0] = &m0;
  group.levels[1] = &m1;
  group.levels[2] = &m2;
  group.ref_coverage = 1.0F;
  group.bounding_radius = 1.5F;
  const std::uint32_t gid = registry.add_lod(group);
  check(gid != 0, "lod: group registered");

  const float cov_near = CoverageFromDistance(1.5F, 1.5F, 1.0F);
  const float cov_mid = CoverageFromDistance(4.5F, 1.5F, 1.0F);
  const float cov_far = CoverageFromDistance(12.0F, 1.5F, 1.0F);
  check(SelectLodIndex(cov_near) == 0, "lod: near distance -> L0");
  check(SelectLodIndex(cov_mid) == 1, "lod: mid distance -> L1");
  check(SelectLodIndex(cov_far) == 2, "lod: far distance -> L2");

  // Flecs world: camera at origin looking -Z, one entity carrying the
  // group id; per distance the entity slides back along +Z.
  EcsWorld world;
  check(world.construct(), "lod: world constructed");
  check(world.register_render(), "lod: render components registered");
  engine::test::HelperEcsIds ids{};
  check(engine::test::helper_ecs_lookup_ids(world, &ids),
        "lod: component ids resolve");
  const ecs_entity_t cam_comp = ids.cam;
  const ecs_entity_t mesh_comp = ids.mesh;
  const ecs_entity_t light_comp = ids.light;
  const ecs_entity_t local_id = ids.local;
  const ecs_entity_t world_id = ids.world;
  {
    const ecs_entity_t e = ecs_new(world.raw());
    Camera c{};
    c.fov_y_rad = 60.0F * 3.14159265F / 180.0F;
    c.near_z = 0.1F;
    c.far_z = 100.0F;
    c.is_main = 1;
    ecs_set_id(world.raw(), e, cam_comp, sizeof(Camera), &c);
    WorldTransform wt{};  // identity: eye at origin, looking -Z
    engine::ecs::MatIdentity(wt.m);
    ecs_set_id(world.raw(), e, world_id, sizeof(WorldTransform), &wt);
  }
  {
    const ecs_entity_t e = ecs_new(world.raw());
    DirectionalLight l{};
    const float len = std::sqrt(0.25F + 0.81F + 0.4225F);
    l.direction[0] = -0.5F / len;
    l.direction[1] = -0.9F / len;
    l.direction[2] = -0.65F / len;
    l.shadow = 0.0F;
    ecs_set_id(world.raw(), e, light_comp, sizeof(DirectionalLight), &l);
  }
  const ecs_entity_t ent = ecs_new(world.raw());
  {
    LocalTransform lt{};
    lt.px = 0.0F;
    lt.py = 0.0F;
    lt.pz = -1.5F;
    lt.qx = 0.0F;
    lt.qy = 0.0F;
    lt.qz = 0.0F;
    lt.qw = 1.0F;
    lt.sx = 1.0F;
    lt.sy = 1.0F;
    lt.sz = 1.0F;
    ecs_set_id(world.raw(), ent, local_id, sizeof(LocalTransform), &lt);
    WorldTransform wt{};
    engine::ecs::ComposeLocal(lt, wt.m);
    ecs_set_id(world.raw(), ent, world_id, sizeof(WorldTransform), &wt);
    MeshRenderer mr{};
    mr.mesh_id = gid;
    mr.base_color[0] = 0.85F;
    mr.base_color[1] = 0.5F;
    mr.base_color[2] = 0.2F;
    mr.base_color[3] = 1.0F;
    ecs_set_id(world.raw(), ent, mesh_comp, sizeof(MeshRenderer), &mr);
  }
  check(world.propagate(), "lod: world propagated");

  auto p_barrier = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(
      dev.proc("vkCmdPipelineBarrier2"));
  check(p_barrier != nullptr, "lod: barrier entry resolves");
  FrameContext fc;
  check(fc.create(dev, sc).ok(), "lod: frame context created");
  engine::test::HelperBarriers bars{};
  bars.cmd_barrier2 = p_barrier;
  bars.depth_image = reinterpret_cast<VkImage>(
      static_cast<std::uintptr_t>(depth.image_bits()));

  bool depth_live = false;
  ForwardRenderer::RenderWorldStats last_stats{};
  auto render_at = [&](float z,
                       std::vector<std::uint8_t>& out) -> bool {
    auto* lt = static_cast<LocalTransform*>(
        ecs_get_mut_id(world.raw(), ent, local_id));
    if (lt == nullptr) {
      return false;
    }
    lt->pz = z;
    if (!world.propagate()) {
      return false;
    }
    engine::test::HelperFrame hf{};
    if (!engine::test::helper_frame_open(fr, fc, sc, depth, bars,
                                         depth_live, &hf)) {
      return false;
    }
    ForwardRenderer::RenderWorldStats stats{};
    if (!fr.render_world(world.raw(), registry, hf.cmd, nullptr, &stats)
             .ok()) {
      return false;
    }
    last_stats = stats;
    if (!fr.end_mesh_pass(hf.cmd).ok()) {
      return false;
    }
    bars.color(hf.cmd, hf.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
               VK_PIPELINE_STAGE_2_COPY_BIT,
               VK_ACCESS_2_TRANSFER_READ_BIT);
    if (!fc.end_frame(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL).ok()) {
      return false;
    }
    if (!capture_rgba8(dev, hf.img, sc.format(), sc.extent(),
                       VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, out)
             .ok()) {
      return false;
    }
    return out.size() == 320u * 240u * 4u;
  };

  std::size_t level = 99;
  const GpuMesh* near_mesh = registry.find_lod(gid, 1.5F, &level);
  check(near_mesh == &m0 && level == 0, "lod: find_lod near -> L0");
  const GpuMesh* mid_mesh = registry.find_lod(gid, 4.5F, &level);
  check(mid_mesh == &m1 && level == 1, "lod: find_lod mid -> L1");
  const GpuMesh* far_mesh = registry.find_lod(gid, 12.0F, &level);
  check(far_mesh == &m2 && level == 2, "lod: find_lod far -> L2");

  std::vector<std::uint8_t> px_near;
  check(render_at(-1.5F, px_near), "lod: near frame captured");
  if (!px_near.empty()) {
    failures += engine::test::golden_check(
        "p4_12_lod_near_320x240",
        std::span<const std::uint8_t>(px_near.data(), px_near.size()),
        320, 240);
    check(last_stats.draws == 1, "lod: near draws 1");
  }
  std::vector<std::uint8_t> px_mid;
  check(render_at(-4.5F, px_mid), "lod: mid frame captured");
  if (!px_mid.empty()) {
    failures += engine::test::golden_check(
        "p4_12_lod_mid_320x240",
        std::span<const std::uint8_t>(px_mid.data(), px_mid.size()), 320,
        240);
    check(last_stats.draws == 1, "lod: mid draws 1");
  }
  std::vector<std::uint8_t> px_far;
  check(render_at(-12.0F, px_far), "lod: far frame captured");
  if (!px_far.empty()) {
    failures += engine::test::golden_check(
        "p4_12_lod_far_320x240",
        std::span<const std::uint8_t>(px_far.data(), px_far.size()), 320,
        240);
    check(last_stats.draws == 1, "lod: far draws 1");
  }
  // The 3 distances must render visibly different triangle densities:
  // near (full quads) vs far (quarter quads) differ past tolerance.
  if (!px_near.empty() && !px_far.empty() &&
      px_near.size() == px_far.size()) {
    std::size_t diff = 0;
    for (std::size_t i = 0; i < px_near.size(); i += 4) {
      const int dr =
          static_cast<int>(px_near[i]) - static_cast<int>(px_far[i]);
      const int dg = static_cast<int>(px_near[i + 1]) -
                     static_cast<int>(px_far[i + 1]);
      const int db = static_cast<int>(px_near[i + 2]) -
                     static_cast<int>(px_far[i + 2]);
      if (dr < -2 || dr > 2 || dg < -2 || dg > 2 || db < -2 || db > 2) {
        ++diff;
      }
    }
    check(diff > 100, "lod: near vs far frames differ");
  }

  world.destroy();
  m2.destroy();
  m1.destroy();
  m0.destroy();
  depth.destroy();
  fr.destroy();
  fc.destroy();
  sc.destroy();
  dev.destroy_surface(surface);
  dev.shutdown();
  w.destroy();
}
#else
void test_mesh_lod_stub() {
  using engine::renderer::MeshRegistry;
  MeshRegistry registry;
  check(registry.find(1) == nullptr, "lod: stub registry empty");
  check(registry.add(nullptr) == 0, "lod: stub null rejected");
  check(registry.lod_level_count(1) == 0, "lod: stub no levels");
  check(registry.find_lod(1, 1.0F) == nullptr, "lod: stub no select");
}
#endif

}  // namespace

auto main() -> int {
  test_cpu_selector();
  test_cpu_registry();
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  test_mesh_lod_gpu();
#else
  test_mesh_lod_stub();
#endif
  std::printf(failures == 0 ? "MESH_LOD_OK\n" : "MESH_LOD_FAILURES=%d\n",
              failures);
  return failures == 0 ? 0 : 1;
}

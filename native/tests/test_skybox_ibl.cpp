// test_skybox_ibl.cpp — P4-05 irradiance + prefilter + skybox/IBL integration.
//
// CPU contract (no GPU; runs on stub builds too): IrradianceValidateImport
// plans, PrefilterValidateImport plans (mip counts + byte totals),
// PrefilterMipRoughness mapping (0 at mip 0, 1 at last), IblBasis
// orthonormality, and IblEvaluate split-sum wiring (chrome shows sharp
// specular + no diffuse; rough dielectric shows diffuse + biased spec).
//
// GPU section A (compute): the checked-in irradiance + prefilter shaders
// generate maps from a synthetic HDR cubemap; every texel is compared
// against CPU references (cosine-weighted hemisphere mean via
// IblIrradianceReference for irradiance; LOD-0 prefilter matches the
// source texel since rough=0 samples the normal direction only).
// GPU section B (graphics): the checked-in ibl_shade pair renders the
// chrome/rough split + skybox backdrop into an RGBA32F offscreen target;
// probe pixels are compared against CPU IblEvaluate over analytic sphere
// normals. [FAIL] (not [SKIP]) under ENGINE_REQUIRE_GPU=1 (rule 11).
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#endif

#include "engine/renderer/brdf_lut.hpp"
#include "engine/renderer/skybox_ibl.hpp"
#include "engine/renderer/skybox_image_based_lighting.hpp"
#include "engine/renderer/vulkan_device.hpp"

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#include "engine/renderer/bindless_table.hpp"
#include "engine/renderer/hdr_camera.hpp"
#include "engine/renderer/pbr_pipeline.hpp"
#include "engine/renderer/shaders/equirect_to_cube_spv.inc"
#include "engine/renderer/shaders/ibl_shade_frag_spv.inc"
#include "engine/renderer/shaders/ibl_shade_vert_spv.inc"
#include "engine/renderer/shaders/irradiance_spv.inc"
#include "engine/renderer/shaders/pbr_shade_frag_spv.inc"
#include "engine/renderer/shaders/pbr_shade_vert_spv.inc"
#include "engine/renderer/shaders/pbr_vis_frag_spv.inc"
#include "engine/renderer/shaders/pbr_vis_vert_spv.inc"
#include "engine/renderer/shaders/post_frag_spv.inc"
#include "engine/renderer/shaders/post_vert_spv.inc"
#include "engine/renderer/shaders/prefilter_spv.inc"
#include "engine/renderer/shaders/skybox_frag_spv.inc"
#include "engine/renderer/shaders/skybox_vert_spv.inc"
#include "golden_compare.hpp"
#include "golden_pipe_helper.hpp"
#include "golden_vk_helper.hpp"
#endif

namespace {

auto failures = 0;

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}

// --- CPU contract -----------------------------------------------------------

void test_cpu_validate() {
  using engine::renderer::IrradianceImportParams;
  using engine::renderer::IrradianceImportPlan;
  using engine::renderer::PrefilterImportParams;
  using engine::renderer::PrefilterImportPlan;
  using engine::renderer::kIrradianceFaceSize;
  IrradianceImportPlan ip{};
  IrradianceImportParams def{};
  check(IrradianceValidateImport(def, &ip), "ibl: irr default plan ok");
  check(ip.face_size == kIrradianceFaceSize, "ibl: irr default face 32");
  check(ip.total_bytes ==
            static_cast<std::uint64_t>(kIrradianceFaceSize) *
                kIrradianceFaceSize * 16u * 6u,
        "ibl: irr bytes face^2*16*6");
  IrradianceImportParams tiny{};
  tiny.face_size = 4;
  check(!IrradianceValidateImport(tiny, &ip), "ibl: irr face 4 rejected");
  IrradianceImportParams huge{};
  huge.face_size = 256;
  check(!IrradianceValidateImport(huge, &ip), "ibl: irr face 256 rejected");
  check(!IrradianceValidateImport(def, nullptr), "ibl: irr null out rejected");

  PrefilterImportPlan pp{};
  PrefilterImportParams pdef{};
  check(PrefilterValidateImport(pdef, &pp), "ibl: pref default plan ok");
  check(pp.base_size == 512, "ibl: pref default base 512");
  check(pp.mip_count == 10, "ibl: pref 512 -> 10 mips");
  // 512^2+256^2+...+1^2 = 349525 texels; *16B*6 layers = 33554400.
  check(pp.total_bytes == 33554400u, "ibl: pref bytes 512 chain");
  PrefilterImportParams small{};
  small.base_size = 64;
  check(PrefilterValidateImport(small, &pp) && pp.mip_count == 7,
        "ibl: pref 64 -> 7 mips");
  check(!PrefilterValidateImport(pdef, nullptr), "ibl: pref null out");
  PrefilterImportParams bad{};
  bad.base_size = 8;
  check(!PrefilterValidateImport(bad, &pp), "ibl: pref base 8 rejected");
}

void test_cpu_mip_mapping() {
  using engine::renderer::PrefilterMipEdge;
  using engine::renderer::PrefilterMipRoughness;
  check(PrefilterMipRoughness(0, 10) == 0.0F, "ibl: mip 0 rough 0");
  check(std::fabs(PrefilterMipRoughness(9, 10) - 1.0F) < 1e-6F,
        "ibl: last mip rough 1");
  check(std::fabs(PrefilterMipRoughness(1, 7) - (1.0F / 6.0F)) < 1e-6F,
        "ibl: mip 1/7 rough 1/6");
  check(PrefilterMipEdge(64, 0) == 64, "ibl: mip0 edge 64");
  check(PrefilterMipEdge(64, 6) == 1, "ibl: mip6 edge 1");
  check(PrefilterMipEdge(64, 9) == 1, "ibl: over-shift clamps 1");
}

void test_cpu_basis() {
  using engine::renderer::IblBasis;
  const std::array<std::array<float, 3>, 4> normals = {{
      {0.0F, 0.0F, 1.0F},
      {1.0F, 0.0F, 0.0F},
      {0.0F, 1.0F, 0.0F},
      {0.57735F, 0.57735F, 0.57735F},
  }};
  bool ok_all = true;
  for (const auto& n : normals) {
    // IblBasis takes C arrays (header contract, fixed size 3).
    float nn[3] = {n[0], n[1], n[2]};
    const float l = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
    nn[0] /= l;
    nn[1] /= l;
    nn[2] /= l;
    float t[3] = {0.0F, 0.0F, 0.0F};
    float b[3] = {0.0F, 0.0F, 0.0F};
    IblBasis(nn, t, b);
    const float dt = nn[0] * t[0] + nn[1] * t[1] + nn[2] * t[2];
    const float db = nn[0] * b[0] + nn[1] * b[1] + nn[2] * b[2];
    const float tb = t[0] * b[0] + t[1] * b[1] + t[2] * b[2];
    const float lt = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
    const float lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    if (std::fabs(dt) > 1e-5F || std::fabs(db) > 1e-5F ||
        std::fabs(tb) > 1e-5F || std::fabs(lt - 1.0F) > 1e-5F ||
        std::fabs(lb - 1.0F) > 1e-5F) {
      ok_all = false;
    }
  }
  check(ok_all, "ibl: basis orthonormal");
}

void test_cpu_evaluate() {
  using engine::renderer::IblEvaluate;
  // Chrome (metal 1): no diffuse, specular = pref * (F0 * scale + bias).
  float out[3] = {0.0F, 0.0F, 0.0F};
  IblEvaluate(0.8F, 0.8F, 0.8F, 1.0F, 0.9F, 0.05F, 1.0F, 1.0F, 1.0F, 2.0F,
              2.0F, 2.0F, 1.0F, 0.0F, out);
  // F0 = albedo = 0.8, scale 1, bias 0 -> 2.0 * 0.8 = 1.6.
  check(std::fabs(out[0] - 1.6F) < 1e-5F, "ibl: chrome spec = pref * F0");
  // Rough dielectric (metal 0): diffuse = albedo * irr / pi + spec.
  IblEvaluate(0.8F, 0.5F, 0.2F, 0.0F, 0.5F, 0.9F, 3.0F, 3.0F, 3.0F, 1.0F,
              1.0F, 1.0F, 0.5F, 0.1F, out);
  constexpr float kPi = 3.141592653589793F;
  // F0 = 0.04: spec = 0.04*0.5+0.1 = 0.12; diff_r = 0.8*3/pi.
  const float want_r = 0.8F * 3.0F / kPi + 0.12F;
  check(std::fabs(out[0] - want_r) < 1e-4F, "ibl: rough dielectric wiring");
  check(out[1] < out[0] && out[2] < out[1], "ibl: albedo tint preserved");
  // Rough sphere is dimmer in spec than chrome under same env.
  float chrome[3] = {0.0F, 0.0F, 0.0F};
  float rough[3] = {0.0F, 0.0F, 0.0F};
  IblEvaluate(0.8F, 0.8F, 0.8F, 1.0F, 0.9F, 0.05F, 1.0F, 1.0F, 1.0F, 2.0F,
              2.0F, 2.0F, 1.0F, 0.0F, chrome);
  IblEvaluate(0.8F, 0.8F, 0.8F, 0.0F, 0.5F, 0.9F, 1.0F, 1.0F, 1.0F, 2.0F,
              2.0F, 2.0F, 0.5F, 0.1F, rough);
  check(chrome[0] > rough[0], "ibl: chrome brighter spec than rough");
}

void test_cpu_irradiance_ref() {
  using engine::renderer::IblIrradianceReference;
  // Constant sky 2.0 -> irradiance = pi * 2.0 (mean * pi).
  float n[3] = {0.0F, 0.0F, 1.0F};
  auto flat = [](const float* /*dir*/, float* rgb) {
    rgb[0] = 2.0F;
    rgb[1] = 2.0F;
    rgb[2] = 2.0F;
  };
  float out[3] = {0.0F, 0.0F, 0.0F};
  IblIrradianceReference(n, 512, flat, out);
  constexpr float kPi = 3.141592653589793F;
  check(std::fabs(out[0] - 2.0F * kPi) < 0.05F, "ibl: flat sky irr = pi*L");
  // Gradient sky (brighter up): +Y normal sees more than -Y.
  auto grad = [](const float* dir, float* rgb) {
    rgb[0] = 1.0F + dir[1];
    rgb[1] = 1.0F + dir[1];
    rgb[2] = 1.0F + dir[1];
  };
  float up[3] = {0.0F, 0.0F, 0.0F};
  float down[3] = {0.0F, 0.0F, 0.0F};
  float ny[3] = {0.0F, 1.0F, 0.0F};
  float syd[3] = {0.0F, -1.0F, 0.0F};
  IblIrradianceReference(ny, 512, grad, up);
  IblIrradianceReference(syd, 512, grad, down);
  check(up[0] > down[0], "ibl: gradient sky up > down");
}

void test_cpu_dispatch_groups() {
  // Group-count contract for the 16x16x1 compute dispatches (local size 16):
  // ceil(edge/16) in x/y, 6 in z. Exact for the GPU-tested edge (32) and
  // covering for non-multiples: 8 -> 1 group, 40 -> 3 groups.
  // (CodeRabbit review on PR #229: irradiance.comp documents ceil division,
  // but the test hard-coded kIrr/16 — now ceil everywhere with this pin.)
  constexpr auto kLocal = 16u;
  constexpr auto groups = [](std::uint32_t edge) { return (edge + kLocal - 1u) / kLocal; };
  check(groups(32) == 2, "ibl: dispatch groups edge 32 -> 2");
  check(groups(8) == 1, "ibl: dispatch groups edge 8 -> 1");
  check(groups(40) == 3, "ibl: dispatch groups edge 40 -> 3");
}

void test_cpu() {
  test_cpu_validate();
  test_cpu_mip_mapping();
  test_cpu_basis();
  test_cpu_dispatch_groups();
  test_cpu_evaluate();
  test_cpu_irradiance_ref();
}

#if defined(ENGINE_RENDERER_HAS_VULKAN)

// Compute entry points beyond the shared GoldenVkProcs table.
// (vkCreateComputePipelines + vkCmdDispatch now live in GoldenVkProcs;
// this keeps the IBL-TU-only leftovers: destroy + bind procs.)
struct IblComputeVk {
  PFN_vkDestroyPipeline destroy_pipe{nullptr};
  PFN_vkCmdBindPipeline cmd_bind_pipe{nullptr};
  PFN_vkCmdBindDescriptorSets cmd_bind_sets{nullptr};
  PFN_vkCmdPushConstants cmd_push{nullptr};
};

template <typename Loader>
void load_ibl_compute_procs(Loader loader, IblComputeVk* p) {
  auto load = [&](const char* name) { return loader(name); };
  p->destroy_pipe =
      reinterpret_cast<PFN_vkDestroyPipeline>(load("vkDestroyPipeline"));
  p->cmd_bind_pipe =
      reinterpret_cast<PFN_vkCmdBindPipeline>(load("vkCmdBindPipeline"));
  p->cmd_bind_sets = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(
      load("vkCmdBindDescriptorSets"));
  p->cmd_push =
      reinterpret_cast<PFN_vkCmdPushConstants>(load("vkCmdPushConstants"));
}

auto ibl_gpu_missing(std::string_view what) -> int {
  failures += engine::test::GoldenGpuMissing(what);
  return 1;
}

// Device-local cubes via the shared GoldenMakeCube helper (import TU
// shares the same builder; per-mip storage views below are IBL-only).
auto make_mip_cube(engine::test::GoldenVk* g, std::uint32_t edge,
                   std::uint32_t mips, engine::test::GoldenImage* out)
    -> bool {
  return engine::test::GoldenMakeCube(
      g, edge, mips,
      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      out);
}

auto make_per_mip_view(engine::test::GoldenVk* g, VkImage img,
                       std::uint32_t mip, VkImageView* out) -> bool {
  VkImageViewCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vi.image = img;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  vi.format = VK_FORMAT_R32G32B32A32_SFLOAT;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, 1, 0, 6};
  return g->vk.create_view(g->device, &vi, nullptr, out) == VK_SUCCESS;
}

auto test_gpu() -> int {
  using engine::renderer::VulkanDevice;
  constexpr std::uint32_t kSrc = 64;   // source cube edge
  constexpr std::uint32_t kIrr = 32;   // irradiance edge
  constexpr std::uint32_t kPref = 32;  // prefilter base edge (6 mips)
  constexpr std::uint32_t kPrefMips = 6;  // 32->16->8->4->2->1
  constexpr std::uint32_t kW = 320;
  constexpr std::uint32_t kH = 240;

  VulkanDevice dev;
  engine::test::GoldenVk g{};
  const bool vk_ok = engine::test::GoldenOpenComputeDevice(
      check, ibl_gpu_missing, "P4-05-skybox-ibl", "ibl-gpu: no physical device",
      "ibl-gpu: device init ok", "ibl-gpu: entry points resolve",
      &dev, &g);
  if (!vk_ok) {
    return 1;
  }
  auto loader = [&](const char* name) -> void* {
    void* p = dev.proc(name);
    return (p != nullptr) ? p : dev.instance_proc(name);
  };
  IblComputeVk iv{};
  load_ibl_compute_procs(loader, &iv);
  const bool comp_ok =
      g.vk.create_compute != nullptr && iv.destroy_pipe != nullptr &&
      iv.cmd_bind_pipe != nullptr && g.vk.cmd_dispatch != nullptr &&
      iv.cmd_bind_sets != nullptr && iv.cmd_push != nullptr;
  check(comp_ok, "ibl-gpu: compute procs resolve");
  if (!vk_ok || !comp_ok) {
    return 1;
  }
  // Sonar S2259: the early return above proves all procedure pointers are
  // non-null on every path past this point; assert so the analyzer sees it.
  assert(vk_ok && comp_ok);
  assert(g.vk.create_compute != nullptr && iv.destroy_pipe != nullptr &&
         iv.cmd_bind_pipe != nullptr && iv.cmd_bind_sets != nullptr &&
         iv.cmd_push != nullptr && g.vk.cmd_dispatch != nullptr);

  check(kIrradianceSpv[0] == 0x07230203u, "ibl-gpu: irradiance spv magic");
  check(kPrefilterSpv[0] == 0x07230203u, "ibl-gpu: prefilter spv magic");
  check(kSkybox_vertSpv[0] == 0x07230203u, "ibl-gpu: skybox vert magic");
  check(kSkybox_fragSpv[0] == 0x07230203u, "ibl-gpu: skybox frag magic");
  check(kIbl_shade_vertSpv[0] == 0x07230203u, "ibl-gpu: ibl vert magic");
  check(kIbl_shade_fragSpv[0] == 0x07230203u, "ibl-gpu: ibl frag magic");

  // Upload the synthetic cube faces (one staging buffer, 6 layers).
  // Analytic HDR sky: bright zenith band + warm horizon (linear HDR,
  // red channel up to ~4). Sampled by BOTH the GPU upload and the CPU
  // reference below, so the comparison is exact by construction.
  std::vector<float> src_px;
  src_px.resize(static_cast<std::size_t>(kSrc) * kSrc * 4 * 6);
  for (std::uint32_t face = 0; face < 6; ++face) {
    for (std::uint32_t y = 0; y < kSrc; ++y) {
      for (std::uint32_t x = 0; x < kSrc; ++x) {
        float dir[3] = {0.0F, 0.0F, 0.0F};
        if (!engine::renderer::SkyboxFaceDir(face, x, y, kSrc, dir)) {
          continue;
        }
        const float up = dir[1] * 0.5F + 0.5F;
        const std::size_t i =
            (static_cast<std::size_t>(face) * kSrc * kSrc +
             static_cast<std::size_t>(y) * kSrc + x) *
            4;
        src_px[i + 0] = 0.5F + 3.5F * up * up;
        src_px[i + 1] = 0.4F + 1.2F * up;
        src_px[i + 2] = 0.3F + 0.6F * (1.0F - up);
        src_px[i + 3] = 1.0F;
      }
    }
  }

  engine::test::GoldenHostBuf up_b{};
  engine::test::GoldenHostBuf read_irr_b{};
  engine::test::GoldenHostBuf read_pref_b{};
  engine::test::GoldenHostBuf read_shade_b{};
  engine::test::GoldenImage src_img{};
  engine::test::GoldenImage irr_img{};
  engine::test::GoldenImage pref_img{};
  engine::test::GoldenImage lut_img{};
  engine::test::GoldenImage shade_img{};
  VkSampler cube_sampler = nullptr;
  VkSampler lut_sampler = nullptr;

  const std::uint64_t src_bytes =
      static_cast<std::uint64_t>(kSrc) * kSrc * 16 * 6;
  const std::uint64_t irr_bytes =
      static_cast<std::uint64_t>(kIrr) * kIrr * 16 * 6;
  // Prefilter readback: mip0 only (base edge, 6 layers).
  const std::uint64_t pref_bytes =
      static_cast<std::uint64_t>(kPref) * kPref * 16 * 6;
  const std::uint64_t lut_bytes = static_cast<std::uint64_t>(16) * 16 * 8;
  const std::uint64_t up_bytes = src_bytes + lut_bytes;
  const std::uint64_t shade_bytes =
      static_cast<std::uint64_t>(kW) * kH * 16;

  bool ok = engine::test::GoldenAllocHost(
                  &g, up_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &up_b) &&
            engine::test::GoldenAllocHost(
                &g, irr_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &read_irr_b) &&
            engine::test::GoldenAllocHost(
                &g, pref_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                &read_pref_b) &&
            engine::test::GoldenAllocHost(
                &g, shade_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                &read_shade_b);
  check(ok, "ibl-gpu: staging buffers");
  if (ok) {
    void* p = nullptr;
    ok = g.vk.map_mem(g.device, up_b.mem, 0, src_bytes, 0, &p) == VK_SUCCESS &&
         p != nullptr;
    if (ok) {
      auto* d = static_cast<float*>(p);
      for (std::size_t i = 0; i < src_px.size(); ++i) {
        d[i] = src_px[i];
      }
      g.vk.unmap_mem(g.device, up_b.mem);
    }
    check(ok, "ibl-gpu: source uploaded to staging");
  }
  if (ok) {
    ok = make_mip_cube(&g, kSrc, 1, &src_img);
    check(ok, "ibl-gpu: source cube image");
  }
  if (ok) {
    ok = make_mip_cube(&g, kIrr, 1, &irr_img);
    check(ok, "ibl-gpu: irradiance cube image");
  }
  if (ok) {
    ok = make_mip_cube(&g, kPref, kPrefMips, &pref_img);
    check(ok, "ibl-gpu: prefilter cube image");
  }
  // Whole-chain CUBE sample views for the shade set (compute writes use
  // per-mip 2D-array views; sampling the builder's mip0 view would hide
  // mips 1..5 from the LOD-selected prefilter fetch).
  VkImageView src_sample = nullptr;
  VkImageView irr_sample = nullptr;
  VkImageView pref_sample = nullptr;
  if (ok) {
    ok = engine::test::GoldenMakeCubeSampleView(&g, src_img.img, 1,
                                                &src_sample) &&
         engine::test::GoldenMakeCubeSampleView(&g, irr_img.img, 1,
                                                &irr_sample) &&
         engine::test::GoldenMakeCubeSampleView(&g, pref_img.img, kPrefMips,
                                                &pref_sample);
    check(ok, "ibl-gpu: cube sample views");
  }
  // BRDF LUT: small RG32F target for the shade pass (CPU LUT values
  // uploaded directly — LUT generation itself is proven in
  // engine_brdf_lut; here we only need a valid binding-3 texture).
  if (ok) {
    constexpr std::uint32_t kLut = 16;
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R32G32_SFLOAT;
    ii.extent = {kLut, kLut, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ok = g.vk.create_image(g.device, &ii, nullptr, &lut_img.img) == VK_SUCCESS;
    if (ok) {
      VkMemoryRequirements req{};
      g.vk.get_image_reqs(g.device, lut_img.img, &req);
      VkPhysicalDeviceMemoryProperties mp{};
      g.vk.get_mem_props(g.physical, &mp);
      std::uint32_t mi = 0xFFFFFFFFu;
      for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) != 0u) {
          mi = i;
          break;
        }
      }
      ok = mi != 0xFFFFFFFFu;
      if (ok) {
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = mi;
        ok = g.vk.alloc_mem(g.device, &ai, nullptr, &lut_img.mem) ==
             VK_SUCCESS;
      }
      if (ok) {
        ok = g.vk.bind_image_mem(g.device, lut_img.img, lut_img.mem, 0) ==
             VK_SUCCESS;
      }
      if (ok) {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = lut_img.img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R32G32_SFLOAT;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        ok = g.vk.create_view(g.device, &vi, nullptr, &lut_img.view) ==
             VK_SUCCESS;
      }
    }
    check(ok, "ibl-gpu: LUT image");
  }
  if (ok) {
    ok = engine::test::GoldenMakeImage(
        &g, VK_FORMAT_R32G32B32A32_SFLOAT, kW, kH,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        &shade_img);
    check(ok, "ibl-gpu: shade target");
  }
  if (ok) {
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.minLod = 0.0F;
    si.maxLod = 12.0F;
    ok = g.vk.create_sampler(g.device, &si, nullptr, &cube_sampler) ==
         VK_SUCCESS;
    if (ok) {
      VkSamplerCreateInfo li{};
      li.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
      li.magFilter = VK_FILTER_LINEAR;
      li.minFilter = VK_FILTER_LINEAR;
      li.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      li.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      li.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      ok = g.vk.create_sampler(g.device, &li, nullptr, &lut_sampler) ==
           VK_SUCCESS;
    }
    check(ok, "ibl-gpu: samplers");
  }

  // Compute set layout: binding 0 = cube sampler, 1 = storage image.
  VkDescriptorSetLayout comp_layout = nullptr;
  VkPipelineLayout irr_pipe_layout = nullptr;
  VkPipelineLayout pref_pipe_layout = nullptr;
  VkPipeline irr_pipe = nullptr;
  VkPipeline pref_pipe = nullptr;
  VkShaderModule irr_mod = nullptr;
  VkShaderModule pref_mod = nullptr;
  VkShaderModule ibl_vert_mod = nullptr;
  VkShaderModule ibl_frag_mod = nullptr;
  VkShaderModule sky_vert_mod = nullptr;
  VkShaderModule sky_frag_mod = nullptr;
  VkDescriptorPool comp_pool = nullptr;
  VkDescriptorSet irr_set = nullptr;
  // One prefilter set per mip: descriptor sets bound to a command buffer
  // in the recording state must not be updated, so each mip gets its own
  // set (written before recording) instead of rebinding one set mid-record.
  std::array<VkDescriptorSet, 6> pref_sets{};
  VkImageView pref_mip0_view = nullptr;
  VkImageView irr_mip0_view = nullptr;
  // Per-mip prefilter storage views (mip i binds pref_mip_views[i]).
  std::array<VkImageView, 6> pref_mip_views{};
  // Shade set layout: 0 = cube, 1 = irr, 2 = pref, 3 = LUT.
  VkDescriptorSetLayout shade_layout = nullptr;
  VkPipelineLayout shade_pipe_layout = nullptr;
  VkPipeline shade_pipe = nullptr;
  VkPipeline sky_pipe = nullptr;
  VkDescriptorPool shade_pool = nullptr;
  VkDescriptorSet shade_set = nullptr;

  if (ok) {
    ok = engine::test::GoldenMakeComputeSetLayout(&g, &comp_layout);
    check(ok, "ibl-gpu: compute set layout");
  }
  if (ok) {
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pc.size = 8;  // irradiance push: face_size + num_samples
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &comp_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    ok = g.vk.create_pipe_layout(g.device, &pli, nullptr, &irr_pipe_layout) ==
         VK_SUCCESS;
    check(ok, "ibl-gpu: irradiance pipeline layout");
  }
  if (ok) {
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pc.size = 16;  // prefilter push: mip_edge + index + count + samples
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &comp_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    ok =
        g.vk.create_pipe_layout(g.device, &pli, nullptr, &pref_pipe_layout) ==
        VK_SUCCESS;
    check(ok, "ibl-gpu: prefilter pipeline layout");
  }
  if (ok) {
    ok = engine::test::GoldenMakeModule(&g, kIrradianceSpv,
                                        kIrradianceSpvWords, &irr_mod) &&
         engine::test::GoldenMakeModule(&g, kPrefilterSpv,
                                        kPrefilterSpvWords, &pref_mod) &&
         engine::test::GoldenMakeModule(&g, kIbl_shade_vertSpv,
                                        kIbl_shade_vertSpvWords,
                                        &ibl_vert_mod) &&
         engine::test::GoldenMakeModule(&g, kIbl_shade_fragSpv,
                                        kIbl_shade_fragSpvWords,
                                        &ibl_frag_mod) &&
         engine::test::GoldenMakeModule(&g, kSkybox_vertSpv,
                                        kSkybox_vertSpvWords, &sky_vert_mod) &&
         engine::test::GoldenMakeModule(&g, kSkybox_fragSpv,
                                        kSkybox_fragSpvWords, &sky_frag_mod);
    check(ok, "ibl-gpu: shader modules");
  }
  if (ok) {
    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = irr_mod;
    cpi.stage.pName = "main";
    cpi.layout = irr_pipe_layout;
    VkComputePipelineCreateInfo cpi2 = cpi;
    cpi2.stage.module = pref_mod;
    cpi2.layout = pref_pipe_layout;
    ok = g.vk.create_compute(g.device, nullptr, 1, &cpi, nullptr, &irr_pipe) ==
             VK_SUCCESS &&
         g.vk.create_compute(g.device, nullptr, 1, &cpi2, nullptr, &pref_pipe) ==
             VK_SUCCESS;
    check(ok, "ibl-gpu: compute pipelines");
  }
  if (ok) {
    VkDescriptorPoolSize size{};
    size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    size.descriptorCount = 1 + 6;
    VkDescriptorPoolSize size2{};
    size2.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    size2.descriptorCount = 1 + 6;
    VkDescriptorPoolSize sizes[2] = {size, size2};
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 1 + 6;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    if (g.vk.create_pool(g.device, &pi, nullptr, &comp_pool) != VK_SUCCESS) {
      ok = false;
    } else {
      VkDescriptorSetAllocateInfo ai{};
      ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      ai.descriptorPool = comp_pool;
      ai.descriptorSetCount = 1;
      ai.pSetLayouts = &comp_layout;
      ok = g.vk.alloc_sets(g.device, &ai, &irr_set) == VK_SUCCESS;
      for (std::uint32_t m = 0; m < kPrefMips && ok; ++m) {
        ok = g.vk.alloc_sets(g.device, &ai, &pref_sets[m]) == VK_SUCCESS;
      }
    }
    check(ok, "ibl-gpu: compute descriptor sets");
  }
  if (ok) {
    ok = make_per_mip_view(&g, irr_img.img, 0, &irr_mip0_view);
    check(ok, "ibl-gpu: irradiance mip0 view");
  }
  if (ok) {
    // One storage view per prefilter mip; the compute pre-pass below
    // fills every mip the shade cube view exposes, so the LOD-selected
    // prefilter sample never reads an uninitialized level.
    for (std::uint32_t m = 0; m < kPrefMips; ++m) {
      ok = make_per_mip_view(&g, pref_img.img, m, &pref_mip_views[m]) && ok;
    }
    pref_mip0_view = pref_mip_views[0];
    check(ok, "ibl-gpu: prefilter per-mip views");
  }
  if (ok) {
    VkDescriptorImageInfo src_info{};
    src_info.sampler = cube_sampler;
    src_info.imageView = src_sample;
    src_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo irr_stor{};
    irr_stor.imageView = irr_mip0_view;
    irr_stor.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet w0{};
    w0.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w0.dstSet = irr_set;
    w0.dstBinding = 0;
    w0.descriptorCount = 1;
    w0.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w0.pImageInfo = &src_info;
    VkWriteDescriptorSet w1 = w0;
    w1.dstBinding = 1;
    w1.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w1.pImageInfo = &irr_stor;
    VkDescriptorImageInfo pref_stor{};
    pref_stor.imageView = pref_mip0_view;
    pref_stor.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet w2 = w0;
    w2.dstSet = pref_sets[0];
    VkWriteDescriptorSet w3 = w1;
    w3.dstSet = pref_sets[0];
    w3.pImageInfo = &pref_stor;
    VkWriteDescriptorSet writes[4] = {w0, w1, w2, w3};
    g.vk.update_sets(g.device, 4, writes, 0, nullptr);
    // Write the remaining prefilter mip sets now (same src sampler, own
    // mip storage view); no updates happen after recording begins.
    for (std::uint32_t m = 1; m < kPrefMips; ++m) {
      VkDescriptorImageInfo src_m{};
      src_m.sampler = cube_sampler;
      src_m.imageView = src_sample;
      src_m.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      VkDescriptorImageInfo stor_m{};
      stor_m.imageView = pref_mip_views[m];
      stor_m.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      VkWriteDescriptorSet wm0 = w0;
      wm0.dstSet = pref_sets[m];
      wm0.pImageInfo = &src_m;
      VkWriteDescriptorSet wm1 = w1;
      wm1.dstSet = pref_sets[m];
      wm1.pImageInfo = &stor_m;
      VkWriteDescriptorSet wm[2] = {wm0, wm1};
      g.vk.update_sets(g.device, 2, wm, 0, nullptr);
    }
    check(true, "ibl-gpu: compute descriptors written");
  }
  // Shade set layout + pipelines (fullscreen-triangle dynamic rendering).
  if (ok) {
    VkDescriptorSetLayoutBinding binds[4]{};
    binds[0].binding = 0;
    binds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binds[0].descriptorCount = 1;
    binds[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binds[1].binding = 1;
    binds[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binds[1].descriptorCount = 1;
    binds[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binds[2].binding = 2;
    binds[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binds[2].descriptorCount = 1;
    binds[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binds[3].binding = 3;
    binds[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binds[3].descriptorCount = 1;
    binds[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 4;
    li.pBindings = binds;
    ok = g.vk.create_set_layout(g.device, &li, nullptr, &shade_layout) ==
         VK_SUCCESS;
    check(ok, "ibl-gpu: shade set layout");
  }
  if (ok) {
    VkPushConstantRange pc{};
    pc.stageFlags =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.size = 40;  // inv-view 3x3 (9 floats)
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &shade_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    ok = g.vk.create_pipe_layout(g.device, &pli, nullptr,
                                 &shade_pipe_layout) == VK_SUCCESS;
    check(ok, "ibl-gpu: shade pipeline layout");
  }
  if (ok) {
    ok = engine::test::GoldenBuildPipe(g.vk.create_pipes, g.device,
                                       ibl_vert_mod, ibl_frag_mod,
                                       VK_FORMAT_R32G32B32A32_SFLOAT,
                                       shade_pipe_layout, &shade_pipe) &&
         engine::test::GoldenBuildPipe(g.vk.create_pipes, g.device,
                                       sky_vert_mod, sky_frag_mod,
                                       VK_FORMAT_R32G32B32A32_SFLOAT,
                                       shade_pipe_layout, &sky_pipe);
    check(ok, "ibl-gpu: shade + sky pipelines");
  }
  if (ok) {
    VkDescriptorPoolSize size{};
    size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    size.descriptorCount = 4;
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    if (g.vk.create_pool(g.device, &pi, nullptr, &shade_pool) != VK_SUCCESS) {
      ok = false;
    } else {
      VkDescriptorSetAllocateInfo ai{};
      ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      ai.descriptorPool = shade_pool;
      ai.descriptorSetCount = 1;
      ai.pSetLayouts = &shade_layout;
      ok = g.vk.alloc_sets(g.device, &ai, &shade_set) == VK_SUCCESS;
    }
    check(ok, "ibl-gpu: shade descriptor set");
  }
  if (ok) {
    VkDescriptorImageInfo i0{};
    i0.sampler = cube_sampler;
    i0.imageView = src_sample;
    i0.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo i1 = i0;
    i1.imageView = irr_sample;
    VkDescriptorImageInfo i2 = i0;
    i2.imageView = pref_sample;
    VkDescriptorImageInfo i3{};
    i3.sampler = lut_sampler;
    i3.imageView = lut_img.view;
    i3.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet writes[4]{};
    const VkDescriptorImageInfo* infos[4] = {&i0, &i1, &i2, &i3};
    for (int k = 0; k < 4; ++k) {
      writes[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[k].dstSet = shade_set;
      writes[k].dstBinding = static_cast<std::uint32_t>(k);
      writes[k].descriptorCount = 1;
      writes[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[k].pImageInfo = infos[k];
    }
    g.vk.update_sets(g.device, 4, writes, 0, nullptr);
    check(true, "ibl-gpu: shade descriptors written");
  }

  // Record: upload src + LUT, dispatch irradiance + prefilter mip0,
  // render shade, copy all readbacks.
  if (ok) {
    VkCommandPool cmd_pool = nullptr;
    VkCommandBuffer cb = nullptr;
    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.queueFamilyIndex = g.queue_family;
    ok = g.vk.create_cmd_pool(g.device, &cpi, nullptr, &cmd_pool) == VK_SUCCESS;
    if (ok) {
      ok = engine::test::GoldenBeginCmd(&g, cmd_pool, &cb);
    }
    auto barrier_img = [&](VkImage img, VkImageLayout old_l,
                           VkImageLayout new_l, VkPipelineStageFlags2 src_s,
                           VkAccessFlags2 src_a, VkPipelineStageFlags2 dst_s,
                           VkAccessFlags2 dst_a, std::uint32_t layers,
                           std::uint32_t mips) {
      VkImageMemoryBarrier2 b{};
      b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
      b.srcStageMask = src_s;
      b.srcAccessMask = src_a;
      b.dstStageMask = dst_s;
      b.dstAccessMask = dst_a;
      b.oldLayout = old_l;
      b.newLayout = new_l;
      b.image = img;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, layers};
      VkDependencyInfo d{};
      d.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
      d.imageMemoryBarrierCount = 1;
      d.pImageMemoryBarriers = &b;
      g.vk.cmd_barrier2(cb, &d);
    };
    if (ok) {
      // Upload source cube (6 layers).
      barrier_img(src_img.img, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  6, 1);
      for (std::uint32_t layer = 0; layer < 6; ++layer) {
        VkBufferImageCopy rc{};
        rc.bufferOffset = static_cast<VkDeviceSize>(layer) *
                          static_cast<VkDeviceSize>(kSrc) * kSrc * 16;
        rc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
        rc.imageExtent = {kSrc, kSrc, 1};
        g.vk.cmd_copy_buf_img(cb, up_b.buf, src_img.img,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rc);
      }
      // Upload LUT: CPU BrdfLutReference at 16px (RG) into the tail of
      // the staging buffer (RG32F: 2 floats per texel).
      {
        barrier_img(lut_img.img, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0,
                    VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, 1, 1);
        void* lp = nullptr;
        if (g.vk.map_mem(g.device, up_b.mem, src_bytes, lut_bytes, 0, &lp) ==
                VK_SUCCESS &&
            lp != nullptr) {
          auto* ld = static_cast<float*>(lp);
          for (std::uint32_t yy = 0; yy < 16; ++yy) {
            for (std::uint32_t xx = 0; xx < 16; ++xx) {
              float s = 0.0F;
              float b = 0.0F;
              engine::renderer::BrdfLutReference(xx, yy, 16, &s, &b);
              ld[(static_cast<std::size_t>(yy) * 16 + xx) * 2 + 0] = s;
              ld[(static_cast<std::size_t>(yy) * 16 + xx) * 2 + 1] = b;
            }
          }
          g.vk.unmap_mem(g.device, up_b.mem);
        } else {
          ok = false;
        }
        VkBufferImageCopy lc{};
        lc.bufferOffset = static_cast<VkDeviceSize>(src_bytes);
        lc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        lc.imageExtent = {16, 16, 1};
        g.vk.cmd_copy_buf_img(cb, up_b.buf, lut_img.img,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &lc);
        barrier_img(lut_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1, 1);
      }
      barrier_img(src_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 6, 1);
      barrier_img(irr_img.img, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_GENERAL, 0, 0,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, 6, 1);
      // Irradiance dispatch uses ceiling division (matches irradiance.comp
      // header): exact for kIrr=32 and covers non-multiple edges too.
      const std::uint32_t irr_groups = (kIrr + 15u) / 16u;
      iv.cmd_bind_pipe(cb, VK_PIPELINE_BIND_POINT_COMPUTE, irr_pipe);
      iv.cmd_bind_sets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, irr_pipe_layout,
                       0, 1, &irr_set, 0, nullptr);
      {
        std::array<std::uint32_t, 2> push = {kIrr, 512};
        iv.cmd_push(cb, irr_pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8,
                    push.data());
      }
      g.vk.cmd_dispatch(cb, irr_groups, irr_groups, 6);
      barrier_img(pref_img.img, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_GENERAL, 0, 0,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, 6, kPrefMips);
      // Prefilter pre-pass: fill every mip the shade cube view exposes
      // (mip i: edge kPref>>i, roughness i/(count-1)). One storage view
      // per mip is bound before its dispatch; the group count uses
      // ceiling division so tail mips (< 16 texels) still get coverage.
      // mip0 (rough 0) is the mirror lobe checked against the source.
      iv.cmd_bind_pipe(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pref_pipe);
      for (std::uint32_t m = 0; m < kPrefMips; ++m) {
        const std::uint32_t edge = kPref >> m;
        iv.cmd_bind_sets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pref_pipe_layout,
                         0, 1, &pref_sets[m], 0, nullptr);
        {
          std::array<std::uint32_t, 4> push = {edge, m, kPrefMips, 1024};
          iv.cmd_push(cb, pref_pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                      16, push.data());
        }
        const std::uint32_t groups = (edge + 15u) / 16u;
        g.vk.cmd_dispatch(cb, groups, groups, 6);
        // Order the mip writes before the next mip (same image, GENERAL).
        VkMemoryBarrier2 mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                           VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        VkDependencyInfo md{};
        md.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        md.memoryBarrierCount = 1;
        md.pMemoryBarriers = &mb;
        g.vk.cmd_barrier2(cb, &md);
      }
      // Read back irradiance + prefilter mip0.
      barrier_img(irr_img.img, VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                  6, 1);
      for (std::uint32_t layer = 0; layer < 6; ++layer) {
        VkBufferImageCopy rl{};
        rl.bufferOffset =
            static_cast<VkDeviceSize>(layer) * kIrr * kIrr * 16;
        rl.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
        rl.imageExtent = {kIrr, kIrr, 1};
        g.vk.cmd_copy_img_buf(cb, irr_img.img,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              read_irr_b.buf, 1, &rl);
      }
      barrier_img(pref_img.img, VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                  6, kPrefMips);
      for (std::uint32_t layer = 0; layer < 6; ++layer) {
        VkBufferImageCopy rl{};
        rl.bufferOffset =
            static_cast<VkDeviceSize>(layer) * kPref * kPref * 16;
        rl.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
        rl.imageExtent = {kPref, kPref, 1};
        g.vk.cmd_copy_img_buf(cb, pref_img.img,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              read_pref_b.buf, 1, &rl);
      }
      // Shade pass: IBL split into shade_img (RGBA32F offscreen).
      barrier_img(irr_img.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 6, 1);
      barrier_img(pref_img.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 6, kPrefMips);
      barrier_img(shade_img.img, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 0,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, 1, 1);
      VkRenderingAttachmentInfo color_att{};
      color_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      color_att.imageView = shade_img.view;
      color_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      color_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      color_att.clearValue.color.float32[0] = 0.0F;
      color_att.clearValue.color.float32[1] = 0.0F;
      color_att.clearValue.color.float32[2] = 0.0F;
      color_att.clearValue.color.float32[3] = 1.0F;
      VkRenderingInfo ri{};
      ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
      ri.renderArea = {{0, 0}, {kW, kH}};
      ri.layerCount = 1;
      ri.colorAttachmentCount = 1;
      ri.pColorAttachments = &color_att;
      g.vk.cmd_begin_render(cb, &ri);
      {
        // Negative-height viewport: framebuffer row 0 is +Y-up NDC row 0
        // (no CPU row flip on readback); px(x,y) indexes row y directly.
        VkViewport vp{};
        vp.y = static_cast<float>(kH);
        vp.width = static_cast<float>(kW);
        vp.height = -static_cast<float>(kH);
        vp.maxDepth = 1.0F;
        g.vk.cmd_viewport(cb, 0, 1, &vp);
      }
      VkRect2D sc{};
      sc.extent = {kW, kH};
      g.vk.cmd_scissor(cb, 0, 1, &sc);
      g.vk.cmd_bind_pipe(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, shade_pipe);
      g.vk.cmd_bind_sets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                         shade_pipe_layout, 0, 1, &shade_set, 0, nullptr);
      {
        // Identity inverse view rotation (camera looks down -Z, +Y up).
        // 10 floats: the shade push block is 9 floats (36B padded to 40B
        // by scalar-block layout rules); the 10th pads the call to the
        // full 40-byte range so cmd_push never reads past the array.
        float inv_view[10] = {1.0F, 0.0F, 0.0F, 0.0F, 1.0F,
                              0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
        g.vk.cmd_push(cb, shade_pipe_layout,
                      VK_SHADER_STAGE_VERTEX_BIT |
                          VK_SHADER_STAGE_FRAGMENT_BIT,
                      0, 40, inv_view);
      }
      g.vk.cmd_draw(cb, 3, 1, 0, 0);
      g.vk.cmd_end_render(cb);
      barrier_img(shade_img.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                  1, 1);
      {
        VkBufferImageCopy rl{};
        rl.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rl.imageExtent = {kW, kH, 1};
        g.vk.cmd_copy_img_buf(cb, shade_img.img,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              read_shade_b.buf, 1, &rl);
      }
      ok = engine::test::GoldenEndReadback(&g, cb);
    }
    check(ok, "ibl-gpu: frame recorded");
    if (ok) {
      ok = engine::test::GoldenSubmitWait(&g, cb);
      check(ok, "ibl-gpu: submit + fence wait");
    }
    if (cmd_pool != nullptr) {
      g.vk.destroy_cmd_pool(g.device, cmd_pool, nullptr);
    }
  }

  // Readback A: irradiance vs CPU cosine-weighted reference.
  if (ok) {
    void* p = nullptr;
    ok = g.vk.map_mem(g.device, read_irr_b.mem, 0, irr_bytes, 0, &p) ==
             VK_SUCCESS &&
         p != nullptr;
    check(ok, "ibl-gpu: irradiance readback mapped");
    if (ok) {
      const auto* f = static_cast<const float*>(p);
      std::uint32_t bad = 0;
      float worst = 0.0F;
      for (std::uint32_t face = 0; face < 6; ++face) {
        for (std::uint32_t y = 0; y < kIrr; ++y) {
          for (std::uint32_t x = 0; x < kIrr; ++x) {
            float n[3] = {0.0F, 0.0F, 0.0F};
            if (!engine::renderer::SkyboxFaceDir(face, x, y, kIrr, n)) {
              ++bad;
              continue;
            }
            auto sky = [&](const float* dir, float* rgb) {
              const float up = dir[1] * 0.5F + 0.5F;
              rgb[0] = 0.5F + 3.5F * up * up;
              rgb[1] = 0.4F + 1.2F * up;
              rgb[2] = 0.3F + 0.6F * (1.0F - up);
            };
            float want[3] = {0.0F, 0.0F, 0.0F};
            engine::renderer::IblIrradianceReference(n, 512, sky, want);
            const std::size_t i =
                (static_cast<std::size_t>(face) * kIrr * kIrr +
                 static_cast<std::size_t>(y) * kIrr + x) *
                4;
            for (int c = 0; c < 3; ++c) {
              const float got = f[i + static_cast<std::size_t>(c)];
              const float dd = std::fabs(got - want[c]);
              if (dd > worst) {
                worst = dd;
              }
              // Stratified-vs-Hammersley pattern difference + bilinear:
              // tolerance 0.15 absolute on HDR values up to ~4.
              if (dd > 0.15F || !std::isfinite(got)) {
                ++bad;
              }
            }
          }
        }
      }
      std::printf("[INFO] ibl-gpu: irradiance worst %.6f bad=%u\n",
                  static_cast<double>(worst), bad);
      check(bad == 0, "ibl-gpu: irradiance matches CPU ref");
      g.vk.unmap_mem(g.device, read_irr_b.mem);
    }
  }
  // Readback B: prefilter mip0 (rough 0) matches the source texel.
  if (ok) {
    void* p = nullptr;
    ok = g.vk.map_mem(g.device, read_pref_b.mem, 0, pref_bytes, 0, &p) ==
             VK_SUCCESS &&
         p != nullptr;
    check(ok, "ibl-gpu: prefilter readback mapped");
    if (ok) {
      const auto* f = static_cast<const float*>(p);
      std::uint32_t bad = 0;
      float worst = 0.0F;
      // mip0 edge == kPref, but the source is kSrc: compare against the
      // analytic sky at the mip0 texel direction (rough 0 = point lobe).
      for (std::uint32_t face = 0; face < 6; ++face) {
        for (std::uint32_t y = 0; y < kPref; ++y) {
          for (std::uint32_t x = 0; x < kPref; ++x) {
            float dir[3] = {0.0F, 0.0F, 0.0F};
            if (!engine::renderer::SkyboxFaceDir(face, x, y, kPref, dir)) {
              ++bad;
              continue;
            }
            const float up = dir[1] * 0.5F + 0.5F;
            const float want[3] = {0.5F + 3.5F * up * up, 0.4F + 1.2F * up,
                                   0.3F + 0.6F * (1.0F - up)};
            const std::size_t i =
                (static_cast<std::size_t>(face) * kPref * kPref +
                 static_cast<std::size_t>(y) * kPref + x) *
                4;
            for (int c = 0; c < 3; ++c) {
              const float got = f[i + static_cast<std::size_t>(c)];
              const float dd = std::fabs(got - want[c]);
              if (dd > worst) {
                worst = dd;
              }
              // GGX rough-0 lobe is narrow but nonzero: tolerance 0.3.
              if (dd > 0.3F || !std::isfinite(got)) {
                ++bad;
              }
            }
          }
        }
      }
      std::printf("[INFO] ibl-gpu: prefilter-mip0 worst %.6f bad=%u\n",
                  static_cast<double>(worst), bad);
      check(bad == 0, "ibl-gpu: prefilter mip0 matches source");
      g.vk.unmap_mem(g.device, read_pref_b.mem);
    }
  }
  // Readback C: shade probes — chrome vs rough split + sky backdrop.
  if (ok) {
    void* p = nullptr;
    ok = g.vk.map_mem(g.device, read_shade_b.mem, 0, shade_bytes, 0, &p) ==
             VK_SUCCESS &&
         p != nullptr;
    check(ok, "ibl-gpu: shade readback mapped");
    if (ok) {
      const auto* f = static_cast<const float*>(p);
      auto px = [&](std::uint32_t x, std::uint32_t y, int c) -> float {
        // Negative-height viewport: framebuffer row y == NDC row y.
        return f[(static_cast<std::size_t>(y) * kW + x) * 4 +
                 static_cast<std::size_t>(c)];
      };
      // Disc centers: chrome (0.25, 0.5), rough (0.75, 0.5).
      const std::uint32_t cx_l = kW / 4;
      const std::uint32_t cx_r = (3 * kW) / 4;
      const std::uint32_t cy = kH / 2;
      const float chrome_r = px(cx_l, cy, 0);
      const float rough_r = px(cx_r, cy, 0);
      std::printf("[INFO] ibl-gpu: chrome=(%.3f,%.3f,%.3f) rough=(%.3f,%.3f,%.3f)\n",
                  static_cast<double>(chrome_r),
                  static_cast<double>(px(cx_l, cy, 1)),
                  static_cast<double>(px(cx_l, cy, 2)),
                  static_cast<double>(rough_r),
                  static_cast<double>(px(cx_r, cy, 1)),
                  static_cast<double>(px(cx_r, cy, 2)));
      // Chrome mirror lobe under an HDR sky (peak ~4) is much brighter
      // than the diffuse-dominated rough sphere.
      check(chrome_r > rough_r, "ibl-gpu: chrome brighter than rough");
      check(chrome_r > 1.0F, "ibl-gpu: chrome HDR > 1 (sharp reflection)");
      check(std::isfinite(chrome_r) && std::isfinite(rough_r),
            "ibl-gpu: shade finite");
      // Sky backdrop: top-center pixel looks at +Y-ish sky (bright band).
      const float sky_r = px(kW / 2, 4, 0);
      check(sky_r > 0.5F && std::isfinite(sky_r),
            "ibl-gpu: skybox visible (bright zenith)");
      // Golden p4_05_ibl_chrome_rough_320x240: HDR shade -> LDR via the
      // shared CPU tonemap (HdrTonemapCpu mirrors post_composite.frag
      // term for term, no extra post pipeline needed) at EV 0, then
      // golden_compare.hpp P1-05. Negative-height viewport renders +Y
      // up: PNG row 0 is the top framebuffer row (no row flip).
      {
        constexpr float kExposure = 1.0F;
        std::vector<std::uint8_t> ldr(static_cast<std::size_t>(kW) * kH * 4,
                                      0);
        bool tone_ok = true;
        for (std::uint32_t y = 0; y < kH; ++y) {
          for (std::uint32_t x = 0; x < kW; ++x) {
            const float hdr[3] = {px(x, y, 0), px(x, y, 1), px(x, y, 2)};
            float ldr_f[3] = {0.0F, 0.0F, 0.0F};
            if (!engine::renderer::HdrTonemapCpu(hdr, kExposure, ldr_f)) {
              tone_ok = false;
              break;
            }
            const std::size_t o =
                (static_cast<std::size_t>(y) * kW + x) * 4;
            for (int c = 0; c < 3; ++c) {
              const float v = ldr_f[c] < 0.0F   ? 0.0F
                              : ldr_f[c] > 1.0F ? 1.0F
                                                : ldr_f[c];
              ldr[o + static_cast<std::size_t>(c)] =
                  static_cast<std::uint8_t>(v * 255.0F + 0.5F);
            }
            ldr[o + 3] = 255;
          }
        }
        check(tone_ok, "ibl-golden: cpu tonemap ok");
        if (tone_ok) {
          failures += engine::test::golden_check(
              "p4_05_ibl_chrome_rough_320x240",
              std::span<const std::uint8_t>(ldr.data(), ldr.size()), kW, kH);
          // LDR acceptance on the same bytes the golden compares:
          // chrome brighter than rough (red channel), left/right halves
          // differ, nothing fully clips to white.
          const std::size_t lc = ((120 * kW) + 80) * 4;
          const std::size_t rc = ((120 * kW) + 240) * 4;
          check(ldr[lc] > ldr[rc], "ibl-golden: chrome brighter (ldr)");
          check(ldr[lc] > 40, "ibl-golden: chrome lit (ldr)");
          check(ldr[lc] < 255 && ldr[rc] < 255,
                "ibl-golden: no white clip (ldr)");
        }
      }
      g.vk.unmap_mem(g.device, read_shade_b.mem);
    }
  }

  if (sky_pipe != nullptr) {
    g.vk.destroy_pipe(g.device, sky_pipe, nullptr);
  }
  if (shade_pipe != nullptr) {
    g.vk.destroy_pipe(g.device, shade_pipe, nullptr);
  }
  if (irr_pipe != nullptr) {
    iv.destroy_pipe(g.device, irr_pipe, nullptr);
  }
  if (pref_pipe != nullptr) {
    iv.destroy_pipe(g.device, pref_pipe, nullptr);
  }
  if (sky_vert_mod != nullptr) {
    g.vk.destroy_module(g.device, sky_vert_mod, nullptr);
  }
  if (sky_frag_mod != nullptr) {
    g.vk.destroy_module(g.device, sky_frag_mod, nullptr);
  }
  if (ibl_vert_mod != nullptr) {
    g.vk.destroy_module(g.device, ibl_vert_mod, nullptr);
  }
  if (ibl_frag_mod != nullptr) {
    g.vk.destroy_module(g.device, ibl_frag_mod, nullptr);
  }
  if (irr_mod != nullptr) {
    g.vk.destroy_module(g.device, irr_mod, nullptr);
  }
  if (pref_mod != nullptr) {
    g.vk.destroy_module(g.device, pref_mod, nullptr);
  }
  if (comp_layout != nullptr) {
    g.vk.destroy_set_layout(g.device, comp_layout, nullptr);
  }
  if (irr_pipe_layout != nullptr) {
    g.vk.destroy_pipe_layout(g.device, irr_pipe_layout, nullptr);
  }
  if (pref_pipe_layout != nullptr) {
    g.vk.destroy_pipe_layout(g.device, pref_pipe_layout, nullptr);
  }
  if (shade_pipe_layout != nullptr) {
    g.vk.destroy_pipe_layout(g.device, shade_pipe_layout, nullptr);
  }
  if (shade_layout != nullptr) {
    g.vk.destroy_set_layout(g.device, shade_layout, nullptr);
  }
  if (comp_pool != nullptr) {
    g.vk.destroy_pool(g.device, comp_pool, nullptr);
  }
  if (shade_pool != nullptr) {
    g.vk.destroy_pool(g.device, shade_pool, nullptr);
  }
  if (pref_mip0_view != nullptr) {
    // Owned by pref_mip_views[0]; destroyed with the array below.
    pref_mip0_view = nullptr;
  }
  for (auto v : pref_mip_views) {
    if (v != nullptr) {
      g.vk.destroy_view(g.device, v, nullptr);
    }
  }
  if (irr_mip0_view != nullptr) {
    g.vk.destroy_view(g.device, irr_mip0_view, nullptr);
  }
  for (auto v : {src_sample, irr_sample, pref_sample}) {
    if (v != nullptr) {
      g.vk.destroy_view(g.device, v, nullptr);
    }
  }
  if (cube_sampler != nullptr) {
    g.vk.destroy_sampler(g.device, cube_sampler, nullptr);
  }
  if (lut_sampler != nullptr) {
    g.vk.destroy_sampler(g.device, lut_sampler, nullptr);
  }
  engine::test::GoldenFreeImage(&g, &src_img);
  engine::test::GoldenFreeImage(&g, &irr_img);
  engine::test::GoldenFreeImage(&g, &pref_img);
  engine::test::GoldenFreeImage(&g, &lut_img);
  engine::test::GoldenFreeImage(&g, &shade_img);
  engine::test::GoldenFreeHost(&g, &up_b);
  engine::test::GoldenFreeHost(&g, &read_irr_b);
  engine::test::GoldenFreeHost(&g, &read_pref_b);
  engine::test::GoldenFreeHost(&g, &read_shade_b);
  dev.shutdown();
  return 0;
}

#endif

}  // namespace

auto main() -> int {
  test_cpu();
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  test_gpu();
#else
  std::printf("[SKIP] ibl-gpu: SDK absent (stub build)\n");
#endif
  std::printf(failures == 0 ? "test_skybox_ibl: all checks passed\n"
                            : "test_skybox_ibl: %d FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}

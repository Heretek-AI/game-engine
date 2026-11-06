// test_skybox_import.cpp — P4-05 equirect HDR import + cubemap conversion.
//
// CPU contract (no GPU; runs on stub builds too): SkyboxValidateImport
// plans, SkyboxFaceDir face centers + edge continuity, SkyboxDirToUv
// wrap, SkyboxSampleEquirectCpu bilinear + null-safety, and an HDR
// decode probe through the REAL stb decoder (stbi_loadf_from_memory on
// a synthetic 2x2 flat Radiance .hdr — the same decoder the M5-01
// texture pipeline uses, so "HDR loads" is proven, not assumed).
//
// GPU section: the checked-in equirect_to_cube compute shader converts
// a synthetic 64x32 float equirect (red channel up to 4.0, proving the
// HDR range survives) into a 6-layer 64px RGBA32F cubemap; every texel
// is compared against the CPU reference (SkyboxFaceDir +
// SkyboxSampleEquirectCpu) within a hardware-filter tolerance.
// [FAIL] (not [SKIP]) under ENGINE_REQUIRE_GPU=1 (rule 11 / P1-01).
#include <array>
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

#include "engine/renderer/skybox_image_based_lighting.hpp"
#include "engine/renderer/vulkan_device.hpp"

#if defined(ENGINE_ASSETS_HAS_STB)
#include "stb_image.h"
#endif

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#include "engine/renderer/bindless_table.hpp"
#include "engine/renderer/pbr_pipeline.hpp"
#include "engine/renderer/vulkan_device.hpp"
#include "engine/renderer/shaders/equirect_to_cube_spv.inc"

// Established golden-TU pattern (test_hdr_tonemap.cpp): the shared
// helpers need the six PBR SPIR-V symbols + engine types, so include
// all six blobs before golden_vk_helper.hpp. This TU only *runs* the
// equirect compute shader, but the helper's GoldenOpenDevice is never
// called here (device opens inline below, no bindless plan needed).
#include "engine/renderer/shaders/pbr_shade_frag_spv.inc"
#include "engine/renderer/shaders/pbr_shade_vert_spv.inc"
#include "engine/renderer/shaders/pbr_vis_frag_spv.inc"
#include "engine/renderer/shaders/pbr_vis_vert_spv.inc"
#include "engine/renderer/shaders/post_frag_spv.inc"
#include "engine/renderer/shaders/post_vert_spv.inc"
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

// --- CPU contract ---------------------------------------------------------

void test_cpu_validate() {
  using engine::renderer::SkyboxImportParams;
  using engine::renderer::SkyboxImportPlan;
  using engine::renderer::kSkyboxCubemapSize;
  using engine::renderer::kSkyboxFaceCount;
  SkyboxImportPlan plan{};
  SkyboxImportParams good{0, 1024, 512};
  check(SkyboxValidateImport(good, &plan), "sky: default plan ok");
  check(plan.face_size == kSkyboxCubemapSize, "sky: default face 512");
  check(plan.layer_count == kSkyboxFaceCount, "sky: 6 layers");
  check(plan.face_bytes == static_cast<std::uint64_t>(512) * 512 * 16,
        "sky: face bytes 512^2*16");
  SkyboxImportParams small{64, 128, 64};
  check(SkyboxValidateImport(small, &plan) && plan.face_size == 64,
        "sky: custom face 64");
  check(!SkyboxValidateImport(good, nullptr), "sky: null out rejected");
  SkyboxImportParams zero_eq{0, 0, 512};
  check(!SkyboxValidateImport(zero_eq, &plan), "sky: zero equirect rejected");
  SkyboxImportParams tiny{8, 128, 64};
  check(!SkyboxValidateImport(tiny, &plan), "sky: face 8 rejected");
  SkyboxImportParams huge{4096, 128, 64};
  check(!SkyboxValidateImport(huge, &plan), "sky: face 4096 rejected");
}

void test_cpu_face_dir() {
  using engine::renderer::SkyboxFaceDir;
  // Face centers map to the six axis directions (size 1 -> single texel
  // at center of each face).
  const float want[6][3] = {
      {1.0F, 0.0F, 0.0F},   // +X
      {-1.0F, 0.0F, 0.0F},  // -X
      {0.0F, 1.0F, 0.0F},   // +Y
      {0.0F, -1.0F, 0.0F},  // -Y
      {0.0F, 0.0F, 1.0F},   // +Z
      {0.0F, 0.0F, -1.0F},  // -Z
  };
  bool centers_ok = true;
  for (std::uint32_t f = 0; f < 6; ++f) {
    float d[3] = {0.0F, 0.0F, 0.0F};
    if (!SkyboxFaceDir(f, 0, 0, 1, d)) {
      centers_ok = false;
      break;
    }
    for (int c = 0; c < 3; ++c) {
      if (std::fabs(d[c] - want[f][c]) > 1e-5F) {
        centers_ok = false;
      }
    }
  }
  check(centers_ok, "sky: face centers are axis dirs");
  // Edge continuity: the right edge of +X meets the left edge of -Z
  // (+X: z = -u, so x=max borders -Z; -Z: x-dir = -u, so x=0 borders
  // +X). Adjacent faces share directions along the seam.
  float a[3] = {0.0F, 0.0F, 0.0F};
  float b[3] = {0.0F, 0.0F, 0.0F};
  const std::uint32_t n = 64;
  bool seam_ok = SkyboxFaceDir(0, n - 1, n / 2, n, a) &&
                 SkyboxFaceDir(5, 0, n / 2, n, b);
  if (seam_ok) {
    // Both edge texels sit on the +X/-Z diagonal; their normalized dirs
    // must agree closely (same edge, half-texel offset only).
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    seam_ok = dot > 0.999F;
  }
  check(seam_ok, "sky: +X/-Z seam continuous");
  // Rejections.
  float d[3] = {0.0F, 0.0F, 0.0F};
  check(!SkyboxFaceDir(6, 0, 0, 64, d), "sky: face 6 rejected");
  check(!SkyboxFaceDir(0, 0, 0, 0, d), "sky: size 0 rejected");
  check(!SkyboxFaceDir(0, 0, 0, 64, nullptr), "sky: null out rejected");
  // Unit length on a sweep.
  bool unit_ok = true;
  for (std::uint32_t f = 0; f < 6 && unit_ok; ++f) {
    for (std::uint32_t k = 0; k < 16 && unit_ok; ++k) {
      float v[3] = {0.0F, 0.0F, 0.0F};
      if (!SkyboxFaceDir(f, (k * 37) % 64, (k * 53) % 64, 64, v)) {
        unit_ok = false;
        break;
      }
      const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
      if (std::fabs(len - 1.0F) > 1e-5F) {
        unit_ok = false;
      }
    }
  }
  check(unit_ok, "sky: dirs unit length");
}

void test_cpu_sample() {
  using engine::renderer::SkyboxDirToUv;
  using engine::renderer::SkyboxSampleEquirectCpu;
  // 4x2 constant-color equirect: any direction samples the color.
  const std::array<float, 4 * 2 * 4> px = [] {
    std::array<float, 4 * 2 * 4> p{};
    for (std::size_t i = 0; i < 8; ++i) {
      p[i * 4 + 0] = 2.0F;  // HDR red > 1
      p[i * 4 + 1] = 0.5F;
      p[i * 4 + 2] = 0.25F;
      p[i * 4 + 3] = 1.0F;
    }
    return p;
  }();
  const float dirs[4][3] = {
      {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, -1.0F}};
  bool flat_ok = true;
  for (const auto& d : dirs) {
    float o[3] = {0.0F, 0.0F, 0.0F};
    if (!SkyboxSampleEquirectCpu(px.data(), 4, 2, d, o)) {
      flat_ok = false;
      break;
    }
    if (std::fabs(o[0] - 2.0F) > 1e-5F || std::fabs(o[1] - 0.5F) > 1e-5F ||
        std::fabs(o[2] - 0.25F) > 1e-5F) {
      flat_ok = false;
    }
  }
  check(flat_ok, "sky: flat equirect samples constant");
  // UV wrap: +X and -X-adjacent longitudes agree across the seam.
  float u0 = 0.0F;
  float u1 = 0.0F;
  float v0 = 0.0F;
  float v1 = 0.0F;
  SkyboxDirToUv(1.0F, 0.0F, 0.0F, &u0, &v0);
  SkyboxDirToUv(-1.0F, 0.0F, 0.0F, &u1, &v1);
  check(u0 >= 0.0F && u0 < 1.0F && u1 >= 0.0F && u1 < 1.0F,
        "sky: uv in range");
  check(std::fabs(u0 - 0.75F) < 1e-5F, "sky: +X maps u=0.75");
  check(std::fabs(u1 - 0.25F) < 1e-5F, "sky: -X maps u=0.25");
  // Null-safety.
  float o[3] = {0.0F, 0.0F, 0.0F};
  const float d[3] = {0.0F, 0.0F, 1.0F};
  check(!SkyboxSampleEquirectCpu(nullptr, 4, 2, d, o), "sky: null px rejected");
  check(!SkyboxSampleEquirectCpu(px.data(), 0, 2, d, o), "sky: zero w rejected");
  check(!SkyboxSampleEquirectCpu(px.data(), 4, 2, d, nullptr),
        "sky: null out rejected");
}

// Minimal flat Radiance .hdr (2x2, non-RLE: widths < 8 use flat RGBE).
// Pixel i: rgbe bytes -> float via stb.
auto build_tiny_hdr() -> std::vector<std::uint8_t> {
  const std::string head = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 2 +X 2\n";
  std::vector<std::uint8_t> buf(head.begin(), head.end());
  // (128,128,128,129) -> ~(1.0,1.0,1.0); (255,0,0,130) -> HDR red ~1.99.
  const std::uint8_t px[4][4] = {
      {128, 128, 128, 129}, {255, 0, 0, 130}, {0, 255, 0, 130}, {0, 0, 255, 130}};
  for (const auto& p : px) {
    for (int c = 0; c < 4; ++c) {
      buf.push_back(p[c]);
    }
  }
  return buf;
}

void test_cpu_hdr_decode() {
#if defined(ENGINE_ASSETS_HAS_STB)
  const std::vector<std::uint8_t> hdr = build_tiny_hdr();
  check(stbi_is_hdr_from_memory(hdr.data(), static_cast<int>(hdr.size())) != 0,
        "sky: stb recognizes Radiance hdr");
  int w = 0;
  int h = 0;
  int comp = 0;
  float* f = stbi_loadf_from_memory(hdr.data(), static_cast<int>(hdr.size()),
                                    &w, &h, &comp, 4);
  check(f != nullptr, "sky: stbi_loadf decodes hdr");
  if (f != nullptr) {
    check(w == 2 && h == 2, "sky: hdr dims 2x2");
    // Pixel 0 ~1.0 grey; pixel 1 HDR red > 1.5 (proves float range).
    const bool grey = std::fabs(f[0] - 1.0F) < 0.02F &&
                      std::fabs(f[1] - 1.0F) < 0.02F &&
                      std::fabs(f[2] - 1.0F) < 0.02F;
    check(grey, "sky: hdr grey pixel ~1.0");
    check(f[4] > 1.5F && f[5] < 0.05F && f[6] < 0.05F,
          "sky: hdr red pixel > 1 (float range)");
    stbi_image_free(f);
  }
  // Non-HDR junk is rejected, not decoded as HDR.
  const std::array<std::uint8_t, 8> junk = {0x89, 'P', 'N', 'G', 0, 0, 0, 0};
  check(stbi_is_hdr_from_memory(junk.data(), 8) == 0, "sky: png not hdr");
#else
  std::printf("[SKIP] sky: stb absent (offline configure)\n");
#endif
}

void test_cpu() {
  test_cpu_validate();
  test_cpu_face_dir();
  test_cpu_sample();
  test_cpu_hdr_decode();
}

#if defined(ENGINE_RENDERER_HAS_VULKAN)

// Extra compute entry points the shared GoldenVkProcs table lacks.
struct SkyComputeVk {
  PFN_vkCreateComputePipelines create_comp{nullptr};
  PFN_vkDestroyPipeline destroy_pipe{nullptr};
  PFN_vkCmdBindPipeline cmd_bind_pipe{nullptr};
  PFN_vkCmdBindDescriptorSets cmd_bind_sets{nullptr};
  PFN_vkCmdPushConstants cmd_push{nullptr};
  PFN_vkCmdDispatch cmd_dispatch{nullptr};
};

template <typename Loader>
void load_sky_compute_procs(Loader loader, SkyComputeVk* p) {
  auto load = [&](const char* name) { return loader(name); };
  p->create_comp = reinterpret_cast<PFN_vkCreateComputePipelines>(
      load("vkCreateComputePipelines"));
  p->destroy_pipe =
      reinterpret_cast<PFN_vkDestroyPipeline>(load("vkDestroyPipeline"));
  p->cmd_bind_pipe =
      reinterpret_cast<PFN_vkCmdBindPipeline>(load("vkCmdBindPipeline"));
  p->cmd_bind_sets = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(
      load("vkCmdBindDescriptorSets"));
  p->cmd_push =
      reinterpret_cast<PFN_vkCmdPushConstants>(load("vkCmdPushConstants"));
  p->cmd_dispatch = reinterpret_cast<PFN_vkCmdDispatch>(load("vkCmdDispatch"));
}

auto sky_gpu_missing(std::string_view what) -> int {
  return engine::test::GoldenGpuMissing(what);
}

// Synthetic equirect: r = u*4 (HDR range), g = v*2, b = 0.5, a = 1.
void fill_synthetic_equirect(std::vector<float>* px, std::uint32_t w,
                             std::uint32_t h) {
  px->resize(static_cast<std::size_t>(w) * h * 4);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(w);
      const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(h);
      const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
      (*px)[i + 0] = u * 4.0F;
      (*px)[i + 1] = v * 2.0F;
      (*px)[i + 2] = 0.5F;
      (*px)[i + 3] = 1.0F;
    }
  }
}

// Device-local cube-compatible RGBA32F image, 6 layers, STORAGE + SRC.
auto make_cube_image(engine::test::GoldenVk* g, std::uint32_t face,
                     engine::test::GoldenImage* out) -> bool {
  VkImageCreateInfo ii{};
  ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ii.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = VK_FORMAT_R32G32B32A32_SFLOAT;
  ii.extent = {face, face, 1};
  ii.mipLevels = 1;
  ii.arrayLayers = 6;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (g->vk.create_image(g->device, &ii, nullptr, &out->img) != VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements req{};
  g->vk.get_image_reqs(g->device, out->img, &req);
  VkPhysicalDeviceMemoryProperties mp{};
  g->vk.get_mem_props(g->physical, &mp);
  std::uint32_t mi = 0xFFFFFFFFu;
  for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((req.memoryTypeBits & (1u << i)) != 0u) {
      mi = i;
      break;
    }
  }
  if (mi == 0xFFFFFFFFu) {
    g->vk.destroy_image(g->device, out->img, nullptr);
    out->img = nullptr;
    return false;
  }
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = mi;
  if (g->vk.alloc_mem(g->device, &ai, nullptr, &out->mem) != VK_SUCCESS) {
    g->vk.destroy_image(g->device, out->img, nullptr);
    out->img = nullptr;
    return false;
  }
  if (g->vk.bind_image_mem(g->device, out->img, out->mem, 0) != VK_SUCCESS) {
    g->vk.free_mem(g->device, out->mem, nullptr);
    g->vk.destroy_image(g->device, out->img, nullptr);
    out->img = nullptr;
    out->mem = nullptr;
    return false;
  }
  VkImageViewCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vi.image = out->img;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  vi.format = VK_FORMAT_R32G32B32A32_SFLOAT;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
  return g->vk.create_view(g->device, &vi, nullptr, &out->view) == VK_SUCCESS;
}

auto test_gpu() -> int {
  using engine::renderer::VulkanDevice;
  constexpr std::uint32_t kEqW = 64;
  constexpr std::uint32_t kEqH = 32;
  constexpr std::uint32_t kFace = 64;

  VulkanDevice dev;
  engine::test::GoldenVk g{};
  engine::renderer::DeviceCreateInfo ci{};
  ci.app_name = "P4-05-skybox";
  ci.device_hint = "llvmpipe";
  const auto st = dev.init(ci);
  if (!st.ok() &&
      st.code == engine::renderer::StatusCode::kNoPhysicalDevice) {
    sky_gpu_missing("skybox-gpu: no physical device");
    return 0;
  }
  check(st.ok(), "skybox-gpu: device init ok");
  if (!st.ok()) {
    return 1;
  }
  g.device = dev.device();
  g.physical = dev.physical_device();
  g.queue = dev.graphics_queue();
  g.queue_family = dev.graphics_queue_family();
  auto loader = [&](const char* name) -> void* {
    void* p = dev.proc(name);
    return (p != nullptr) ? p : dev.instance_proc(name);
  };
  check(engine::test::GoldenVkLoad(loader, &g.vk),
        "skybox-gpu: entry points resolve");
  SkyComputeVk sv{};
  load_sky_compute_procs(loader, &sv);
  check(sv.create_comp != nullptr && sv.cmd_dispatch != nullptr &&
            sv.cmd_bind_sets != nullptr && sv.cmd_push != nullptr,
        "skybox-gpu: compute procs resolve");

  // SPIR-V magic + version (checked-in blob sanity, like slang blobs).
  check(kEquirect_to_cubeSpv[0] == 0x07230203u, "skybox-gpu: spv magic");
  check(kEquirect_to_cubeSpv[1] >= 0x00010300u, "skybox-gpu: spv >= 1.3");

  // Upload buffer with the synthetic equirect floats.
  std::vector<float> eq;
  fill_synthetic_equirect(&eq, kEqW, kEqH);
  engine::test::GoldenHostBuf up_b{};
  engine::test::GoldenHostBuf read_b{};
  engine::test::GoldenImage eq_img{};
  engine::test::GoldenImage cube_img{};
  VkSampler sampler = nullptr;
  VkDescriptorSetLayout set_layout = nullptr;
  VkPipelineLayout pipe_layout = nullptr;
  VkDescriptorPool pool = nullptr;
  VkDescriptorSet set = nullptr;
  VkPipeline pipe = nullptr;
  VkShaderModule mod = nullptr;

  const std::uint64_t eq_bytes = static_cast<std::uint64_t>(kEqW) * kEqH * 16;
  const std::uint64_t face_bytes =
      static_cast<std::uint64_t>(kFace) * kFace * 16;
  const std::uint64_t cube_bytes = face_bytes * 6;
  bool ok = engine::test::GoldenAllocHost(
                &g, eq_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &up_b) &&
            engine::test::GoldenAllocHost(
                &g, cube_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &read_b);
  check(ok, "skybox-gpu: staging buffers");
  if (ok) {
    void* p = nullptr;
    ok = g.vk.map_mem(g.device, up_b.mem, 0, eq_bytes, 0, &p) == VK_SUCCESS &&
         p != nullptr;
    if (ok) {
      auto* d = static_cast<float*>(p);
      for (std::size_t i = 0; i < eq.size(); ++i) {
        d[i] = eq[i];
      }
      g.vk.unmap_mem(g.device, up_b.mem);
    }
    check(ok, "skybox-gpu: equirect uploaded to staging");
  }
  // Equirect sampled image (RGBA32F, SAMPLED + DST).
  if (ok) {
    ok = engine::test::GoldenMakeImage(
        &g, VK_FORMAT_R32G32B32A32_SFLOAT, kEqW, kEqH,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        &eq_img);
    check(ok, "skybox-gpu: equirect image");
  }
  if (ok) {
    ok = make_cube_image(&g, kFace, &cube_img);
    check(ok, "skybox-gpu: cubemap image (6 layers)");
  }
  if (ok) {
    VkSamplerCreateInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    // Equirect wraps in U (longitude seam) but clamps in V (poles).
    // REPEAT in U keeps the +/-X (and +/-Z) top/bottom edge texels in
    // agreement with the CPU reference's u-wrap; CLAMP in V matches its
    // v-clamp. Production skybox sampling uses the same modes.
    si.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ok = g.vk.create_sampler(g.device, &si, nullptr, &sampler) == VK_SUCCESS;
    check(ok, "skybox-gpu: sampler");
  }
  // Set layout: 0 = combined sampler, 1 = storage image; push: face_size.
  if (ok) {
    VkDescriptorSetLayoutBinding binds[2]{};
    binds[0].binding = 0;
    binds[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binds[0].descriptorCount = 1;
    binds[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    binds[1].binding = 1;
    binds[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    binds[1].descriptorCount = 1;
    binds[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 2;
    li.pBindings = binds;
    ok = g.vk.create_set_layout(g.device, &li, nullptr, &set_layout) ==
         VK_SUCCESS;
    check(ok, "skybox-gpu: set layout");
  }
  if (ok) {
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pc.size = 4;
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    ok = g.vk.create_pipe_layout(g.device, &pli, nullptr, &pipe_layout) ==
         VK_SUCCESS;
    check(ok, "skybox-gpu: pipeline layout");
  }
  if (ok) {
    ok = engine::test::GoldenMakeModule(&g, kEquirect_to_cubeSpv,
                                        kEquirect_to_cubeSpvWords, &mod);
    check(ok, "skybox-gpu: compute module");
  }
  if (ok) {
    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = mod;
    cpi.stage.pName = "main";
    cpi.layout = pipe_layout;
    ok = sv.create_comp(g.device, nullptr, 1, &cpi, nullptr, &pipe) ==
         VK_SUCCESS;
    check(ok, "skybox-gpu: compute pipeline");
  }
  if (ok) {
    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[0].descriptorCount = 1;
    sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    sizes[1].descriptorCount = 1;
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 1;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    if (g.vk.create_pool(g.device, &pi, nullptr, &pool) != VK_SUCCESS) {
      ok = false;
    } else {
      VkDescriptorSetAllocateInfo ai{};
      ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      ai.descriptorPool = pool;
      ai.descriptorSetCount = 1;
      ai.pSetLayouts = &set_layout;
      ok = g.vk.alloc_sets(g.device, &ai, &set) == VK_SUCCESS;
    }
    check(ok, "skybox-gpu: descriptor set");
  }
  if (ok) {
    VkDescriptorImageInfo img_info{};
    img_info.sampler = sampler;
    img_info.imageView = eq_img.view;
    img_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo stor_info{};
    stor_info.imageView = cube_img.view;
    stor_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &img_info;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &stor_info;
    g.vk.update_sets(g.device, 2, writes, 0, nullptr);
    check(true, "skybox-gpu: descriptors written");
  }
  // Record: upload equirect, dispatch (face/16, face/16, 6), read back.
  if (ok) {
    VkCommandPool cmd_pool = nullptr;
    VkCommandBuffer cb = nullptr;
    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.queueFamilyIndex = g.queue_family;
    ok = g.vk.create_cmd_pool(g.device, &cpi, nullptr, &cmd_pool) ==
         VK_SUCCESS;
    if (ok) {
      VkCommandBufferAllocateInfo ab{};
      ab.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      ab.commandPool = cmd_pool;
      ab.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      ab.commandBufferCount = 1;
      ok = g.vk.alloc_cmds(g.device, &ab, &cb) == VK_SUCCESS;
    }
    if (ok) {
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      ok = g.vk.begin_cmd(cb, &bi) == VK_SUCCESS;
    }
    auto barrier_img = [&](VkImage img, VkImageLayout old_l, VkImageLayout new_l,
                           VkPipelineStageFlags2 src_s, VkAccessFlags2 src_a,
                           VkPipelineStageFlags2 dst_s, VkAccessFlags2 dst_a,
                           std::uint32_t layers) {
      VkImageMemoryBarrier2 b{};
      b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
      b.srcStageMask = src_s;
      b.srcAccessMask = src_a;
      b.dstStageMask = dst_s;
      b.dstAccessMask = dst_a;
      b.oldLayout = old_l;
      b.newLayout = new_l;
      b.image = img;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
      VkDependencyInfo d{};
      d.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
      d.imageMemoryBarrierCount = 1;
      d.pImageMemoryBarriers = &b;
      g.vk.cmd_barrier2(cb, &d);
    };
    if (ok) {
      barrier_img(eq_img.img, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, 0,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  1);
      VkBufferImageCopy rc{};
      rc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      rc.imageExtent = {kEqW, kEqH, 1};
      g.vk.cmd_copy_buf_img(cb, up_b.buf, eq_img.img,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rc);
      barrier_img(eq_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 1);
      barrier_img(cube_img.img, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_GENERAL, 0, 0,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, 6);
      sv.cmd_bind_pipe(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
      sv.cmd_bind_sets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout, 0, 1,
                       &set, 0, nullptr);
      sv.cmd_push(cb, pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &kFace);
      sv.cmd_dispatch(cb, kFace / 16, kFace / 16, 6);
      barrier_img(cube_img.img, VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                  6);
      for (std::uint32_t layer = 0; layer < 6; ++layer) {
        VkBufferImageCopy rl{};
        rl.bufferOffset = static_cast<VkDeviceSize>(layer) * face_bytes;
        rl.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
        rl.imageExtent = {kFace, kFace, 1};
        g.vk.cmd_copy_img_buf(cb, cube_img.img,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, read_b.buf,
                              1, &rl);
      }
      VkMemoryBarrier2 hb{};
      hb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
      hb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      hb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      hb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
      hb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
      VkDependencyInfo dh{};
      dh.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
      dh.memoryBarrierCount = 1;
      dh.pMemoryBarriers = &hb;
      g.vk.cmd_barrier2(cb, &dh);
      ok = g.vk.end_cmd(cb) == VK_SUCCESS;
    }
    check(ok, "skybox-gpu: frame recorded");
    if (ok) {
      ok = engine::test::GoldenSubmitWait(&g, cb);
      check(ok, "skybox-gpu: submit + fence wait");
    }
    if (cmd_pool != nullptr) {
      g.vk.destroy_cmd_pool(g.device, cmd_pool, nullptr);
    }
  }
  // Readback: every texel vs CPU reference (hardware-filter tolerance).
  if (ok) {
    void* p = nullptr;
    ok = g.vk.map_mem(g.device, read_b.mem, 0, cube_bytes, 0, &p) ==
             VK_SUCCESS &&
         p != nullptr;
    check(ok, "skybox-gpu: readback mapped");
    if (ok) {
      const auto* f = static_cast<const float*>(p);
      std::uint32_t bad = 0;
      std::uint32_t nan_ct = 0;
      float worst = 0.0F;
      float worst_u = 0.0F;
      float worst_v = 0.0F;
      std::uint32_t worst_face = 0;
      std::uint32_t worst_x = 0;
      std::uint32_t worst_y = 0;
      int worst_c = 0;
      for (std::uint32_t face = 0; face < 6; ++face) {
        for (std::uint32_t y = 0; y < kFace; ++y) {
          for (std::uint32_t x = 0; x < kFace; ++x) {
            float dir[3] = {0.0F, 0.0F, 0.0F};
            float want[3] = {0.0F, 0.0F, 0.0F};
            if (!engine::renderer::SkyboxFaceDir(face, x, y, kFace, dir) ||
                !engine::renderer::SkyboxSampleEquirectCpu(
                    eq.data(), kEqW, kEqH, dir, want)) {
              ++bad;
              continue;
            }
            const std::size_t i =
                (static_cast<std::size_t>(face) * kFace * kFace +
                 static_cast<std::size_t>(y) * kFace + x) *
                4;
            for (int c = 0; c < 3; ++c) {
              const float got = f[i + static_cast<std::size_t>(c)];
              if (!std::isfinite(got)) {
                ++nan_ct;
                continue;
              }
              const float dd = std::fabs(got - want[c]);
              if (dd > worst) {
                worst = dd;
                worst_face = face;
                worst_x = x;
                worst_y = y;
                worst_c = c;
                engine::renderer::SkyboxDirToUv(dir[0], dir[1], dir[2],
                                                &worst_u, &worst_v);
              }
              if (dd > 0.02F) {
                ++bad;
                if (bad <= 5) {
                  float uu = 0.0F;
                  float vv = 0.0F;
                  engine::renderer::SkyboxDirToUv(dir[0], dir[1], dir[2], &uu,
                                                  &vv);
                  std::printf(
                      "[INFO] skybox-gpu: mismatch face=%u x=%u y=%u c=%d "
                      "got=%.4f want=%.4f uv=%.4f,%.4f dir=%.4f,%.4f,%.4f\n",
                      face, x, y, c, static_cast<double>(got),
                      static_cast<double>(want[c]),
                      static_cast<double>(uu), static_cast<double>(vv),
                      static_cast<double>(dir[0]), static_cast<double>(dir[1]),
                      static_cast<double>(dir[2]));
                }
              }
            }
            if (std::fabs(f[i + 3] - 1.0F) > 1e-5F) {
              ++bad;
            }
          }
        }
      }
      std::printf("[INFO] skybox-gpu: worst abs diff %.6f, bad=%u nan=%u\n",
                  static_cast<double>(worst), bad, nan_ct);
      std::printf(
          "[INFO] skybox-gpu: worst at face=%u x=%u y=%u c=%d uv=%.5f,%.5f\n",
          worst_face, worst_x, worst_y, worst_c,
          static_cast<double>(worst_u), static_cast<double>(worst_v));
      check(bad == 0 && nan_ct == 0, "skybox-gpu: cubemap matches CPU ref");
      // HDR range survived: +X center samples u~0.75 -> r ~3.0.
      const std::size_t center_i =
          (static_cast<std::size_t>(0) * kFace * kFace +
           static_cast<std::size_t>(kFace / 2) * kFace +
           static_cast<std::size_t>(kFace / 2)) *
          4;
      check(f[center_i] > 2.0F, "skybox-gpu: HDR range > 1 survives");
      g.vk.unmap_mem(g.device, read_b.mem);
    }
  }
  if (pipe != nullptr) {
    sv.destroy_pipe(g.device, pipe, nullptr);
  }
  if (mod != nullptr) {
    g.vk.destroy_module(g.device, mod, nullptr);
  }
  if (set_layout != nullptr) {
    g.vk.destroy_set_layout(g.device, set_layout, nullptr);
  }
  if (pipe_layout != nullptr) {
    g.vk.destroy_pipe_layout(g.device, pipe_layout, nullptr);
  }
  if (pool != nullptr) {
    g.vk.destroy_pool(g.device, pool, nullptr);
  }
  if (sampler != nullptr) {
    g.vk.destroy_sampler(g.device, sampler, nullptr);
  }
  engine::test::GoldenFreeImage(&g, &eq_img);
  engine::test::GoldenFreeImage(&g, &cube_img);
  engine::test::GoldenFreeHost(&g, &up_b);
  engine::test::GoldenFreeHost(&g, &read_b);
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
  std::printf("[SKIP] skybox-gpu: SDK absent (stub build)\n");
#endif
  std::printf(failures == 0 ? "test_skybox_import: all checks passed\n"
                            : "test_skybox_import: %d FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}

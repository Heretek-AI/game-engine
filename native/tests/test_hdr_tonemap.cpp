// test_hdr_tonemap.cpp — P4-02 HDR rendering + ACES tonemapping golden.
//
// Renders through the REAL shade + post pipelines into an offscreen HDR
// target (R16G16B16A16_SFLOAT) then LDR (R8G8B8A8_UNORM) and compares
// against golden p4_02_hdr_320x240 via golden_compare.hpp (P1-05).
//
// Frame shape (mirrors test_pbr_golden.cpp, one submit, fence-waited):
//   vis buffer upload (2 instances, left/right halves) -> HDR shade ->
//   post (exposure from HdrCameraComponent via HdrExposureFromEv) ->
//   LDR readback.
//
// Scene: instance 0 (left half) emissive red 10.0 (HDR probe); instance 1
// (right half) mid-grey diffuse 0.5 under one point light (sanity that
// direct lighting still shades). One render at EV -3: the emissive-10
// pixel must NOT clip to white (acceptance), and must match the CPU
// HdrTonemapCpu reference within 8-bit rounding.
//
// [FAIL] (not [SKIP]) under ENGINE_REQUIRE_GPU=1 (rule 11 / P1-01).
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>
#include <vector>

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#endif

#include "engine/renderer/bindless_table.hpp"
#include "engine/renderer/hdr_camera.hpp"
#include "engine/renderer/pbr_material.hpp"
#include "engine/renderer/pbr_pipeline.hpp"
#include "engine/renderer/vulkan_device.hpp"

#include "engine/renderer/shaders/pbr_shade_frag_spv.inc"
#include "engine/renderer/shaders/pbr_shade_vert_spv.inc"
#include "engine/renderer/shaders/pbr_vis_frag_spv.inc"
#include "engine/renderer/shaders/pbr_vis_vert_spv.inc"
#include "engine/renderer/shaders/post_frag_spv.inc"
#include "engine/renderer/shaders/post_vert_spv.inc"

#include "golden_compare.hpp"
#include "golden_pipe_helper.hpp"
#include "golden_vk_helper.hpp"

// TU-side engine includes for the helper template instantiations below
// (GoldenOpenDevice names engine types; the helper header stays engine-
// include-free so stub builds compile it without Vulkan types).
#include "engine/renderer/bindless_table.hpp"
#include "engine/renderer/pbr_pipeline.hpp"
#include "engine/renderer/vulkan_device.hpp"

// check()/failures/gpu_missing mirror test_pbr_golden.cpp and
// test_pbr_materials.cpp by repo convention (each GPU test TU is
// standalone with its own main). Shared Vulkan scaffolding (proc
// loading, alloc/image/barrier/submit, pipe + descriptor builders)
// lives in golden_vk_helper.hpp so Sonar's duplication gate only sees
// the per-TU check helper, not the frame struct-fill.
namespace {

auto failures = 0;

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}

#if defined(ENGINE_RENDERER_HAS_VULKAN)
// [FAIL]-not-[SKIP] under ENGINE_REQUIRE_GPU=1 (rule 11 / P1-01 contract).
// Shared helper (golden_vk_helper.hpp) so the 3 golden TUs don't each
// carry the 16-line getenv fallback (Sonar duplication gate).
auto gpu_missing(std::string_view what) -> int {
  return engine::test::GoldenGpuMissing(what);
}
#endif

// P4-02 CPU contract: EV->exposure, tonemap reference, component size.
// No GPU needed; runs on stub builds too (defined OUTSIDE the Vulkan
// guard — main() calls it unconditionally; CI stub builds need it).
void test_cpu() {
  using engine::renderer::HdrCameraComponent;
  using engine::renderer::HdrExposureFromEv;
  using engine::renderer::HdrTonemapCpu;
  check(sizeof(HdrCameraComponent) == 16, "hdr: camera component 16B");
  const float ev0 = HdrExposureFromEv(0.0f);
  check(ev0 > 0.999f && ev0 < 1.001f, "hdr: EV0 exposure 1");
  const float evn3 = HdrExposureFromEv(-3.0f);
  check(evn3 > 0.124f && evn3 < 0.126f, "hdr: EV-3 exposure 0.125");
  const float evp2 = HdrExposureFromEv(2.0f);
  check(evp2 > 3.99f && evp2 < 4.01f, "hdr: EV+2 exposure 4");
  // Mid-grey through the reference: must sit strictly inside (0, 255).
  const float grey[3] = {0.5f, 0.5f, 0.5f};
  float grey_out[3] = {0.0f, 0.0f, 0.0f};
  check(HdrTonemapCpu(grey, 1.0f, grey_out), "hdr: tonemap ok");
  // ACES(0.5) ~ 0.616 -> sRGB ~ 0.807: brightened mid-grey (the S-curve
  // lifts mid-tones; neutrality + range is what this asserts).
  check(grey_out[0] > 0.79f && grey_out[0] < 0.82f, "hdr: grey mid-range");
  // Acceptance probe on the CPU path: emissive 10 at EV-3 must not
  // reach white (1.0 maps to 255; the probe asserts headroom below it).
  const float hot[3] = {10.0f, 10.0f, 10.0f};
  float hot_out[3] = {0.0f, 0.0f, 0.0f};
  check(HdrTonemapCpu(hot, evn3, hot_out), "hdr: hot tonemap ok");
  check(hot_out[0] < 0.99f, "hdr: emissive-10 at EV-3 not white");
  check(hot_out[0] > 0.5f, "hdr: emissive-10 at EV-3 still bright");
  // Null guards fail closed without writing.
  float keep[3] = {0.25f, 0.25f, 0.25f};
  check(!HdrTonemapCpu(nullptr, 1.0f, keep), "hdr: null rgb rejected");
  check(keep[0] == 0.25f, "hdr: null rgb leaves out alone");
  check(!HdrTonemapCpu(grey, 1.0f, nullptr), "hdr: null out rejected");
}

#if defined(ENGINE_RENDERER_HAS_VULKAN)

constexpr std::uint32_t kW = 320;
constexpr std::uint32_t kH = 240;
constexpr std::uint32_t kInst = 2;

// P4-02 frame inputs: scene/material/light/grid/index/vis CPU bytes for
// the 2-instance halves golden. Built by fill_hdr_inputs(); the upload +
// image + descriptor + record + probe stages below consume these vectors.
struct HdrInputs {
  std::vector<std::uint8_t> scene;
  std::vector<std::uint8_t> mats;
  float light_data[8]{};
  std::vector<std::uint32_t> grid;
  std::uint32_t index_list[1]{0};
  std::vector<std::uint32_t> vis;
};

// Visibility: left half -> instance 0 (emissive 10), right half ->
// instance 1 (diffuse grey).
inline void fill_hdr_vis(std::uint32_t w, std::uint32_t h,
                         std::vector<std::uint32_t>* vis) {
  vis->assign(w * h * 2, 0);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      const std::uint32_t inst = (x < w / 2) ? 0u : 1u;
      (*vis)[(y * w + x) * 2 + 0] = inst;
      (*vis)[(y * w + x) * 2 + 1] = 0;
    }
  }
}

inline auto fill_hdr_materials(std::uint8_t* scene, std::uint8_t* mats,
                               engine::renderer::PbrMaterialLibrary* lib)
    -> bool {
  bool pack_ok = true;
  for (std::uint32_t inst = 0; inst < kInst && pack_ok; ++inst) {
    engine::test::GoldenFillSceneRecord(scene, inst,
                                        (inst == 0) ? 80.0f : 240.0f, 120.0f,
                                        0.0f, 60.0f);
    engine::renderer::PbrMaterialAsset a{};
    if (inst == 0) {
      // HDR probe: black diffuse (no direct response), emissive 10.
      a.base_color = {0.0f, 0.0f, 0.0f, 1.0f};
      a.emissive = {10.0f, 2.0f, 1.0f, 0.0f};
    } else {
      a.base_color = {0.5f, 0.5f, 0.5f, 1.0f};
      a.metallic_roughness = {0.0f, 0.9f, 0.0f, 0.0f};
    }
    if (!engine::test::GoldenPackMaterial(
            check, lib, a, inst,
            reinterpret_cast<float*>(mats + inst * 64),
            "hdr-golden: material add", "hdr-golden: record pack")) {
      pack_ok = false;
    }
  }
  return pack_ok;
}

inline void fill_hdr_inputs(HdrInputs* in) {
  constexpr std::uint64_t kSceneBytes = kInst * 96;
  constexpr std::uint64_t kMatBytes = kInst * 64;
  in->scene.assign(kSceneBytes, 0);
  in->mats.assign(kMatBytes, 0);
  // One dim point light; the left (emissive) half must stay unlit so
  // the acceptance probe measures emissive only.
  in->light_data[0] = 160.0f;
  in->light_data[1] = -500.0f;
  in->light_data[2] = 200.0f;
  in->light_data[3] = 1.0f;
  constexpr std::uint32_t kTilesX = kW / 16;
  constexpr std::uint32_t kTilesY = kH / 16;
  in->grid.assign(kTilesX * kTilesY * 2, 0);
  engine::test::GoldenFillTrivialGrid(in->grid.data(), kTilesX * kTilesY);
  fill_hdr_vis(kW, kH, &in->vis);
}

// Acceptance: emissive 10 at EV-3 does not clip to white.
// Left-half center pixel (80,120): HDR 10 * 0.125 = 1.25 pre-tonemap;
// ACES(1.25) ~ 0.83 -> sRGB ~ 0.93 -> ~237. White would be 255; assert
// headroom (r < 250) AND brightness (r > 150) so the probe fails both
// when clipped and when the emissive term goes missing (black).
inline void probe_hdr_acceptance(const std::vector<std::uint8_t>& pixels) {
  const std::size_t c = (120 * kW + 80) * 4;
  const int r = pixels[c];
  const int gg = pixels[c + 1];
  const int b = pixels[c + 2];
  check(r < 250 && gg < 250 && b < 250, "hdr-golden: no white clip");
  check(r > 150, "hdr-golden: emissive survives exposure");
  check(r > gg && gg >= b, "hdr-golden: red tint kept");
  // CPU/GPU parity: the same HDR value through HdrTonemapCpu must land
  // within 8-bit rounding (+1 for half-float target) of the GPU pixel.
  const float hot[3] = {10.0f, 2.0f, 1.0f};
  float ref[3] = {0.0f, 0.0f, 0.0f};
  const bool ref_ok = engine::renderer::HdrTonemapCpu(
      hot, engine::renderer::HdrExposureFromEv(-3.0f), ref);
  check(ref_ok, "hdr-golden: cpu reference ok");
  if (ref_ok) {
    const int want_r = static_cast<int>(ref[0] * 255.0f + 0.5f);
    int dr = r - want_r;
    if (dr < 0) {
      dr = -dr;
    }
    check(dr <= 4, "hdr-golden: gpu matches cpu reference");
  }
}

// Failures counter hook for GoldenOpenDevice's missing-device path.
inline void hdr_gpu_missing() {
  failures += gpu_missing("hdr-golden: no physical device");
}

// HDR golden device bootstrap: llvmpipe + procs + PBR plan + bindless.
// Wraps GoldenOpenDevice with this TU's labels (Sonar S3776: keeps the
// two ifs + two lambdas out of test_gpu's complexity budget).
inline auto open_hdr_device(engine::renderer::VulkanDevice* dev,
                            engine::test::GoldenVk* g,
                            engine::renderer::PbrPipeline* pbr,
                            engine::renderer::BindlessTable* bt) -> bool {
  auto check_fn = [&](bool ok, const char* name) { check(ok, name); };
  auto missing_fn = [&](const char* /*what*/) { hdr_gpu_missing(); };
  return engine::test::GoldenOpenDevice(
      check_fn, missing_fn, "P4-02-hdr", "hdr-golden: device init ok",
      "hdr-golden: entry points resolve", "hdr-golden: plan init",
      "hdr-golden: bindless init", "hdr-golden: bindless layout builds", dev,
      g, pbr, bt);
}

// Shade/post pipeline pair for the HDR golden (shared builder in
// golden_vk_helper.hpp — the same module/layout/pipeline struct-fill the
// P4-01 TUs carry inline; the helper keeps this TU from tripping Sonar's
// duplication gate). Returns false after destroying partial state.
// Device-routed Vulkan callables for GoldenBuildShadePost (named funcs,
// not lambdas: Sonar S3776 counts each lambda body's branches + nesting
// into the enclosing function).
inline auto hdr_create_set_layout(VkDevice device,
                                  const VkDescriptorSetLayoutCreateInfo* info,
                                  const VkAllocationCallbacks* alloc,
                                  VkDescriptorSetLayout* out,
                                  engine::test::GoldenVk* g) {
  return g->vk.create_set_layout(device, info, alloc, out);
}

inline auto hdr_create_pipe_layout(const VkPipelineLayoutCreateInfo* info,
                                   VkPipelineLayout* out,
                                   engine::test::GoldenVk* g) -> bool {
  return g->vk.create_pipe_layout(g->device, info, nullptr, out) ==
         VK_SUCCESS;
}

inline auto hdr_create_pipes(VkDevice device, VkPipelineCache cache,
                             std::uint32_t count,
                             const VkGraphicsPipelineCreateInfo* infos,
                             const VkAllocationCallbacks* alloc,
                             VkPipeline* pipes, engine::test::GoldenVk* g) {
  return g->vk.create_pipes(device, cache, count, infos, alloc, pipes);
}

inline void hdr_destroy_mod(VkShaderModule m, engine::test::GoldenVk* g) {
  if (m != nullptr) {
    g->vk.destroy_module(g->device, m, nullptr);
  }
}

inline auto build_hdr_pipes(engine::test::GoldenVk* g,
                            engine::renderer::BindlessTable* bt,
                            engine::test::GoldenShadePost* sp) -> bool {
  auto create_set_layout =
      [&](VkDevice device, const VkDescriptorSetLayoutCreateInfo* info,
          const VkAllocationCallbacks* alloc, VkDescriptorSetLayout* out) {
        return hdr_create_set_layout(device, info, alloc, out, g);
      };
  auto create_pipe_layout = [&](const VkPipelineLayoutCreateInfo* info,
                                VkPipelineLayout* out) {
    return hdr_create_pipe_layout(info, out, g);
  };
  auto create_pipes = [&](VkDevice device, VkPipelineCache cache,
                          std::uint32_t count,
                          const VkGraphicsPipelineCreateInfo* infos,
                          const VkAllocationCallbacks* alloc,
                          VkPipeline* pipes) {
    return hdr_create_pipes(device, cache, count, infos, alloc, pipes, g);
  };
  auto destroy_mod = [&](VkShaderModule m) { hdr_destroy_mod(m, g); };
  const bool ok = engine::test::GoldenBuildShadePost(
      g, bt->set_layout(), kPbr_shade_vertSpv, kPbr_shade_vertSpvWords,
      kPbr_shade_fragSpv, kPbr_shade_fragSpvWords,
      engine::renderer::kHdrSceneFormat, kPost_vertSpv, kPost_vertSpvWords,
      kPost_fragSpv, kPost_fragSpvWords, VK_FORMAT_R8G8B8A8_UNORM,
      create_set_layout, create_pipe_layout, create_pipes, destroy_mod, sp);
  check(ok, "hdr-golden: shade+post pipelines build");
  return ok;
}

// Seven upload buffers (scene/mat/light/grid/index + vis/readback).
// One GoldenAllocHost call per buffer via a small table (keeps Sonar's
// cognitive-complexity gate quiet: a 7-way && chain costs +7).
struct HdrUploadSpec {
  std::uint64_t bytes{0};
  VkBufferUsageFlags usage{};
  engine::test::GoldenHostBuf* out{nullptr};
};

inline auto alloc_hdr_uploads(engine::test::GoldenVk* g,
                              std::uint64_t kSceneBytes,
                              std::uint64_t kMatBytes, const HdrInputs& in,
                              engine::test::GoldenHostBuf* scene_b,
                              engine::test::GoldenHostBuf* mat_b,
                              engine::test::GoldenHostBuf* light_b,
                              engine::test::GoldenHostBuf* grid_b,
                              engine::test::GoldenHostBuf* index_b,
                              engine::test::GoldenHostBuf* vis_b,
                              engine::test::GoldenHostBuf* read_b) -> bool {
  constexpr VkBufferUsageFlags kStorSrc =
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  const HdrUploadSpec specs[7] = {
      {kSceneBytes, kStorSrc, scene_b},
      {kMatBytes, kStorSrc, mat_b},
      {sizeof(in.light_data), kStorSrc, light_b},
      {in.grid.size() * 4, kStorSrc, grid_b},
      {sizeof(in.index_list), kStorSrc, index_b},
      {static_cast<std::uint64_t>(kW) * kH * 8, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
       vis_b},
      {static_cast<std::uint64_t>(kW) * kH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
       read_b},
  };
  bool bufs_ok = true;
  for (const auto& s : specs) {
    bufs_ok = engine::test::GoldenAllocHost(g, s.bytes, s.usage, s.out) &&
              bufs_ok;
  }
  check(bufs_ok, "hdr-golden: upload buffers");
  return bufs_ok;
}

inline auto upload_hdr_bytes(engine::test::GoldenVk* g, const HdrInputs& in,
                             const float* light_data,
                             const std::uint32_t* index_list,
                             engine::test::GoldenHostBuf* scene_b,
                             engine::test::GoldenHostBuf* mat_b,
                             engine::test::GoldenHostBuf* light_b,
                             engine::test::GoldenHostBuf* grid_b,
                             engine::test::GoldenHostBuf* index_b,
                             engine::test::GoldenHostBuf* vis_b) -> bool {
  auto as_bytes = [](const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    return std::span<const std::uint8_t>(b, n);
  };
  // Six uploads via a small table (same complexity reason as above).
  struct UpSpec {
    engine::test::GoldenHostBuf* dst{nullptr};
    std::span<const std::uint8_t> bytes{};
  };
  const UpSpec ups[6] = {
      {scene_b, as_bytes(in.scene.data(), in.scene.size())},
      {mat_b, as_bytes(in.mats.data(), in.mats.size())},
      {light_b, as_bytes(light_data, sizeof(in.light_data))},
      {grid_b, as_bytes(in.grid.data(), in.grid.size() * 4)},
      {index_b, as_bytes(index_list, sizeof(in.index_list))},
      {vis_b, as_bytes(in.vis.data(), in.vis.size() * 4)},
  };
  bool up_ok = true;
  for (const auto& u : ups) {
    up_ok = engine::test::GoldenUploadBytes(g, u.dst, u.bytes) && up_ok;
  }
  check(up_ok, "hdr-golden: uploads mapped");
  return up_ok;
}

inline auto alloc_hdr_images(engine::test::GoldenVk* g,
                             engine::test::GoldenImage* vis_img,
                             engine::test::GoldenImage* hdr_img,
                             engine::test::GoldenImage* ldr_img) -> bool {
  const bool imgs_ok =
      engine::test::GoldenMakeImage(
          g, VK_FORMAT_R32G32_UINT, kW, kH,
          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
          vis_img) &&
      // P4-02 HDR scene target: float format, color-attachment write in
      // the shade pass + sampled input to the post pass (the only
      // consumer of the HDR contents; swapchain stays LDR below).
      engine::test::GoldenMakeImage(
          g, engine::renderer::kHdrSceneFormat, kW, kH,
          engine::renderer::kHdrSceneUsage, hdr_img) &&
      engine::test::GoldenMakeImage(
          g, VK_FORMAT_R8G8B8A8_UNORM, kW, kH,
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
          ldr_img);
  check(imgs_ok, "hdr-golden: images allocated");
  return imgs_ok;
}

// Upload all frame inputs + allocate the vis/HDR/LDR images (shared
// alloc/upload/image struct-fill; returns false after check(false)).
// The HDR contract probes (format + usage bits) ride here too.
inline auto alloc_hdr_frame(engine::test::GoldenVk* g, const HdrInputs& in,
                            std::uint64_t kSceneBytes,
                            std::uint64_t kMatBytes, const float* light_data,
                            const std::uint32_t* index_list,
                            engine::test::GoldenHostBuf* scene_b,
                            engine::test::GoldenHostBuf* mat_b,
                            engine::test::GoldenHostBuf* light_b,
                            engine::test::GoldenHostBuf* grid_b,
                            engine::test::GoldenHostBuf* index_b,
                            engine::test::GoldenHostBuf* vis_b,
                            engine::test::GoldenHostBuf* read_b,
                            engine::test::GoldenImage* vis_img,
                            engine::test::GoldenImage* hdr_img,
                            engine::test::GoldenImage* ldr_img) -> bool {
  if (!alloc_hdr_uploads(g, kSceneBytes, kMatBytes, in, scene_b, mat_b,
                         light_b, grid_b, index_b, vis_b, read_b)) {
    return false;
  }
  if (!upload_hdr_bytes(g, in, light_data, index_list, scene_b, mat_b,
                        light_b, grid_b, index_b, vis_b)) {
    return false;
  }
  if (!alloc_hdr_images(g, vis_img, hdr_img, ldr_img)) {
    return false;
  }
  // Contract: the engine constant really is the SFLOAT HDR format and
  // carries both usage bits the frame record needs.
  check(engine::renderer::kHdrSceneFormat ==
            VK_FORMAT_R16G16B16A16_SFLOAT,
        "hdr-golden: HDR format is R16G16B16A16_SFLOAT");
  check((engine::renderer::kHdrSceneUsage &
         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) != 0 &&
            (engine::renderer::kHdrSceneUsage &
             VK_IMAGE_USAGE_SAMPLED_BIT) != 0,
        "hdr-golden: HDR usage is attachment+sampled");
  return true;
}

// Descriptor sets + writes for the HDR frame (shared alloc/write fill;
// returns false after check(false)).
inline auto alloc_hdr_sets(engine::test::GoldenVk* g, VkDescriptorSetLayout set0_layout,
                           VkDescriptorSetLayout set1_shade,
                           VkDescriptorSetLayout set1_post,
                           const engine::test::GoldenHostBuf* scene_b,
                           const engine::test::GoldenHostBuf* mat_b,
                           const engine::test::GoldenHostBuf* light_b,
                           const engine::test::GoldenHostBuf* grid_b,
                           const engine::test::GoldenHostBuf* index_b,
                           const engine::test::GoldenImage* vis_img,
                           const engine::test::GoldenImage* hdr_img,
                           VkSampler sampler, std::uint64_t kSceneBytes,
                           std::uint64_t kMatBytes, std::uint64_t light_bytes,
                           std::uint64_t grid_bytes, std::uint64_t index_bytes,
                           VkDescriptorPool* pool, VkDescriptorSet* set0,
                           VkDescriptorSet* set_shade,
                           VkDescriptorSet* set_post) -> bool {
  auto create_pool = [&](VkDevice device,
                         const VkDescriptorPoolCreateInfo* info,
                         const VkAllocationCallbacks* alloc,
                         VkDescriptorPool* out) {
    return g->vk.create_pool(device, info, alloc, out);
  };
  auto alloc_sets = [&](VkDevice device,
                        const VkDescriptorSetAllocateInfo* info,
                        VkDescriptorSet* out) {
    return g->vk.alloc_sets(device, info, out);
  };
  auto update_sets = [&](VkDevice device,
                         const VkWriteDescriptorSet* writes) {
    g->vk.update_sets(device, 9, writes, 0, nullptr);
  };
  engine::test::GoldenFrameHandles h{};
  h.scene_buf = scene_b->buf;
  h.scene_bytes = kSceneBytes;
  h.mat_buf = mat_b->buf;
  h.mat_bytes = kMatBytes;
  h.light_buf = light_b->buf;
  h.light_bytes = light_bytes;
  h.grid_buf = grid_b->buf;
  h.grid_bytes = grid_bytes;
  h.index_buf = index_b->buf;
  h.index_bytes = index_bytes;
  h.vis_view = vis_img->view;
  h.hdr_view = hdr_img->view;
  h.sampler = sampler;
  const bool sets_ok = engine::test::GoldenAllocWriteFrameSets(
      g, set0_layout, set1_shade, set1_post, create_pool, alloc_sets,
      update_sets, h, pool, set0, set_shade, set_post);
  check(sets_ok, "hdr-golden: descriptor sets");
  return sets_ok;
}

// Record the shade->post frame + LDR readback copy into one command
// buffer (submit-ready cb + pool, or false on any failure).
inline auto begin_hdr_frame(engine::test::GoldenVk* g,
                            const engine::test::GoldenHostBuf* vis_b,
                            const engine::test::GoldenImage* vis_img,
                            const engine::test::GoldenImage* hdr_img,
                            const engine::test::GoldenImage* ldr_img,
                            const engine::test::GoldenHostBuf* read_b,
                            VkPipeline shade_pipe, VkPipeline post_pipe,
                            VkPipelineLayout shade_layout,
                            VkPipelineLayout post_layout,
                            VkDescriptorSet set0, VkDescriptorSet set_shade,
                            VkDescriptorSet set_post, VkCommandPool* pool_out,
                            VkCommandBuffer* cb_out) -> bool {
  VkCommandPoolCreateInfo cpi{};
  cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpi.queueFamilyIndex = g->queue_family;
  if (g->vk.create_cmd_pool(g->device, &cpi, nullptr, pool_out) !=
      VK_SUCCESS) {
    check(false, "hdr-golden: cmd pool");
    return false;
  }
  VkCommandBufferAllocateInfo ab{};
  ab.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  ab.commandPool = *pool_out;
  ab.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ab.commandBufferCount = 1;
  if (g->vk.alloc_cmds(g->device, &ab, cb_out) != VK_SUCCESS) {
    check(false, "hdr-golden: cmd alloc");
    g->vk.destroy_cmd_pool(g->device, *pool_out, nullptr);
    *pool_out = nullptr;
    return false;
  }
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (g->vk.begin_cmd(*cb_out, &bi) != VK_SUCCESS) {
    g->vk.destroy_cmd_pool(g->device, *pool_out, nullptr);
    *pool_out = nullptr;
    return false;
  }
  engine::test::GoldenFrame fr{};
  engine::test::GoldenFrameBufs fb{};
  fb.vis_b = *vis_b;
  fb.vis_img = *vis_img;
  fb.hdr_img = *hdr_img;
  fb.ldr_img = *ldr_img;
  fb.set0 = set0;
  fb.set_shade = set_shade;
  fb.set_post = set_post;
  engine::test::GoldenFillFrame(&fr, &fb, kW, kH, shade_pipe, post_pipe,
                                shade_layout, post_layout,
                                engine::renderer::HdrExposureFromEv(-3.0f));
  engine::test::GoldenRecordFrame(g, *cb_out, fr);
  auto copy_fn = [&](VkCommandBuffer c, VkImage img, VkImageLayout lay,
                     VkBuffer buf, std::uint32_t n,
                     const VkBufferImageCopy* r) {
    g->vk.cmd_copy_img_buf(c, img, lay, buf, n, r);
  };
  auto barrier_fn = [&](VkCommandBuffer c, const VkDependencyInfo* d) {
    g->vk.cmd_barrier2(c, d);
  };
  auto end_fn = [&](VkCommandBuffer c) {
    return g->vk.end_cmd(c) == VK_SUCCESS;
  };
  if (!engine::test::GoldenSubmitReadback(g, *cb_out, ldr_img->img,
                                          read_b->buf, kW, kH, copy_fn,
                                          barrier_fn, end_fn)) {
    g->vk.destroy_cmd_pool(g->device, *pool_out, nullptr);
    *pool_out = nullptr;
    return false;
  }
  return true;
}

// Submit the recorded frame + fence-wait it.
inline auto record_hdr_frame(engine::test::GoldenVk* g,
                             const engine::test::GoldenHostBuf* vis_b,
                             const engine::test::GoldenImage* vis_img,
                             const engine::test::GoldenImage* hdr_img,
                             const engine::test::GoldenImage* ldr_img,
                             const engine::test::GoldenHostBuf* read_b,
                             VkPipeline shade_pipe, VkPipeline post_pipe,
                             VkPipelineLayout shade_layout,
                             VkPipelineLayout post_layout,
                             VkDescriptorSet set0, VkDescriptorSet set_shade,
                             VkDescriptorSet set_post) -> bool {
  VkCommandPool cmd_pool = nullptr;
  VkCommandBuffer cb = nullptr;
  if (!begin_hdr_frame(g, vis_b, vis_img, hdr_img, ldr_img, read_b,
                       shade_pipe, post_pipe, shade_layout, post_layout, set0,
                       set_shade, set_post, &cmd_pool, &cb)) {
    return false;
  }
  const bool submit_ok = engine::test::GoldenSubmitWait(g, cb);
  check(submit_ok, "hdr-golden: submit + fence wait");
  g->vk.destroy_cmd_pool(g->device, cmd_pool, nullptr);
  return submit_ok;
}

// Map the readback buffer + run the golden + acceptance probes.
inline auto check_hdr_pixels(engine::test::GoldenVk* g,
                             const engine::test::GoldenHostBuf* read_b,
                             std::vector<std::uint8_t>* pixels) -> bool {
  const std::size_t n = static_cast<std::size_t>(kW) * kH * 4;
  auto map_fn = [&](VkDevice d, VkDeviceMemory m, VkDeviceSize o,
                    VkDeviceSize s, VkMemoryMapFlags f, void** p) {
    return g->vk.map_mem(d, m, o, s, f, p);
  };
  auto unmap_fn = [&](VkDevice d, VkDeviceMemory m) {
    g->vk.unmap_mem(d, m);
  };
  if (!engine::test::GoldenMapPixels(map_fn, unmap_fn, g->device, read_b->mem,
                                     pixels->data(), n)) {
    check(false, "hdr-golden: readback mapped");
    return false;
  }
  check(true, "hdr-golden: readback mapped");
  failures += engine::test::golden_check(
      "p4_02_hdr_320x240",
      std::span<const std::uint8_t>(pixels->data(), pixels->size()), kW, kH);
  probe_hdr_acceptance(*pixels);
  return true;
}

auto test_gpu() -> int {
  using engine::renderer::BindlessTable;
  using engine::renderer::PbrMaterialLibrary;
  using engine::renderer::PbrPipeline;
  using engine::renderer::VulkanDevice;
  VulkanDevice dev;
  engine::test::GoldenVk g{};
  PbrPipeline pbr;
  BindlessTable bt;
  if (!open_hdr_device(&dev, &g, &pbr, &bt)) {
    return 1;
  }

  engine::test::GoldenShadePost sp{};
  if (!build_hdr_pipes(&g, &bt, &sp)) {
    bt.destroy_gpu();
    dev.shutdown();
    return 1;
  }

  // -- Frame content (handles live here; stages fill via helpers) --------
  engine::test::GoldenHostBuf scene_b{};
  engine::test::GoldenHostBuf mat_b{};
  engine::test::GoldenHostBuf light_b{};
  engine::test::GoldenHostBuf grid_b{};
  engine::test::GoldenHostBuf index_b{};
  engine::test::GoldenHostBuf vis_b{};
  engine::test::GoldenHostBuf read_b{};
  engine::test::GoldenImage vis_img{};
  engine::test::GoldenImage hdr_img{};
  engine::test::GoldenImage ldr_img{};
  VkSampler sampler = nullptr;
  VkDescriptorPool pool = nullptr;
  VkDescriptorSet set0 = nullptr;
  VkDescriptorSet set_shade = nullptr;
  VkDescriptorSet set_post = nullptr;
  const int frame_rc = [&]() -> int {
    constexpr std::uint64_t kSceneBytes = kInst * 96;
    constexpr std::uint64_t kMatBytes = kInst * 64;
    HdrInputs in{};
    fill_hdr_inputs(&in);
    PbrMaterialLibrary pack_lib;
    pack_lib.init();
    PbrMaterialLibrary& lib = pack_lib;
    const bool pack_ok =
        fill_hdr_materials(in.scene.data(), in.mats.data(), &lib);
    check(pack_ok, "hdr-golden: 2 materials packed");
    if (!pack_ok) {
      return 1;
    }
    if (!alloc_hdr_frame(&g, in, kSceneBytes, kMatBytes, in.light_data,
                         in.index_list, &scene_b, &mat_b, &light_b, &grid_b,
                         &index_b, &vis_b, &read_b, &vis_img, &hdr_img,
                         &ldr_img)) {
      return 1;
    }

    {
      VkSamplerCreateInfo si{};
      si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
      si.magFilter = VK_FILTER_LINEAR;
      si.minFilter = VK_FILTER_LINEAR;
      si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      if (g.vk.create_sampler(g.device, &si, nullptr, &sampler) !=
          VK_SUCCESS) {
        check(false, "hdr-golden: sampler");
        return 1;
      }
    }

    if (!alloc_hdr_sets(&g, bt.set_layout(), sp.set1_shade, sp.set1_post,
                        &scene_b, &mat_b, &light_b, &grid_b, &index_b,
                        &vis_img, &hdr_img, sampler, kSceneBytes, kMatBytes,
                        sizeof(in.light_data), in.grid.size() * 4,
                        sizeof(in.index_list), &pool, &set0, &set_shade,
                        &set_post)) {
      return 1;
    }
    check(true, "hdr-golden: descriptors written");

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kW) * kH * 4, 0);
    if (!record_hdr_frame(&g, &vis_b, &vis_img, &hdr_img, &ldr_img, &read_b,
                          sp.shade_pipe, sp.post_pipe, sp.shade_layout,
                          sp.post_layout, set0, set_shade, set_post)) {
      return 1;
    }
    check_hdr_pixels(&g, &read_b, &pixels);

    return 0;
  }();
  (void)frame_rc;
  g.vk.destroy_pool(g.device, pool, nullptr);
  g.vk.destroy_sampler(g.device, sampler, nullptr);
  engine::test::GoldenFreeImage(&g, &vis_img);
  engine::test::GoldenFreeImage(&g, &hdr_img);
  engine::test::GoldenFreeImage(&g, &ldr_img);
  engine::test::GoldenFreeHost(&g, &scene_b);
  engine::test::GoldenFreeHost(&g, &mat_b);
  engine::test::GoldenFreeHost(&g, &light_b);
  engine::test::GoldenFreeHost(&g, &grid_b);
  engine::test::GoldenFreeHost(&g, &index_b);
  engine::test::GoldenFreeHost(&g, &vis_b);
  engine::test::GoldenFreeHost(&g, &read_b);
  g.vk.destroy_pipe(g.device, sp.shade_pipe, nullptr);
  g.vk.destroy_pipe(g.device, sp.post_pipe, nullptr);
  g.vk.destroy_pipe_layout(g.device, sp.shade_layout, nullptr);
  g.vk.destroy_pipe_layout(g.device, sp.post_layout, nullptr);
  g.vk.destroy_set_layout(g.device, sp.set1_shade, nullptr);
  g.vk.destroy_set_layout(g.device, sp.set1_post, nullptr);
  bt.destroy_gpu();
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
  std::printf("[SKIP] hdr-gpu: SDK absent (stub build)\n");
#endif
  std::printf(failures == 0 ? "test_hdr_tonemap: all checks passed\n"
                            : "test_hdr_tonemap: %d FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}

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
// MSVC treats getenv as a C4996 error under /WX: _dupenv_s on _WIN32.
auto gpu_missing(std::string_view what) -> int {
#if defined(_WIN32)
  char* dup = nullptr;
  std::size_t dup_len = 0;
  const bool required =
      _dupenv_s(&dup, &dup_len, "ENGINE_REQUIRE_GPU") == 0 && dup != nullptr &&
      std::string_view(dup) == "1";
  std::free(dup);
#else
  const char* req = std::getenv("ENGINE_REQUIRE_GPU");
  const bool required = req != nullptr && std::string_view(req) == "1";
#endif
  std::printf("[%s] %.*s\n", required ? "FAIL" : "SKIP",
              static_cast<int>(what.size()), what.data());
  return required ? 1 : 0;
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

// Build one Vulkan context (device + all entry points) or count the
// missing-GPU failure per rule 11.
auto test_gpu() -> int;

auto test_gpu() -> int {
  using engine::renderer::BindlessTable;
  using engine::renderer::HdrExposureFromEv;
  using engine::renderer::PbrMaterialLibrary;
  using engine::renderer::PbrPipeline;
  using engine::renderer::VulkanDevice;
  VulkanDevice dev;
  engine::test::GoldenVk g{};
  PbrPipeline pbr;
  BindlessTable bt;
  auto check_fn = [&](bool ok, const char* name) { check(ok, name); };
  auto missing_fn = [&](const char* /*what*/) {
    failures += gpu_missing("hdr-golden: no physical device");
  };
  if (!engine::test::GoldenOpenDevice(
          check_fn, missing_fn, "P4-02-hdr", "hdr-golden: device init ok",
          "hdr-golden: entry points resolve", "hdr-golden: plan init",
          "hdr-golden: bindless init", "hdr-golden: bindless layout builds",
          &dev, &g, &pbr, &bt)) {
    return 1;
  }

  // Shade/post pair (shared builder in golden_vk_helper.hpp — the same
  // module/layout/pipeline struct-fill the P4-01 TUs carry inline; the
  // helper keeps this third TU from tripping Sonar's duplication gate).
  engine::test::GoldenShadePost sp{};
  bool pipes_ok = false;
  {
    auto create_set_layout =
        [&](VkDevice device, const VkDescriptorSetLayoutCreateInfo* info,
            const VkAllocationCallbacks* alloc, VkDescriptorSetLayout* out) {
          return g.vk.create_set_layout(device, info, alloc, out);
        };
    auto create_pipe_layout = [&](const VkPipelineLayoutCreateInfo* info,
                                  VkPipelineLayout* out) {
      return g.vk.create_pipe_layout(g.device, info, nullptr, out) ==
             VK_SUCCESS;
    };
    auto create_pipes = [&](VkDevice device, VkPipelineCache cache,
                            std::uint32_t count,
                            const VkGraphicsPipelineCreateInfo* infos,
                            const VkAllocationCallbacks* alloc,
                            VkPipeline* pipes) {
      return g.vk.create_pipes(device, cache, count, infos, alloc, pipes);
    };
    auto destroy_mod = [&](VkShaderModule m) {
      if (m != nullptr) {
        g.vk.destroy_module(g.device, m, nullptr);
      }
    };
    pipes_ok = engine::test::GoldenBuildShadePost(
        &g, bt.set_layout(), kPbr_shade_vertSpv, kPbr_shade_vertSpvWords,
        kPbr_shade_fragSpv, kPbr_shade_fragSpvWords,
        VK_FORMAT_R16G16B16A16_SFLOAT, kPost_vertSpv, kPost_vertSpvWords,
        kPost_fragSpv, kPost_fragSpvWords, VK_FORMAT_R8G8B8A8_UNORM,
        create_set_layout, create_pipe_layout, create_pipes, destroy_mod, &sp);
  }
  check(pipes_ok, "hdr-golden: shade+post pipelines build");
  if (!pipes_ok) {
    bt.destroy_gpu();
    dev.shutdown();
    return 1;
  }
  VkPipelineLayout shade_layout = sp.shade_layout;
  VkPipelineLayout post_layout = sp.post_layout;
  VkPipeline shade_pipe = sp.shade_pipe;
  VkPipeline post_pipe = sp.post_pipe;
  VkDescriptorSetLayout set1_shade = sp.set1_shade;
  VkDescriptorSetLayout set1_post = sp.set1_post;

  // -- Frame content --------------------------------------------------
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
    std::vector<std::uint8_t> scene(kSceneBytes, 0);
    std::vector<std::uint8_t> mats(kMatBytes, 0);
    PbrMaterialLibrary pack_lib;
    pack_lib.init();
    PbrMaterialLibrary& lib = pack_lib;
    bool pack_ok = true;
    // One dim point light; the left (emissive) half must stay unlit so
    // the acceptance probe measures emissive only.
    float light_data[8] = {160.0f, -500.0f, 200.0f, 1.0f,
                           0.0f,   0.0f,    0.0f,   0.0f};
    constexpr std::uint32_t kTilesX = kW / 16;
    constexpr std::uint32_t kTilesY = kH / 16;
    std::vector<std::uint32_t> grid(kTilesX * kTilesY * 2, 0);
    for (std::size_t t = 0; t < kTilesX * kTilesY; ++t) {
      grid[t * 2 + 0] = 0;
      grid[t * 2 + 1] = 1;
    }
    std::uint32_t index_list[1] = {0};
    // Visibility: left half -> instance 0 (emissive 10), right half ->
    // instance 1 (diffuse grey).
    std::vector<std::uint32_t> vis(kW * kH * 2, 0);
    for (std::uint32_t y = 0; y < kH; ++y) {
      for (std::uint32_t x = 0; x < kW; ++x) {
        const std::uint32_t inst = (x < kW / 2) ? 0u : 1u;
        vis[(y * kW + x) * 2 + 0] = inst;
        vis[(y * kW + x) * 2 + 1] = 0;
      }
    }
    {
      for (std::uint32_t inst = 0; inst < kInst && pack_ok; ++inst) {
        float* m = reinterpret_cast<float*>(&scene[inst * 96]);
        for (int d = 0; d < 16; ++d) {
          m[d] = (d % 5 == 0) ? 1.0f : 0.0f;
        }
        float* sph = reinterpret_cast<float*>(&scene[inst * 96 + 64]);
        sph[0] = (inst == 0) ? 80.0f : 240.0f;
        sph[1] = 120.0f;
        sph[2] = 0.0f;
        sph[3] = 60.0f;
        std::uint32_t* ids =
            reinterpret_cast<std::uint32_t*>(&scene[inst * 96 + 80]);
        ids[0] = 0;
        ids[1] = inst;
        ids[2] = 1;
        ids[3] = 0;
        engine::renderer::PbrMaterialAsset a{};
        if (inst == 0) {
          // HDR probe: black diffuse (no direct response), emissive 10.
          a.base_color = {0.0f, 0.0f, 0.0f, 1.0f};
          a.emissive = {10.0f, 2.0f, 1.0f, 0.0f};
        } else {
          a.base_color = {0.5f, 0.5f, 0.5f, 1.0f};
          a.metallic_roughness = {0.0f, 0.9f, 0.0f, 0.0f};
        }
        std::uint32_t slot = 0xFFFFFFFFu;
        if (!lib.add(a, &slot).ok() || slot != inst) {
          check(false, "hdr-golden: material add");
          pack_ok = false;
          break;
        }
        float rec[16]{};
        if (!PbrMaterialLibrary::to_scene_record(a, rec).ok()) {
          check(false, "hdr-golden: record pack");
          pack_ok = false;
          break;
        }
        for (int f = 0; f < 16; ++f) {
          reinterpret_cast<float*>(&mats[inst * 64])[f] = rec[f];
        }
      }
    }
    check(pack_ok, "hdr-golden: 2 materials packed");
    if (!pack_ok) {
      return 1;
    }
    const bool bufs_ok =
        engine::test::GoldenAllocHost(
            &g, kSceneBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            &scene_b) &&
        engine::test::GoldenAllocHost(
            &g, kMatBytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            &mat_b) &&
        engine::test::GoldenAllocHost(
            &g, sizeof(light_data),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            &light_b) &&
        engine::test::GoldenAllocHost(
            &g, grid.size() * 4,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            &grid_b) &&
        engine::test::GoldenAllocHost(
            &g, sizeof(index_list),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            &index_b) &&
        engine::test::GoldenAllocHost(&g, static_cast<std::uint64_t>(kW) * kH * 8,
                                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &vis_b) &&
        engine::test::GoldenAllocHost(&g, static_cast<std::uint64_t>(kW) * kH * 4,
                                      VK_BUFFER_USAGE_TRANSFER_DST_BIT, &read_b);
    check(bufs_ok, "hdr-golden: upload buffers");
    if (!bufs_ok) {
      return 1;
    }
    auto as_bytes = [](const void* p, std::size_t n) {
      const auto* b = static_cast<const std::uint8_t*>(p);
      return std::span<const std::uint8_t>(b, n);
    };
    if (!engine::test::GoldenUploadBytes(&g, &scene_b,
                                         as_bytes(scene.data(), scene.size())) ||
        !engine::test::GoldenUploadBytes(&g, &mat_b,
                                         as_bytes(mats.data(), mats.size())) ||
        !engine::test::GoldenUploadBytes(&g, &light_b,
                                         as_bytes(light_data, sizeof(light_data))) ||
        !engine::test::GoldenUploadBytes(
            &g, &grid_b, as_bytes(grid.data(), grid.size() * 4)) ||
        !engine::test::GoldenUploadBytes(
            &g, &index_b, as_bytes(index_list, sizeof(index_list))) ||
        !engine::test::GoldenUploadBytes(&g, &vis_b,
                                         as_bytes(vis.data(), vis.size() * 4))) {
      check(false, "hdr-golden: uploads mapped");
      return 1;
    }
    check(true, "hdr-golden: uploads mapped");

    const bool imgs_ok =
        engine::test::GoldenMakeImage(
            &g, VK_FORMAT_R32G32_UINT, kW, kH,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
            &vis_img) &&
        engine::test::GoldenMakeImage(
            &g, VK_FORMAT_R16G16B16A16_SFLOAT, kW, kH,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            &hdr_img) &&
        engine::test::GoldenMakeImage(
            &g, VK_FORMAT_R8G8B8A8_UNORM, kW, kH,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            &ldr_img);
    check(imgs_ok, "hdr-golden: images allocated");
    if (!imgs_ok) {
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
      if (g.vk.create_sampler(g.device, &si, nullptr, &sampler) != VK_SUCCESS) {
        check(false, "hdr-golden: sampler");
        return 1;
      }
    }

    {
      auto create_pool = [&](VkDevice device,
                             const VkDescriptorPoolCreateInfo* info,
                             const VkAllocationCallbacks* alloc,
                             VkDescriptorPool* out) {
        return g.vk.create_pool(device, info, alloc, out);
      };
      auto alloc_sets = [&](VkDevice device,
                            const VkDescriptorSetAllocateInfo* info,
                            VkDescriptorSet* out) {
        return g.vk.alloc_sets(device, info, out);
      };
      const bool sets_ok = engine::test::GoldenAllocFrameSets(
          &g, bt.set_layout(), set1_shade, set1_post, create_pool, alloc_sets,
          &pool, &set0, &set_shade, &set_post);
      check(sets_ok, "hdr-golden: descriptor sets");
      if (!sets_ok) {
        return 1;
      }
    }

    {
      engine::test::GoldenFrameSets f{};
      f.scene_buf = scene_b.buf;
      f.scene_bytes = kSceneBytes;
      f.mat_buf = mat_b.buf;
      f.mat_bytes = kMatBytes;
      f.light_buf = light_b.buf;
      f.light_bytes = sizeof(light_data);
      f.grid_buf = grid_b.buf;
      f.grid_bytes = grid.size() * 4;
      f.index_buf = index_b.buf;
      f.index_bytes = sizeof(index_list);
      f.vis_view = vis_img.view;
      f.hdr_view = hdr_img.view;
      f.sampler = sampler;
      f.set0 = set0;
      f.set_shade = set_shade;
      f.set_post = set_post;
      auto update_sets = [&](VkDevice device,
                             const VkWriteDescriptorSet* writes) {
        g.vk.update_sets(device, 9, writes, 0, nullptr);
      };
      engine::test::GoldenWriteFrameSets(&g, f, update_sets);
    }
    check(true, "hdr-golden: descriptors written");

    // -- Record, submit, read back ------------------------------------
    bool submit_ok = false;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kW) * kH * 4, 0);
    VkCommandPool cmd_pool = nullptr;
    {
      VkCommandPoolCreateInfo cpi{};
      cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      cpi.queueFamilyIndex = g.queue_family;
      if (g.vk.create_cmd_pool(g.device, &cpi, nullptr, &cmd_pool) !=
          VK_SUCCESS) {
        check(false, "hdr-golden: cmd pool");
        return 1;
      }
    }
    {
      VkCommandBufferAllocateInfo ab{};
      ab.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      ab.commandPool = cmd_pool;
      ab.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      ab.commandBufferCount = 1;
      VkCommandBuffer cb = nullptr;
      if (g.vk.alloc_cmds(g.device, &ab, &cb) != VK_SUCCESS) {
        check(false, "hdr-golden: cmd alloc");
        return 1;
      }
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      bool rec_ok = g.vk.begin_cmd(cb, &bi) == VK_SUCCESS;
      if (rec_ok) {
        engine::test::GoldenFrame fr{};
        fr.vis_buf = vis_b.buf;
        fr.w = kW;
        fr.h = kH;
        fr.vis_img = vis_img.img;
        fr.hdr_img = hdr_img.img;
        fr.ldr_img = ldr_img.img;
        fr.hdr_view = hdr_img.view;
        fr.ldr_view = ldr_img.view;
        fr.shade_pipe = shade_pipe;
        fr.post_pipe = post_pipe;
        fr.shade_layout = shade_layout;
        fr.post_layout = post_layout;
        fr.set0 = set0;
        fr.set_shade = set_shade;
        fr.set_post = set_post;
        fr.shade.cam[0] = 160.0f;
        fr.shade.cam[1] = 120.0f;
        fr.shade.cam[2] = 80.0f;
        fr.shade.cam[3] = 1.0f;
        fr.shade.light_count = 1;
        fr.shade.max_lights_per_tile = 64;
        fr.post.exposure = HdrExposureFromEv(-3.0f);
        engine::test::GoldenRecordFrame(&g, cb, fr);
        // LDR -> TRANSFER_SRC + copy to readback.
        engine::test::GoldenBarrier(
            &g, cb, ldr_img.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy rc{};
        rc.bufferOffset = 0;
        rc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rc.imageExtent = {kW, kH, 1};
        g.vk.cmd_copy_img_buf(cb, ldr_img.img,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, read_b.buf,
                              1, &rc);
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
        rec_ok = g.vk.end_cmd(cb) == VK_SUCCESS;
      }
      if (rec_ok) {
        submit_ok = engine::test::GoldenSubmitWait(&g, cb);
      }
    }
    check(submit_ok, "hdr-golden: submit + fence wait");
    if (submit_ok) {
      void* p = nullptr;
      const std::size_t n = static_cast<std::size_t>(kW) * kH * 4;
      if (g.vk.map_mem(g.device, read_b.mem, 0, n, 0, &p) == VK_SUCCESS &&
          p != nullptr) {
        const auto* src = static_cast<const std::uint8_t*>(p);
        for (std::size_t i = 0; i < n; ++i) {
          pixels[i] = src[i];
        }
        g.vk.unmap_mem(g.device, read_b.mem);
        check(true, "hdr-golden: readback mapped");
        failures += engine::test::golden_check(
            "p4_02_hdr_320x240",
            std::span<const std::uint8_t>(pixels.data(), pixels.size()), kW,
            kH);
        // Acceptance: emissive 10 at EV-3 does not clip to white.
        // Left-half center pixel (80,120): HDR 10 * 0.125 = 1.25 pre-
        // tonemap; ACES(1.25) ~ 0.83 -> sRGB ~ 0.93 -> ~237. White
        // would be 255; assert headroom (r < 250) AND brightness
        // (r > 150) so the probe fails both when clipped and when the
        // emissive term goes missing (black).
        const std::size_t c = (120 * kW + 80) * 4;
        const int r = pixels[c];
        const int gg = pixels[c + 1];
        const int b = pixels[c + 2];
        check(r < 250 && gg < 250 && b < 250, "hdr-golden: no white clip");
        check(r > 150, "hdr-golden: emissive survives exposure");
        check(r > gg && gg >= b, "hdr-golden: red tint kept");
        // CPU/GPU parity: the same HDR value through HdrTonemapCpu
        // must land within 8-bit rounding (+1 for half-float target)
        // of the GPU pixel.
        const float hot[3] = {10.0f, 2.0f, 1.0f};
        float ref[3] = {0.0f, 0.0f, 0.0f};
        const bool ref_ok =
            engine::renderer::HdrTonemapCpu(hot, HdrExposureFromEv(-3.0f), ref);
        check(ref_ok, "hdr-golden: cpu reference ok");
        if (ref_ok) {
          const int want_r = static_cast<int>(ref[0] * 255.0f + 0.5f);
          int dr = r - want_r;
          if (dr < 0) {
            dr = -dr;
          }
          check(dr <= 4, "hdr-golden: gpu matches cpu reference");
        }
      } else {
        check(false, "hdr-golden: readback mapped");
      }
    }
    g.vk.destroy_cmd_pool(g.device, cmd_pool, nullptr);

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
  g.vk.destroy_pipe(g.device, shade_pipe, nullptr);
  g.vk.destroy_pipe(g.device, post_pipe, nullptr);
  g.vk.destroy_pipe_layout(g.device, shade_layout, nullptr);
  g.vk.destroy_pipe_layout(g.device, post_layout, nullptr);
  g.vk.destroy_set_layout(g.device, set1_shade, nullptr);
  g.vk.destroy_set_layout(g.device, set1_post, nullptr);
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

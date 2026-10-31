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

namespace {

auto failures = 0;

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}

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

#if defined(ENGINE_RENDERER_HAS_VULKAN)

constexpr std::uint32_t kW = 320;
constexpr std::uint32_t kH = 240;
constexpr std::uint32_t kInst = 2;

// P4-02 CPU contract: EV->exposure, tonemap reference, component size.
// No GPU needed; runs on stub builds too.
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

// Build one Vulkan context (device + all entry points) or count the
// missing-GPU failure per rule 11.
auto test_gpu() -> int;

auto test_gpu() -> int {
  using engine::renderer::BindlessTable;
  using engine::renderer::DeviceCreateInfo;
  using engine::renderer::HdrExposureFromEv;
  using engine::renderer::PbrMaterialLibrary;
  using engine::renderer::PbrPipeline;
  using engine::renderer::StatusCode;
  using engine::renderer::VulkanDevice;
  VulkanDevice dev;
  DeviceCreateInfo ci{};
  ci.app_name = "P4-02-hdr";
  ci.device_hint = "llvmpipe";
  const auto st = dev.init(ci);
  if (!st.ok() && st.code == StatusCode::kNoPhysicalDevice) {
    failures += gpu_missing("hdr-golden: no physical device");
    return 1;
  }
  check(st.ok(), "hdr-golden: device init ok");
  if (!st.ok()) {
    return 1;
  }
  engine::test::GoldenVk g{};
  g.device = dev.device();
  g.physical = dev.physical_device();
  g.queue = dev.graphics_queue();
  g.queue_family = dev.graphics_queue_family();
  auto loader = [&](const char* name) -> void* {
    void* p = dev.proc(name);
    return (p != nullptr) ? p : dev.instance_proc(name);
  };
  check(engine::test::GoldenVkLoad(loader, &g.vk),
        "hdr-golden: entry points resolve");
  if (g.vk.create_buffer == nullptr || g.vk.create_pipes == nullptr ||
      g.vk.queue_submit == nullptr) {
    dev.shutdown();
    return 1;
  }

  PbrPipeline pbr;
  check(pbr
            .init(kPbr_vis_vertSpv, kPbr_vis_vertSpvWords, kPbr_vis_fragSpv,
                  kPbr_vis_fragSpvWords, kPbr_shade_vertSpv,
                  kPbr_shade_vertSpvWords, kPbr_shade_fragSpv,
                  kPbr_shade_fragSpvWords, kPost_vertSpv, kPost_vertSpvWords,
                  kPost_fragSpv, kPost_fragSpvWords)
            .ok(),
        "hdr-golden: plan init");
  BindlessTable bt;
  check(bt.init().ok(), "hdr-golden: bindless init");
  VkBuffer sentinel = reinterpret_cast<VkBuffer>(0x1000);
  check(bt.create_gpu(dev.device(), sentinel, 1 << 20, sentinel, 1 << 16)
            .ok(),
        "hdr-golden: bindless layout builds");
  if (!pbr.ready() || !bt.gpu_ready()) {
    bt.destroy_gpu();
    dev.shutdown();
    return 1;
  }

  VkShaderModule shade_v = nullptr;
  VkShaderModule shade_f = nullptr;
  VkShaderModule post_v = nullptr;
  VkShaderModule post_f = nullptr;
  const bool mods_ok =
      engine::test::GoldenMakeModule(&g, kPbr_shade_vertSpv,
                                     kPbr_shade_vertSpvWords, &shade_v) &&
      engine::test::GoldenMakeModule(&g, kPbr_shade_fragSpv,
                                     kPbr_shade_fragSpvWords, &shade_f) &&
      engine::test::GoldenMakeModule(&g, kPost_vertSpv, kPost_vertSpvWords,
                                     &post_v) &&
      engine::test::GoldenMakeModule(&g, kPost_fragSpv, kPost_fragSpvWords,
                                     &post_f);
  check(mods_ok, "hdr-golden: modules created");
  if (!mods_ok) {
    for (auto* m : {&shade_v, &shade_f, &post_v, &post_f}) {
      if (*m != nullptr) {
        g.vk.destroy_module(g.device, *m, nullptr);
      }
    }
    bt.destroy_gpu();
    dev.shutdown();
    return 1;
  }

  // Set-1 layouts (shared builder with the P4-01 TUs).
  VkDescriptorSetLayout set1_shade = nullptr;
  VkDescriptorSetLayout set1_post = nullptr;
  bool layouts_ok = true;
  {
    auto create_set_layout =
        [&](VkDevice device, const VkDescriptorSetLayoutCreateInfo* info,
            const VkAllocationCallbacks* alloc, VkDescriptorSetLayout* out) {
          return g.vk.create_set_layout(device, info, alloc, out);
        };
    layouts_ok = engine::test::GoldenBuildSet1(
        create_set_layout, g.device, &set1_shade, &set1_post);
  }
  check(layouts_ok, "hdr-golden: set-1 layouts");
  VkPipelineLayout shade_layout = nullptr;
  VkPipelineLayout post_layout = nullptr;
  bool plo_ok = layouts_ok;
  if (plo_ok) {
    VkDescriptorSetLayout sets[2] = {bt.set_layout(), set1_shade};
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset = 0;
    push.size = 64;  // PbrShadePush
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 2;
    pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    plo_ok = g.vk.create_pipe_layout(g.device, &pli, nullptr,
                                     &shade_layout) == VK_SUCCESS;
    VkDescriptorSetLayout psets[2] = {bt.set_layout(), set1_post};
    VkPushConstantRange ppush{};
    ppush.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    ppush.offset = 0;
    ppush.size = 20;  // PostPush
    VkPipelineLayoutCreateInfo ppli{};
    ppli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ppli.setLayoutCount = 2;
    ppli.pSetLayouts = psets;
    ppli.pushConstantRangeCount = 1;
    ppli.pPushConstantRanges = &ppush;
    plo_ok = plo_ok && g.vk.create_pipe_layout(g.device, &ppli, nullptr,
                                              &post_layout) == VK_SUCCESS;
  }
  check(plo_ok, "hdr-golden: pipeline layouts");
  VkPipeline shade_pipe = nullptr;
  VkPipeline post_pipe = nullptr;
  bool pipes_ok = plo_ok;
  if (pipes_ok) {
    auto mk = [&](VkShaderModule v, VkShaderModule f, VkFormat fmt,
                  VkPipelineLayout layout, VkPipeline* out) -> bool {
      auto create_pipes = [&](VkDevice device, VkPipelineCache cache,
                              std::uint32_t count,
                              const VkGraphicsPipelineCreateInfo* infos,
                              const VkAllocationCallbacks* alloc,
                              VkPipeline* pipes) {
        return g.vk.create_pipes(device, cache, count, infos, alloc, pipes);
      };
      return engine::test::GoldenBuildPipe(create_pipes, g.device, v, f, fmt,
                                           layout, out);
    };
    pipes_ok =
        mk(shade_v, shade_f, VK_FORMAT_R16G16B16A16_SFLOAT, shade_layout,
           &shade_pipe) &&
        mk(post_v, post_f, VK_FORMAT_R8G8B8A8_UNORM, post_layout, &post_pipe);
  }
  check(pipes_ok, "hdr-golden: pipelines build");
  for (auto* m : {&shade_v, &shade_f, &post_v, &post_f}) {
    g.vk.destroy_module(g.device, *m, nullptr);
    *m = nullptr;
  }
  if (!pipes_ok) {
    if (shade_pipe != nullptr) {
      g.vk.destroy_pipe(g.device, shade_pipe, nullptr);
    }
    if (post_pipe != nullptr) {
      g.vk.destroy_pipe(g.device, post_pipe, nullptr);
    }
    if (shade_layout != nullptr) {
      g.vk.destroy_pipe_layout(g.device, shade_layout, nullptr);
    }
    if (post_layout != nullptr) {
      g.vk.destroy_pipe_layout(g.device, post_layout, nullptr);
    }
    g.vk.destroy_set_layout(g.device, set1_shade, nullptr);
    g.vk.destroy_set_layout(g.device, set1_post, nullptr);
    bt.destroy_gpu();
    dev.shutdown();
    return 1;
  }

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
      VkDescriptorPoolSize sizes[5]{};
      sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      sizes[0].descriptorCount = 5;
      sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      sizes[1].descriptorCount = 1;
      sizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLER;
      sizes[2].descriptorCount = 2;
      sizes[3].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      sizes[3].descriptorCount = 1;
      sizes[4].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      sizes[4].descriptorCount = 1;
      VkDescriptorPoolCreateInfo pi{};
      pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
      pi.maxSets = 3;
      pi.poolSizeCount = 5;
      pi.pPoolSizes = sizes;
      bool sets_ok = false;
      if (g.vk.create_pool(g.device, &pi, nullptr, &pool) == VK_SUCCESS) {
        VkDescriptorSetLayout layouts[3] = {bt.set_layout(), set1_shade,
                                            set1_post};
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 3;
        ai.pSetLayouts = layouts;
        VkDescriptorSet sets[3]{nullptr, nullptr, nullptr};
        if (g.vk.alloc_sets(g.device, &ai, sets) == VK_SUCCESS) {
          set0 = sets[0];
          set_shade = sets[1];
          set_post = sets[2];
          sets_ok = true;
        }
      }
      check(sets_ok, "hdr-golden: descriptor sets");
      if (!sets_ok) {
        return 1;
      }
    }

    {
      VkDescriptorBufferInfo scene_info{scene_b.buf, 0, kSceneBytes};
      VkDescriptorBufferInfo mat_info{mat_b.buf, 0, kMatBytes};
      VkDescriptorBufferInfo light_info{light_b.buf, 0, sizeof(light_data)};
      VkDescriptorBufferInfo grid_info{grid_b.buf, 0, grid.size() * 4};
      VkDescriptorBufferInfo index_info{index_b.buf, 0, sizeof(index_list)};
      VkDescriptorImageInfo vis_info{};
      vis_info.imageView = vis_img.view;
      vis_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      VkDescriptorImageInfo hdr_info{};
      hdr_info.imageView = hdr_img.view;
      hdr_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      VkDescriptorImageInfo post_smp{};
      post_smp.sampler = sampler;
      VkDescriptorImageInfo shade_dummy_comb{};
      shade_dummy_comb.imageView = hdr_img.view;
      shade_dummy_comb.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      shade_dummy_comb.sampler = sampler;
      VkWriteDescriptorSet w[10]{};
      for (int i = 0; i < 10; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      }
      w[0].dstSet = set0;
      w[0].dstBinding = 0;
      w[0].descriptorCount = 1;
      w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      w[0].pBufferInfo = &scene_info;
      w[1].dstSet = set0;
      w[1].dstBinding = 1;
      w[1].descriptorCount = 1;
      w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      w[1].pBufferInfo = &mat_info;
      w[2].dstSet = set_shade;
      w[2].dstBinding = 0;
      w[2].descriptorCount = 1;
      w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      w[2].pImageInfo = &vis_info;
      w[3].dstSet = set_shade;
      w[3].dstBinding = 3;
      w[3].descriptorCount = 1;
      w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      w[3].pBufferInfo = &light_info;
      w[4].dstSet = set_shade;
      w[4].dstBinding = 4;
      w[4].descriptorCount = 1;
      w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      w[4].pBufferInfo = &grid_info;
      w[5].dstSet = set_shade;
      w[5].dstBinding = 5;
      w[5].descriptorCount = 1;
      w[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      w[5].pBufferInfo = &index_info;
      w[6].dstSet = set_shade;
      w[6].dstBinding = 2;
      w[6].descriptorCount = 1;
      w[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      w[6].pImageInfo = &shade_dummy_comb;
      w[7].dstSet = set_post;
      w[7].dstBinding = 0;
      w[7].descriptorCount = 1;
      w[7].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      w[7].pImageInfo = &hdr_info;
      w[8].dstSet = set_post;
      w[8].dstBinding = 1;
      w[8].descriptorCount = 1;
      w[8].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
      w[8].pImageInfo = &post_smp;
      g.vk.update_sets(g.device, 9, w, 0, nullptr);
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
        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {kW, kH, 1};
        engine::test::GoldenBarrier(
            &g, cb, vis_img.img, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        g.vk.cmd_copy_buf_img(cb, vis_b.buf, vis_img.img,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        engine::test::GoldenBarrier(
            &g, cb, vis_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
        for (auto* im : {&hdr_img, &ldr_img}) {
          engine::test::GoldenBarrier(
              &g, cb, im->img, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        }
        // Shade pass -> HDR.
        VkRenderingAttachmentInfo shade_att{};
        shade_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        shade_att.imageView = hdr_img.view;
        shade_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        shade_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        shade_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        shade_att.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        VkRenderingInfo shade_ri{};
        shade_ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        shade_ri.renderArea = {{0, 0}, {kW, kH}};
        shade_ri.layerCount = 1;
        shade_ri.colorAttachmentCount = 1;
        shade_ri.pColorAttachments = &shade_att;
        g.vk.cmd_begin_render(cb, &shade_ri);
        VkViewport vp{0, 0, static_cast<float>(kW), static_cast<float>(kH),
                      0.0f, 1.0f};
        g.vk.cmd_viewport(cb, 0, 1, &vp);
        VkRect2D sc{{0, 0}, {kW, kH}};
        g.vk.cmd_scissor(cb, 0, 1, &sc);
        g.vk.cmd_bind_pipe(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, shade_pipe);
        VkDescriptorSet shade_sets[2] = {set0, set_shade};
        g.vk.cmd_bind_sets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, shade_layout,
                           0, 2, shade_sets, 0, nullptr);
        struct ShadePush {
          float cam[4];
          float frame[4];
          float inv[4];
          std::uint32_t lc;
          std::uint32_t flags;
          std::uint32_t max_tile;
          std::uint32_t pad;
        };
        ShadePush sp{};
        sp.cam[0] = 160.0f;
        sp.cam[1] = 120.0f;
        sp.cam[2] = 80.0f;
        sp.cam[3] = 1.0f;
        sp.frame[0] = static_cast<float>(kW);
        sp.frame[1] = static_cast<float>(kH);
        sp.frame[2] = 1.0f;
        sp.frame[3] = 1.0f;
        sp.inv[0] = 1.0f / static_cast<float>(kW);
        sp.inv[1] = 1.0f / static_cast<float>(kH);
        sp.inv[2] = 1.0f;
        sp.inv[3] = 1.0f;
        sp.lc = 1;
        sp.max_tile = 64;
        g.vk.cmd_push(cb, shade_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                      sizeof(sp), &sp);
        g.vk.cmd_draw(cb, 3, 1, 0, 0);
        g.vk.cmd_end_render(cb);
        // HDR -> SHADER_READ for the post pass.
        engine::test::GoldenBarrier(
            &g, cb, hdr_img.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT);
        // Post pass -> LDR (EV-3 exposure from the camera component).
        VkRenderingAttachmentInfo post_att{};
        post_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        post_att.imageView = ldr_img.view;
        post_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        post_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        post_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        post_att.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        VkRenderingInfo post_ri{};
        post_ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        post_ri.renderArea = {{0, 0}, {kW, kH}};
        post_ri.layerCount = 1;
        post_ri.colorAttachmentCount = 1;
        post_ri.pColorAttachments = &post_att;
        g.vk.cmd_begin_render(cb, &post_ri);
        g.vk.cmd_viewport(cb, 0, 1, &vp);
        g.vk.cmd_scissor(cb, 0, 1, &sc);
        g.vk.cmd_bind_pipe(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, post_pipe);
        VkDescriptorSet post_sets[2] = {set0, set_post};
        g.vk.cmd_bind_sets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, post_layout,
                           0, 2, post_sets, 0, nullptr);
        struct PostPush {
          float exposure;
          std::uint32_t flags;
          std::uint32_t p0;
          std::uint32_t p1;
          std::uint32_t p2;
        };
        PostPush pp{};
        pp.exposure = HdrExposureFromEv(-3.0f);
        g.vk.cmd_push(cb, post_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                      sizeof(pp), &pp);
        g.vk.cmd_draw(cb, 3, 1, 0, 0);
        g.vk.cmd_end_render(cb);
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

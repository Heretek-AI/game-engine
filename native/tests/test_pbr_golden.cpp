// test_pbr_golden.cpp — P4-01 sphere-grid golden (own TU, own main).
//
// Renders the 5x5 material grid (metallic x, roughness y) through the
// REAL shade + post pipelines into an offscreen RGBA8 image and compares
// against golden p4_01_spheres_320x240 via golden_compare.hpp (P1-05
// tolerance). No visibility rasterization: the visibility image uploads
// from the CPU (each 64x48 tile -> its instance id), so the shade pass
// reads exactly what a rasterizer would have produced. One submit,
// fence-waited; [FAIL] (not [SKIP]) under ENGINE_REQUIRE_GPU=1.
//
// Check naming mirrors test_slang_pbr.cpp (own TU, no shared test lib).
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

namespace {

auto failures = 0;


#if defined(ENGINE_RENDERER_HAS_VULKAN)

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}

// [FAIL]-not-[SKIP] under ENGINE_REQUIRE_GPU=1 (rule 11 / P1-01 contract).
// Shared helper (golden_vk_helper.hpp) so the 3 golden TUs don't each
// carry the 16-line getenv fallback (Sonar duplication gate).
auto gpu_missing(std::string_view what) -> int {
  return engine::test::GoldenGpuMissing(what);
}

constexpr std::uint32_t kW = 320;
constexpr std::uint32_t kH = 240;
constexpr std::uint32_t kGrid = 5;  // 5x5 materials
constexpr std::uint32_t kInst = kGrid * kGrid;

// P4-02 shared scaffolding (golden_vk_helper.hpp): loader procs, host
// buffer + image allocation, module creation, barriers, submit-wait, and
// the shade/post pipeline pair. GoldenVkProcs/GoldenVk replace the former
// TU-local Procs/load_procs (~150 lines, Sonar duplication gate);
// GoldenCtx/alloc_host/free_host/upload_bytes/make_image/free_image are
// thin TU-local shims over the helper so call sites stay readable.
// Field names (g.vk.*, g.dev->device()) stay TU-local.
//
// NOTE: GoldenOpenDevice is NOT used here: it pins the HDR shade/post
// formats + PbrPipeline plan init for the P4-02 frame shape, while this
// TU keeps its own device bootstrap (LDR post format, direct pbr.init
// call). Sharing stops at GoldenVk*/GoldenAllocHost/GoldenMakeImage;
// the bootstrap difference is intentional, not drift.

struct GoldenCtx {
  const engine::renderer::VulkanDevice* dev{nullptr};
  engine::test::GoldenVk vk{};
};

struct HostBuf {
  VkBuffer buf{nullptr};
  VkDeviceMemory mem{nullptr};
};

struct Img {
  VkImage img{nullptr};
  VkDeviceMemory mem{nullptr};
  VkImageView view{nullptr};
};

void load_procs(const engine::renderer::VulkanDevice& dev,
                engine::test::GoldenVk* gv) {
  gv->device = dev.device();
  gv->physical = dev.physical_device();
  gv->queue = dev.graphics_queue();
  gv->queue_family = dev.graphics_queue_family();
  auto loader = [&](const char* name) -> void* {
    void* p = dev.proc(name);
    return (p != nullptr) ? p : dev.instance_proc(name);
  };
  engine::test::GoldenVkLoad(loader, &gv->vk);
}

auto alloc_host(GoldenCtx* g, std::uint64_t bytes, VkBufferUsageFlags usage,
                HostBuf* out) -> bool {
  engine::test::GoldenHostBuf hb{};
  if (!engine::test::GoldenAllocHost(&g->vk, bytes, usage, &hb)) {
    return false;
  }
  out->buf = hb.buf;
  out->mem = hb.mem;
  return true;
}

void free_host(GoldenCtx* g, HostBuf* hb) {
  engine::test::GoldenHostBuf wrapped{hb->buf, hb->mem};
  engine::test::GoldenFreeHost(&g->vk, &wrapped);
  hb->buf = nullptr;
  hb->mem = nullptr;
}

auto upload_bytes(GoldenCtx* g, HostBuf* hb, const void* src, std::size_t n)
    -> bool {
  void* p = nullptr;
  if (g->vk.vk.map_mem(g->dev->device(), hb->mem, 0, n, 0, &p) != VK_SUCCESS ||
      p == nullptr) {
    return false;
  }
  const auto* b = static_cast<const std::uint8_t*>(src);
  auto* d = static_cast<std::uint8_t*>(p);
  for (std::size_t i = 0; i < n; ++i) {
    d[i] = b[i];
  }
  g->vk.vk.unmap_mem(g->dev->device(), hb->mem);
  return true;
}

auto make_image(GoldenCtx* g, VkFormat fmt, VkImageUsageFlags usage, Img* out)
    -> bool {
  engine::test::GoldenImage gi{};
  if (!engine::test::GoldenMakeImage(&g->vk, fmt, kW, kH, usage, &gi)) {
    return false;
  }
  out->img = gi.img;
  out->mem = gi.mem;
  out->view = gi.view;
  return true;
}

void free_image(GoldenCtx* g, Img* im) {
  engine::test::GoldenImage gi{im->img, im->mem, im->view};
  engine::test::GoldenFreeImage(&g->vk, &gi);
  im->img = nullptr;
  im->mem = nullptr;
  im->view = nullptr;
}

// Graphics pipelines share GoldenBuildPipe (golden_pipe_helper.hpp) — the
// fullscreen-triangle struct-fill lives there, not duplicated per TU.
auto build_pipe(GoldenCtx* g, VkShaderModule vert, VkShaderModule frag,
                VkFormat color_fmt, VkPipelineLayout layout, VkPipeline* out)
    -> bool {
  auto create_pipes = [&](VkDevice device, VkPipelineCache cache,
                          std::uint32_t count,
                          const VkGraphicsPipelineCreateInfo* infos,
                          const VkAllocationCallbacks* alloc, VkPipeline* pipes) {
    return g->vk.vk.create_pipes(device, cache, count, infos, alloc, pipes);
  };
  return engine::test::GoldenBuildPipe(create_pipes, g->dev->device(), vert,
                                       frag, color_fmt, layout, out);
}

void test_golden() {
  using engine::renderer::BindlessTable;
  using engine::renderer::DeviceCreateInfo;
  using engine::renderer::PbrMaterialLibrary;
  using engine::renderer::PbrPipeline;
  using engine::renderer::StatusCode;
  using engine::renderer::VulkanDevice;
  VulkanDevice dev;
  DeviceCreateInfo ci{};
  ci.app_name = "P4-01-golden";
  ci.device_hint = "llvmpipe";
  const auto st = dev.init(ci);
  if (!st.ok() && st.code == StatusCode::kNoPhysicalDevice) {
    failures += gpu_missing("pbr-golden: no physical device");
    return;
  }
  check(st.ok(), "pbr-golden: device init ok");
  if (!st.ok()) {
    return;
  }
  GoldenCtx g{};
  g.dev = &dev;
  load_procs(dev, &g.vk);
  check(g.vk.vk.create_buffer != nullptr && g.vk.vk.create_pipes != nullptr &&
            g.vk.vk.queue_submit != nullptr,
        "pbr-golden: entry points resolve");
  if (g.vk.vk.create_buffer == nullptr || g.vk.vk.create_pipes == nullptr ||
      g.vk.vk.queue_submit == nullptr) {
    dev.shutdown();
    return;
  }

  // PBR plan (blob validation) + bindless layout.
  PbrPipeline pbr;
  check(pbr
            .init(kPbr_vis_vertSpv, kPbr_vis_vertSpvWords, kPbr_vis_fragSpv,
                  kPbr_vis_fragSpvWords, kPbr_shade_vertSpv,
                  kPbr_shade_vertSpvWords, kPbr_shade_fragSpv,
                  kPbr_shade_fragSpvWords, kPost_vertSpv, kPost_vertSpvWords,
                  kPost_fragSpv, kPost_fragSpvWords)
            .ok(),
        "pbr-golden: plan init");
  BindlessTable bt;
  check(bt.init().ok(), "pbr-golden: bindless init");
  VkBuffer sentinel = reinterpret_cast<VkBuffer>(0x1000);
  check(bt.create_gpu(dev.device(), sentinel, 1 << 20, sentinel, 1 << 16)
            .ok(),
        "pbr-golden: bindless layout builds");
  if (!pbr.ready() || !bt.gpu_ready()) {
    bt.destroy_gpu();
    dev.shutdown();
    return;
  }

  // Shader modules (shade + post only — the golden records no geometry).
  VkShaderModule shade_v = nullptr;
  VkShaderModule shade_f = nullptr;
  VkShaderModule post_v = nullptr;
  VkShaderModule post_f = nullptr;
  auto make_mod = [&](const std::uint32_t* w, std::size_t n,
                      VkShaderModule* m) -> bool {
    return engine::test::GoldenMakeModule(&g.vk, w, n, m);
  };
  const bool mods_ok =
      make_mod(kPbr_shade_vertSpv, kPbr_shade_vertSpvWords, &shade_v) &&
      make_mod(kPbr_shade_fragSpv, kPbr_shade_fragSpvWords, &shade_f) &&
      make_mod(kPost_vertSpv, kPost_vertSpvWords, &post_v) &&
      make_mod(kPost_fragSpv, kPost_fragSpvWords, &post_f);
  check(mods_ok, "pbr-golden: modules created");
  if (!mods_ok) {
    for (auto* m : {&shade_v, &shade_f, &post_v, &post_f}) {
      if (*m != nullptr) {
        g.vk.vk.destroy_module(dev.device(), *m, nullptr);
      }
    }
    bt.destroy_gpu();
    dev.shutdown();
    return;
  }

  // Set-1 layouts: descriptor-fill shared with test_pbr_materials.cpp via
  // GoldenBuildSet1 (shade vis image[0] + albedo combined[2] + light
  // SSBOs[3/4/5]; post HDR image[0] + sampler[1]). Visibility rides shade
  // set-1 binding 0.
  VkDescriptorSetLayout set1_shade = nullptr;
  VkDescriptorSetLayout set1_post = nullptr;
  bool layouts_ok = true;
  {
    auto create_set_layout =
        [&](VkDevice device, const VkDescriptorSetLayoutCreateInfo* info,
            const VkAllocationCallbacks* alloc, VkDescriptorSetLayout* out) {
          return g.vk.vk.create_set_layout(device, info, alloc, out);
        };
    layouts_ok = engine::test::GoldenBuildSet1(
        create_set_layout, dev.device(), &set1_shade, &set1_post);
  }
  check(layouts_ok, "pbr-golden: set-1 layouts");
  // Pipeline layouts: bindless set-0 + set-1, with push ranges.
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
    plo_ok = g.vk.vk.create_pipe_layout(dev.device(), &pli, nullptr,
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
    plo_ok = plo_ok && g.vk.vk.create_pipe_layout(dev.device(), &ppli, nullptr,
                                               &post_layout) == VK_SUCCESS;
  }
  check(plo_ok, "pbr-golden: pipeline layouts");
  VkPipeline shade_pipe = nullptr;
  VkPipeline post_pipe = nullptr;
  const bool pipes_ok =
      plo_ok &&
      build_pipe(&g, shade_v, shade_f, VK_FORMAT_R16G16B16A16_SFLOAT,
                 shade_layout, &shade_pipe) &&
      build_pipe(&g, post_v, post_f, VK_FORMAT_R8G8B8A8_UNORM, post_layout,
                 &post_pipe);
  check(pipes_ok, "pbr-golden: pipelines build");
  for (auto* m : {&shade_v, &shade_f, &post_v, &post_f}) {
    g.vk.vk.destroy_module(dev.device(), *m, nullptr);
    *m = nullptr;
  }
  if (!pipes_ok) {
    if (shade_pipe != nullptr) {
      g.vk.vk.destroy_pipe(dev.device(), shade_pipe, nullptr);
    }
    if (post_pipe != nullptr) {
      g.vk.vk.destroy_pipe(dev.device(), post_pipe, nullptr);
    }
    if (shade_layout != nullptr) {
      g.vk.vk.destroy_pipe_layout(dev.device(), shade_layout, nullptr);
    }
    if (post_layout != nullptr) {
      g.vk.vk.destroy_pipe_layout(dev.device(), post_layout, nullptr);
    }
    g.vk.vk.destroy_set_layout(dev.device(), set1_shade, nullptr);
    g.vk.vk.destroy_set_layout(dev.device(), set1_post, nullptr);
    bt.destroy_gpu();
    dev.shutdown();
    return;
  }

  // -- Frame content (lambda for early return; cleanup runs once below) --
  // Handles live outside the lambda so cleanup sees them (all null until set).
  HostBuf scene_b{};
  HostBuf mat_b{};
  HostBuf light_b{};
  HostBuf grid_b{};
  HostBuf index_b{};
  HostBuf vis_b{};
  HostBuf read_b{};
  Img vis_img{};
  Img hdr_img{};
  Img ldr_img{};
  VkSampler sampler = nullptr;
  VkDescriptorPool pool = nullptr;
  VkDescriptorSet set0 = nullptr;
  VkDescriptorSet set_shade = nullptr;
  VkDescriptorSet set_post = nullptr;
  const int frame_rc = [&]() -> int {
  // Resource handles live at function scope so every failure path jumps to
  // the single golden_cleanup label without crossing initializations.
  constexpr std::uint64_t kSceneBytes = kInst * 96;
  constexpr std::uint64_t kMatBytes = kInst * 64;
  std::vector<std::uint8_t> scene(kSceneBytes, 0);
  std::vector<std::uint8_t> mats(kMatBytes, 0);
  PbrMaterialLibrary pack_lib;
  pack_lib.init();
  PbrMaterialLibrary& lib = pack_lib;
  bool pack_ok = true;
  float light_data[8] = {160.0f, 120.0f, 200.0f, 500.0f,
                         1.0f,   1.0f,   1.0f,   40000.0f};
  constexpr std::uint32_t kTilesX = kW / 16;
  constexpr std::uint32_t kTilesY = kH / 16;
  std::vector<std::uint32_t> grid(kTilesX * kTilesY * 2, 0);
  engine::test::GoldenFillTrivialGrid(grid.data(), kTilesX * kTilesY);
  std::uint32_t index_list[1] = {0};
  std::vector<std::uint32_t> vis(kW * kH * 2, 0);
  for (std::uint32_t y = 0; y < kH; ++y) {
    for (std::uint32_t x = 0; x < kW; ++x) {
      const std::uint32_t gx = x / 64;
      const std::uint32_t gy = y / 48;
      vis[(y * kW + x) * 2 + 0] = gy * kGrid + gx;
      vis[(y * kW + x) * 2 + 1] = 0;
    }
  }
  bool bufs_ok = false;
  bool imgs_ok = false;
  bool sets_ok = false;
  {
    for (std::uint32_t gy = 0; gy < kGrid && pack_ok; ++gy) {
      for (std::uint32_t gx = 0; gx < kGrid && pack_ok; ++gx) {
        const std::uint32_t inst = gy * kGrid + gx;
        engine::test::GoldenFillSceneRecord(
            scene.data(), inst, (static_cast<float>(gx) + 0.5f) * 64.0f,
            (static_cast<float>(gy) + 0.5f) * 48.0f, 0.0f, 20.0f);
        engine::renderer::PbrMaterialAsset a{};
        a.base_color = {0.8f, 0.8f, 0.8f, 1.0f};
        a.metallic_roughness[0] = static_cast<float>(gx) / 4.0f;
        a.metallic_roughness[1] =
            0.04f + 0.96f * (static_cast<float>(gy) / 4.0f);
        std::uint32_t slot = 0xFFFFFFFFu;
        if (!lib.add(a, &slot).ok() || slot != inst) {
          check(false, "pbr-golden: material add");
          pack_ok = false;
          break;
        }
        float rec[16]{};
        if (!PbrMaterialLibrary::to_scene_record(a, rec).ok()) {
          check(false, "pbr-golden: record pack");
          pack_ok = false;
          break;
        }
        for (int f = 0; f < 16; ++f) {
          reinterpret_cast<float*>(&mats[inst * 64])[f] = rec[f];
        }
      }
    }
  }
  check(pack_ok, "pbr-golden: 25 materials packed");
  if (!pack_ok) {
    return 1;
  }
  bufs_ok =
      alloc_host(&g, kSceneBytes,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 &scene_b) &&
      alloc_host(&g, kMatBytes,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 &mat_b) &&
      alloc_host(&g, sizeof(light_data),
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 &light_b) &&
      alloc_host(&g, grid.size() * 4,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 &grid_b) &&
      alloc_host(&g, sizeof(index_list),
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 &index_b) &&
      alloc_host(&g, static_cast<std::uint64_t>(kW) * kH * 8,
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &vis_b) &&
      alloc_host(&g, static_cast<std::uint64_t>(kW) * kH * 4,
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT, &read_b);
  check(bufs_ok, "pbr-golden: upload buffers");
  if (!bufs_ok) {
    return 1;
  }
  if (!upload_bytes(&g, &scene_b, scene.data(), scene.size()) ||
      !upload_bytes(&g, &mat_b, mats.data(), mats.size()) ||
      !upload_bytes(&g, &light_b, light_data, sizeof(light_data)) ||
      !upload_bytes(&g, &grid_b, grid.data(), grid.size() * 4) ||
      !upload_bytes(&g, &index_b, index_list, sizeof(index_list)) ||
      !upload_bytes(&g, &vis_b, vis.data(), vis.size() * 4)) {
    check(false, "pbr-golden: uploads mapped");
    return 1;
  }
  check(true, "pbr-golden: uploads mapped");

  imgs_ok =
    make_image(&g, VK_FORMAT_R32G32_UINT,
               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
               &vis_img) &&
    make_image(&g, VK_FORMAT_R16G16B16A16_SFLOAT,
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                   VK_IMAGE_USAGE_SAMPLED_BIT,
               &hdr_img) &&
    make_image(&g, VK_FORMAT_R8G8B8A8_UNORM,
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
               &ldr_img);
  check(imgs_ok, "pbr-golden: images allocated");
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
  if (g.vk.vk.create_sampler(dev.device(), &si, nullptr, &sampler) !=
      VK_SUCCESS) {
    check(false, "pbr-golden: sampler");
    return 1;
  }
  }

  {
  auto create_pool = [&](VkDevice device,
                         const VkDescriptorPoolCreateInfo* info,
                         const VkAllocationCallbacks* alloc,
                         VkDescriptorPool* out) {
    return g.vk.vk.create_pool(device, info, alloc, out);
  };
  auto alloc_sets = [&](VkDevice device,
                        const VkDescriptorSetAllocateInfo* info,
                        VkDescriptorSet* out) {
    return g.vk.vk.alloc_sets(device, info, out);
  };
  sets_ok = engine::test::GoldenAllocFrameSets(
      &g.vk, bt.set_layout(), set1_shade, set1_post, create_pool, alloc_sets,
      &pool, &set0, &set_shade, &set_post);
  check(sets_ok, "pbr-golden: descriptor sets");
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
  // Visibility rides shade set-1 binding 0 (STORAGE_IMAGE readonly;
  // no sampler). The image uploads TRANSFER_DST -> GENERAL for imageLoad.
  // Set-0 binding 2 stays the bindless COMBINED array, unwritten by this
  // golden (P4-06 frame wiring owns it).
  f.vis_view = vis_img.view;
  f.hdr_view = hdr_img.view;
  f.sampler = sampler;
  f.set0 = set0;
  f.set_shade = set_shade;
  f.set_post = set_post;
  auto update_sets = [&](VkDevice device, const VkWriteDescriptorSet* w) {
    // 9 writes (w[9] stays a zeroed sentinel in the helper's w[10]).
    g.vk.vk.update_sets(device, 9, w, 0, nullptr);
  };
  engine::test::GoldenWriteFrameSets(&g.vk, f, update_sets);
  }
  check(true, "pbr-golden: descriptors written");

  // -- Record, submit, read back ------------------------------------------------
  bool submit_ok = false;
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kW) * kH * 4, 0);
  VkCommandPool cmd_pool = nullptr;
  {
    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.queueFamilyIndex = dev.graphics_queue_family();
    if (g.vk.vk.create_cmd_pool(dev.device(), &cpi, nullptr, &cmd_pool) !=
        VK_SUCCESS) {
      check(false, "pbr-golden: cmd pool");
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
    if (g.vk.vk.alloc_cmds(dev.device(), &ab, &cb) != VK_SUCCESS) {
      check(false, "pbr-golden: cmd alloc");
      return 1;
    }
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    bool rec_ok = g.vk.vk.begin_cmd(cb, &bi) == VK_SUCCESS;
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
      fr.post.exposure = 1.0f;
      engine::test::GoldenRecordFrame(&g.vk, cb, fr);
      VkBufferImageCopy rc{};
      rc.bufferOffset = 0;
      rc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      rc.imageExtent = {kW, kH, 1};
      g.vk.vk.cmd_copy_img_buf(cb, ldr_img.img,
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
      g.vk.vk.cmd_barrier2(cb, &dh);
      rec_ok = g.vk.vk.end_cmd(cb) == VK_SUCCESS;
    }
    if (rec_ok) {
      VkSubmitInfo si{};
      si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      si.commandBufferCount = 1;
      si.pCommandBuffers = &cb;
      VkFenceCreateInfo fi{};
      fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      VkFence fence = nullptr;
      if (g.vk.vk.create_fence(dev.device(), &fi, nullptr, &fence) ==
          VK_SUCCESS) {
        if (g.vk.vk.queue_submit(dev.graphics_queue(), 1, &si, fence) ==
                VK_SUCCESS &&
            g.vk.vk.wait_fences(dev.device(), 1, &fence, 1, 10000000000ull) ==
                VK_SUCCESS) {
          submit_ok = true;
        }
        g.vk.vk.destroy_fence(dev.device(), fence, nullptr);
      }
    }
  }
  check(submit_ok, "pbr-golden: submit + fence wait");
  if (submit_ok) {
    void* p = nullptr;
    const std::size_t n = static_cast<std::size_t>(kW) * kH * 4;
    if (g.vk.vk.map_mem(dev.device(), read_b.mem, 0, n, 0, &p) == VK_SUCCESS &&
        p != nullptr) {
      const auto* src = static_cast<const std::uint8_t*>(p);
      for (std::size_t i = 0; i < n; ++i) {
        pixels[i] = src[i];
      }
      g.vk.vk.unmap_mem(dev.device(), read_b.mem);
      check(true, "pbr-golden: readback mapped");
      failures += engine::test::golden_check(
          "p4_01_spheres_320x240",
          std::span<const std::uint8_t>(pixels.data(), pixels.size()), kW,
          kH);
      // Sanity: metallic varies along x — column 0 (metal 0, diffuse
      // kd~0.235) is brighter than column 1 (metal 0.25, kd~0.147) on the
      // BOTTOM row (gy=4, y 192-239: readback row 0 is grid row 0 since
      // the post pass no longer flips Y). The single light at x=160 makes
      // the image x-symmetric, so left-half vs right-half would compare
      // equal — compare adjacent columns instead.
      std::uint64_t col0 = 0;
      std::uint64_t col1 = 0;
      for (std::uint32_t y = 4 * 48; y < 5 * 48; ++y) {
        for (std::uint32_t x = 0; x < 64; ++x) {
          const std::size_t i = (y * kW + x) * 4;
          col0 += pixels[i] + pixels[i + 1] + pixels[i + 2];
          const std::size_t j = (y * kW + x + 64) * 4;
          col1 += pixels[j] + pixels[j + 1] + pixels[j + 2];
        }
      }
      check(col0 > col1, "pbr-golden: metallic varies along x");
      // Sanity: roughness varies along y — the smooth top row (gy=0)
      // concentrates the point-light response into a tight highlight with
      // dark surroundings, while the rough bottom row (gy=4) spreads it:
      // the bottom row's total energy (column 0) exceeds the top row's.
      std::uint64_t row_top = 0;
      std::uint64_t row_bottom = 0;
      for (std::uint32_t y = 0; y < 48; ++y) {
        for (std::uint32_t x = 0; x < 64; ++x) {
          const std::size_t i = (y * kW + x) * 4;
          row_top += pixels[i] + pixels[i + 1] + pixels[i + 2];
          const std::size_t j = ((y + 4 * 48) * kW + x) * 4;
          row_bottom += pixels[j] + pixels[j + 1] + pixels[j + 2];
        }
      }
      check(row_bottom > row_top, "pbr-golden: roughness varies along y");
    } else {
      check(false, "pbr-golden: readback mapped");
    }
  }
  g.vk.vk.destroy_cmd_pool(dev.device(), cmd_pool, nullptr);

  return 0;
  }();
  (void)frame_rc;
  g.vk.vk.destroy_pool(dev.device(), pool, nullptr);
  g.vk.vk.destroy_sampler(dev.device(), sampler, nullptr);
  free_image(&g, &vis_img);
  free_image(&g, &hdr_img);
  free_image(&g, &ldr_img);
  free_host(&g, &scene_b);
  free_host(&g, &mat_b);
  free_host(&g, &light_b);
  free_host(&g, &grid_b);
  free_host(&g, &index_b);
  free_host(&g, &vis_b);
  free_host(&g, &read_b);
  g.vk.vk.destroy_pipe(dev.device(), shade_pipe, nullptr);
  g.vk.vk.destroy_pipe(dev.device(), post_pipe, nullptr);
  g.vk.vk.destroy_pipe_layout(dev.device(), shade_layout, nullptr);
  g.vk.vk.destroy_pipe_layout(dev.device(), post_layout, nullptr);
  g.vk.vk.destroy_set_layout(dev.device(), set1_shade, nullptr);
  g.vk.vk.destroy_set_layout(dev.device(), set1_post, nullptr);
  bt.destroy_gpu();
  dev.shutdown();
}

#endif

}  // namespace

auto main() -> int {
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  test_golden();
#else
  std::printf("[SKIP] pbr-golden: SDK absent (stub build)\n");
#endif
  std::printf(failures == 0 ? "test_pbr_golden: all checks passed\n"
                            : "test_pbr_golden: %d FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}

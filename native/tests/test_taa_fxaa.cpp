// test_taa_fxaa.cpp — P4-10 anti-aliasing golden (TAA + FXAA fallback).
//
// CPU contract (no GPU; runs on stub builds too): Halton jitter
// sequence values, clamp verdicts, resolve blend weights, FXAA edge
// test + luma.
//
// GPU golden: a static vertical-edge scene (dark left 0.1, bright
// right 0.9, 320x240) rendered CPU-side per frame with the edge
// shifted by the TAA jitter (+/-0.5px): 16 frames through the REAL
// taa_resolve pass with ping-pong history, then golden
// p4_10_taa_320x240. Acceptance probes: (a) edge-pixel variance
// |frame16 - frame15| over the edge band stays below kTaaEdgeVarianceMax
// (stated stability threshold check); (b) the FXAA fallback lane runs
// once over the aliased frame-0 and its edge band differs from the
// raw aliased input (it smooths) while flat areas pass through.
//
// Frame shape: one submit per TAA frame (fence-waited), readback after
// frame 15 and frame 16 for the variance probe, final readback for
// the golden. Upload buffers are host-visible; cur/hist images are
// R8G8B8A8_UNORM device-local with SAMPLED+TRANSFER_DST usage; the
// resolve target is COLOR_ATTACHMENT+SAMPLED (ping-pong).
//
// [FAIL] (not [SKIP]) under ENGINE_REQUIRE_GPU=1.
#include <array>
#include <cmath>
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

#include "engine/renderer/anti_aliasing.hpp"
#include "engine/renderer/bindless_table.hpp"
#include "engine/renderer/pbr_pipeline.hpp"
#include "engine/renderer/vulkan_device.hpp"

#include "engine/renderer/shaders/fxaa_frag_spv.inc"
#include "engine/renderer/shaders/post_frag_spv.inc"
#include "engine/renderer/shaders/post_vert_spv.inc"
#include "engine/renderer/shaders/pbr_shade_frag_spv.inc"
#include "engine/renderer/shaders/pbr_shade_vert_spv.inc"
#include "engine/renderer/shaders/pbr_vis_frag_spv.inc"
#include "engine/renderer/shaders/pbr_vis_vert_spv.inc"
#include "engine/renderer/shaders/pbr_shade_vert_spv.inc"
#include "engine/renderer/shaders/taa_resolve_frag_spv.inc"

#include "golden_compare.hpp"
#include "golden_pipe_helper.hpp"
#include "golden_vk_helper.hpp"

#include "engine/renderer/bindless_table.hpp"
#include "engine/renderer/pbr_pipeline.hpp"
#include "engine/renderer/vulkan_device.hpp"

namespace {

auto failures = 0;

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}

#if defined(ENGINE_RENDERER_HAS_VULKAN)
auto gpu_missing(std::string_view what) -> int {
  return engine::test::GoldenGpuMissing(what);
}
#endif

// CPU contract: jitter, clamp, blend, FXAA edge. No GPU.
void test_cpu() {
  using engine::renderer::FxaaBlendWeight;
  using engine::renderer::FxaaIsEdge;
  using engine::renderer::FxaaLuma;
  using engine::renderer::TaaClampHistory;
  using engine::renderer::TaaHalton;
  using engine::renderer::TaaJitterForFrame;
  using engine::renderer::TaaResolveBlend;
  check(engine::renderer::kTaaCycleFrames == 16, "taa: 16-frame cycle");
  // Halton base-2: 1/2, 1/4, 3/4, 1/8 ... (index+1 radical inverse).
  check(std::fabs(TaaHalton(0, 2) - 0.5f) < 1e-6f, "taa: halton2(0)=0.5");
  check(std::fabs(TaaHalton(1, 2) - 0.25f) < 1e-6f, "taa: halton2(1)=0.25");
  check(std::fabs(TaaHalton(2, 2) - 0.75f) < 1e-6f, "taa: halton2(2)=0.75");
  check(std::fabs(TaaHalton(0, 3) - (1.0f / 3.0f)) < 1e-6f,
        "taa: halton3(0)=1/3");
  check(std::fabs(TaaHalton(1, 3) - (2.0f / 3.0f)) < 1e-6f,
        "taa: halton3(1)=2/3");
  check(TaaHalton(0, 5) == 0.0f, "taa: bad base 0");
  // Jitter stays in [-0.5, +0.5) over the full cycle.
  bool jitter_ok = true;
  for (std::uint32_t f = 0; f < 16; ++f) {
    float jx = 9.0f;
    float jy = 9.0f;
    TaaJitterForFrame(f, &jx, &jy);
    if (jx < -0.5f || jx >= 0.5f || jy < -0.5f || jy >= 0.5f) {
      jitter_ok = false;
    }
  }
  check(jitter_ok, "taa: jitter in [-0.5,0.5)");
  float jx0 = 0.0f;
  float jy0 = 0.0f;
  TaaJitterForFrame(0, &jx0, &jy0);
  check(jx0 == 0.0f, "taa: frame0 jx 0");
  // Clamp verdicts.
  float clo = 0.0f;
  check(TaaClampHistory(0.5f, 0.4f, 0.6f, &clo) && clo == 0.5f,
        "taa: inside kept");
  check(!TaaClampHistory(0.9f, 0.4f, 0.6f, &clo) &&
            std::fabs(clo - 0.62f) < 1e-6f,
        "taa: above rejected+clamped");
  check(!TaaClampHistory(0.1f, 0.4f, 0.6f, &clo) &&
            std::fabs(clo - 0.38f) < 1e-6f,
        "taa: below rejected+clamped");
  check(TaaClampHistory(0.5f, 0.4f, 0.6f, nullptr),
        "taa: null out keeps verdict");
  // Slack edge: exactly at bound + slack is kept.
  check(TaaClampHistory(0.62f, 0.4f, 0.6f, nullptr), "taa: slack kept");
  // Blend weights.
  check(std::fabs(TaaResolveBlend(0.0f, 1.0f, true) - 0.9f) < 1e-6f,
        "taa: kept blends 0.9");
  check(std::fabs(TaaResolveBlend(0.0f, 1.0f, false) - 0.5f) < 1e-6f,
        "taa: rejected blends 0.5");
  // FXAA edge + luma.
  check(FxaaIsEdge(0.1f, 0.9f), "fxaa: edge detected");
  check(!FxaaIsEdge(0.5f, 0.55f), "fxaa: flat passes");
  check(FxaaBlendWeight(true) == 0.5f, "fxaa: edge weight 0.5");
  check(FxaaBlendWeight(false) == 0.0f, "fxaa: flat weight 0");
  const float lum = FxaaLuma(1.0f, 1.0f, 1.0f);
  check(std::fabs(lum - 1.0f) < 1e-5f, "fxaa: white luma 1");
  check(std::fabs(FxaaLuma(1.0f, 0.0f, 0.0f) - 0.2126f) < 1e-5f,
        "fxaa: red luma");
}

#if defined(ENGINE_RENDERER_HAS_VULKAN)

constexpr std::uint32_t kW = 320;
constexpr std::uint32_t kH = 240;
constexpr std::uint32_t kFrames = 16;
constexpr float kDark = 0.1f;
constexpr float kBright = 0.9f;
constexpr float kEdgeX = 160.0f;

// Render one jittered edge frame CPU-side: vertical edge at
// kEdgeX + jitter_x pixels (dark left, bright right), RGBA8.
inline void fill_edge_frame(float jitter_x, std::vector<std::uint8_t>* out) {
  out->assign(static_cast<std::size_t>(kW) * kH * 4, 255);
  const float edge = kEdgeX + jitter_x;
  for (std::uint32_t y = 0; y < kH; ++y) {
    for (std::uint32_t x = 0; x < kW; ++x) {
      const float v =
          (static_cast<float>(x) + 0.5f < edge) ? kDark : kBright;
      const auto b = static_cast<std::uint8_t>(v * 255.0f + 0.5f);
      const std::size_t o = (static_cast<std::size_t>(y) * kW + x) * 4;
      (*out)[o + 0] = b;
      (*out)[o + 1] = b;
      (*out)[o + 2] = b;
      (*out)[o + 3] = 255;
    }
  }
}

struct TaaCalls {
  engine::test::GoldenVk* g{nullptr};
  auto operator()(VkDevice device, const VkDescriptorSetLayoutCreateInfo* info,
                  const VkAllocationCallbacks* alloc,
                  VkDescriptorSetLayout* out) const {
    return g->vk.create_set_layout(device, info, alloc, out);
  }
  auto operator()(const VkPipelineLayoutCreateInfo* info,
                  VkPipelineLayout* out) const -> bool {
    return g->vk.create_pipe_layout(g->device, info, nullptr, out) ==
           VK_SUCCESS;
  }
  auto operator()(VkDevice device, VkPipelineCache cache, std::uint32_t count,
                  const VkGraphicsPipelineCreateInfo* infos,
                  const VkAllocationCallbacks* alloc, VkPipeline* pipes) const {
    return g->vk.create_pipes(device, cache, count, infos, alloc, pipes);
  }
  void operator()(VkShaderModule m) const {
    if (m != nullptr) {
      g->vk.destroy_module(g->device, m, nullptr);
    }
  }
  auto operator()(VkDevice device, const VkDescriptorPoolCreateInfo* info,
                  const VkAllocationCallbacks* alloc,
                  VkDescriptorPool* out) const {
    return g->vk.create_pool(device, info, alloc, out);
  }
  auto operator()(VkDevice device, const VkDescriptorSetAllocateInfo* info,
                  VkDescriptorSet* out) const {
    return g->vk.alloc_sets(device, info, out);
  }
  void operator()(VkDevice device, const VkWriteDescriptorSet* writes,
                  int n) const {
    g->vk.update_sets(device, static_cast<std::uint32_t>(n), writes, 0,
                       nullptr);
  }
};

inline auto open_taa_device(engine::renderer::VulkanDevice* dev,
                            engine::test::GoldenVk* g,
                            engine::renderer::PbrPipeline* pbr,
                            engine::renderer::BindlessTable* bt) -> bool {
  auto check_fn = [&](bool ok, const char* name) { check(ok, name); };
  auto missing_fn = [&](const char* /*what*/) {
    failures += gpu_missing("taa: no physical device");
  };
  return engine::test::GoldenOpenDevice(
      check_fn, missing_fn, "P4-10-taa", "taa: device init ok",
      "taa: entry points resolve", "taa: plan init", "taa: bindless init",
      "taa: bindless layout builds", dev, g, pbr, bt);
}

// Fullscreen-triangle pipeline for (pbr_shade.vert, frag) writing
// R8G8B8A8_UNORM with the given set-1 layout + push size.
inline auto build_aa_pipe(engine::test::GoldenVk* g,
                          VkDescriptorSetLayout set1,
                          const std::uint32_t* frag, std::size_t frag_words,
                          std::uint32_t push_size, VkPipelineLayout* layout_out,
                          VkPipeline* pipe_out) -> bool {
  TaaCalls c{g};
  VkShaderModule vert = nullptr;
  VkShaderModule fr = nullptr;
  if (!engine::test::GoldenMakeModule(g, kPbr_shade_vertSpv,
                                      kPbr_shade_vertSpvWords, &vert) ||
      !engine::test::GoldenMakeModule(g, frag, frag_words, &fr)) {
    if (vert != nullptr) {
      c(vert);
    }
    if (fr != nullptr) {
      c(fr);
    }
    return false;
  }
  VkDescriptorSetLayout sets[2] = {nullptr, set1};
  VkPushConstantRange push{};
  push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  push.offset = 0;
  push.size = push_size;
  VkPipelineLayoutCreateInfo pli{};
  pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pli.setLayoutCount = 1;
  pli.pSetLayouts = &sets[1];
  if (push_size > 0) {
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
  }
  bool ok = c(&pli, layout_out);
  if (ok) {
    ok = engine::test::GoldenBuildPipe(c, g->device, vert, fr,
                                       VK_FORMAT_R8G8B8A8_UNORM, *layout_out,
                                       pipe_out);
  }
  c(vert);
  c(fr);
  return ok;
}

inline auto make_aa_sampler(engine::test::GoldenVk* g, VkSampler* out)
    -> bool {
  VkSamplerCreateInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  si.magFilter = VK_FILTER_LINEAR;
  si.minFilter = VK_FILTER_LINEAR;
  si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  return g->vk.create_sampler(g->device, &si, nullptr, out) == VK_SUCCESS;
}

struct TaaPush {
  float dx{0.0f};
  float dy{0.0f};
  float keep_w{0.9f};
  float reject_w{0.5f};
};
static_assert(sizeof(TaaPush) == 16, "taa push is 16B");

auto test_gpu() -> int {
  using engine::renderer::BindlessTable;
  using engine::renderer::PbrPipeline;
  using engine::renderer::VulkanDevice;
  VulkanDevice dev;
  engine::test::GoldenVk g{};
  PbrPipeline pbr;
  BindlessTable bt;
  if (!open_taa_device(&dev, &g, &pbr, &bt)) {
    return 1;
  }

  // Set-1 layouts: TAA resolve (cur SAMPLED + hist SAMPLED + sampler),
  // FXAA (src SAMPLED + sampler).
  TaaCalls calls{&g};
  VkDescriptorSetLayout taa_set1 = nullptr;
  VkDescriptorSetLayout fxaa_set1 = nullptr;
  {
    VkDescriptorSetLayoutBinding b[3]{};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[2].binding = 2;
    b[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    b[2].descriptorCount = 1;
    b[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 3;
    li.pBindings = b;
    bool ok = calls(g.device, &li, nullptr, &taa_set1) == VK_SUCCESS;
    VkDescriptorSetLayoutBinding fb[2]{};
    fb[0].binding = 0;
    fb[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    fb[0].descriptorCount = 1;
    fb[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    fb[1].binding = 1;
    fb[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    fb[1].descriptorCount = 1;
    fb[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo fli{};
    fli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    fli.bindingCount = 2;
    fli.pBindings = fb;
    ok = ok && calls(g.device, &fli, nullptr, &fxaa_set1) == VK_SUCCESS;
    check(ok, "taa: set-1 layouts");
    if (!ok) {
      bt.destroy_gpu();
      dev.shutdown();
      return 1;
    }
  }

  VkPipelineLayout taa_layout = nullptr;
  VkPipeline taa_pipe = nullptr;
  VkPipelineLayout fxaa_layout = nullptr;
  VkPipeline fxaa_pipe = nullptr;
  const bool pipes_ok =
      build_aa_pipe(&g, taa_set1, kTaaResolveFragSpv,
                    kTaaResolveFragSpvWords, sizeof(TaaPush), &taa_layout,
                    &taa_pipe) &&
      build_aa_pipe(&g, fxaa_set1, kFxaaFragSpv, kFxaaFragSpvWords, 0,
                    &fxaa_layout, &fxaa_pipe);
  check(pipes_ok, "taa: resolve+fxaa pipelines build");
  if (!pipes_ok) {
    bt.destroy_gpu();
    dev.shutdown();
    return 1;
  }

  // Images: cur upload target (TRANSFER_DST+SAMPLED), two ping-pong
  // resolve targets (ATTACHMENT+SAMPLED), fxaa target, LDR readback
  // uses the resolve image directly via copy.
  engine::test::GoldenImage cur_img{};
  engine::test::GoldenImage hist_a{};
  engine::test::GoldenImage hist_b{};
  engine::test::GoldenImage fxaa_img{};
  engine::test::GoldenHostBuf upload_b{};
  engine::test::GoldenHostBuf read_b{};
  const std::uint64_t kPxBytes = static_cast<std::uint64_t>(kW) * kH * 4;
  bool res_ok =
      engine::test::GoldenMakeImage(
          &g, VK_FORMAT_R8G8B8A8_UNORM, kW, kH,
          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
          &cur_img) &&
      engine::test::GoldenMakeImage(
          &g, VK_FORMAT_R8G8B8A8_UNORM, kW, kH,
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
              VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
          &hist_a) &&
      engine::test::GoldenMakeImage(
          &g, VK_FORMAT_R8G8B8A8_UNORM, kW, kH,
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
              VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
          &hist_b) &&
      engine::test::GoldenMakeImage(
          &g, VK_FORMAT_R8G8B8A8_UNORM, kW, kH,
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
              VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
          &fxaa_img) &&
      engine::test::GoldenAllocHost(&g, kPxBytes,
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                    &upload_b) &&
      engine::test::GoldenAllocHost(&g, kPxBytes,
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT, &read_b);
  check(res_ok, "taa: images + staging allocated");
  if (!res_ok) {
    bt.destroy_gpu();
    dev.shutdown();
    return 1;
  }
  VkSampler sampler = nullptr;
  check(make_aa_sampler(&g, &sampler), "taa: sampler");
  if (sampler == nullptr) {
    bt.destroy_gpu();
    dev.shutdown();
    return 1;
  }

  // Descriptor pool + sets (oversized sampled-image counts: RADV sizes
  // bindless set-0 at 2x; the pool here covers only set-1 so exact
  // counts suffice, but keep the shared 8192 shape harmlessly small:
  // TAA needs 2 sampled + 1 sampler, FXAA 1 + 1).
  VkDescriptorPool pool = nullptr;
  VkDescriptorSet taa_set = nullptr;
  VkDescriptorSet fxaa_set = nullptr;
  {
    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    sizes[0].descriptorCount = 4;
    sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    sizes[1].descriptorCount = 4;
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 2;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    bool ok = calls(g.device, &pi, nullptr, &pool) == VK_SUCCESS;
    if (ok) {
      VkDescriptorSetLayout layouts[2] = {taa_set1, fxaa_set1};
      VkDescriptorSetAllocateInfo ai{};
      ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      ai.descriptorPool = pool;
      ai.descriptorSetCount = 2;
      ai.pSetLayouts = layouts;
      VkDescriptorSet sets[2]{nullptr, nullptr};
      ok = calls(g.device, &ai, sets) == VK_SUCCESS;
      taa_set = sets[0];
      fxaa_set = sets[1];
    }
    check(ok, "taa: descriptor pool + sets");
    if (!ok) {
      bt.destroy_gpu();
      dev.shutdown();
      return 1;
    }
  }

  // Write sets once (cur/hist views rotate per frame via update).
  auto write_taa_sets = [&](VkImageView cur_v, VkImageView hist_v) {
    VkDescriptorImageInfo cur_i{};
    cur_i.imageView = cur_v;
    cur_i.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo hist_i{};
    hist_i.imageView = hist_v;
    hist_i.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo smp{};
    smp.sampler = sampler;
    VkWriteDescriptorSet w[3]{};
    for (int i = 0; i < 3; ++i) {
      w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w[i].dstSet = taa_set;
      w[i].descriptorCount = 1;
    }
    w[0].dstBinding = 0;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[0].pImageInfo = &cur_i;
    w[1].dstBinding = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[1].pImageInfo = &hist_i;
    w[2].dstBinding = 2;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[2].pImageInfo = &smp;
    calls(g.device, w, 3);
  };
  auto write_fxaa_set = [&](VkImageView src_v) {
    VkDescriptorImageInfo src_i{};
    src_i.imageView = src_v;
    src_i.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo smp{};
    smp.sampler = sampler;
    VkWriteDescriptorSet w[2]{};
    for (int i = 0; i < 2; ++i) {
      w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w[i].dstSet = fxaa_set;
      w[i].descriptorCount = 1;
    }
    w[0].dstBinding = 0;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[0].pImageInfo = &src_i;
    w[1].dstBinding = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[1].pImageInfo = &smp;
    calls(g.device, w, 2);
  };

  // Per-frame: upload jittered cur, record (upload copy + resolve
  // render into ping/pong + barriers), submit, fence-wait.
  std::vector<std::uint8_t> frame_px(static_cast<std::size_t>(kPxBytes), 0);
  std::vector<std::uint8_t> prev_px(static_cast<std::size_t>(kPxBytes), 0);
  std::vector<std::uint8_t> final_px(static_cast<std::size_t>(kPxBytes), 0);
  std::vector<std::uint8_t> aliased_px(static_cast<std::size_t>(kPxBytes), 0);
  bool frames_ok = true;
  float prev_jx = 0.0f;
  float prev_jy = 0.0f;
  bool have_prev = false;
  for (std::uint32_t f = 0; f < kFrames && frames_ok; ++f) {
    float jx = 0.0f;
    float jy = 0.0f;
    engine::renderer::TaaJitterForFrame(f, &jx, &jy);
    fill_edge_frame(jx, &frame_px);
    if (f == 0) {
      aliased_px = frame_px;
    }
    if (!engine::test::GoldenUploadBytes(
            &g, &upload_b,
            std::span<const std::uint8_t>(frame_px.data(), frame_px.size()))) {
      check(false, "taa: frame upload");
      frames_ok = false;
      break;
    }
    const engine::test::GoldenImage* hist_src =
        have_prev ? ((f % 2 == 1) ? &hist_a : &hist_b) : nullptr;
    engine::test::GoldenImage* hist_dst = (f % 2 == 0) ? &hist_a : &hist_b;
    // First frame: seed history with the current frame (no reject).
    TaaPush push{};
    push.dx = have_prev ? (jx - prev_jx) : 0.0f;
    push.dy = have_prev ? (jy - prev_jy) : 0.0f;
    // Rotate descriptor views for this frame.
    if (have_prev) {
      write_taa_sets(cur_img.view, hist_src->view);
    } else {
      write_taa_sets(cur_img.view, cur_img.view);
    }
    // Record.
    VkCommandPool cmd_pool = nullptr;
    VkCommandBuffer cb = nullptr;
    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.queueFamilyIndex = g.queue_family;
    bool rec_ok =
        g.vk.create_cmd_pool(g.device, &cpi, nullptr, &cmd_pool) == VK_SUCCESS;
    if (rec_ok) {
      VkCommandBufferAllocateInfo ab{};
      ab.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      ab.commandPool = cmd_pool;
      ab.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      ab.commandBufferCount = 1;
      rec_ok = g.vk.alloc_cmds(g.device, &ab, &cb) == VK_SUCCESS;
    }
    if (rec_ok) {
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      rec_ok = g.vk.begin_cmd(cb, &bi) == VK_SUCCESS;
    }
    if (rec_ok) {
      // Upload -> cur_img.
      engine::test::GoldenBarrier(
          &g, cb, cur_img.img, VK_IMAGE_LAYOUT_UNDEFINED,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE,
          VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT,
          VK_ACCESS_2_TRANSFER_WRITE_BIT);
      VkBufferImageCopy region{};
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageExtent = {kW, kH, 1};
      g.vk.cmd_copy_buf_img(cb, upload_b.buf, cur_img.img,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
      engine::test::GoldenBarrier(
          &g, cb, cur_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
          VK_ACCESS_2_SHADER_READ_BIT);
      if (have_prev) {
        engine::test::GoldenBarrier(
            &g, cb, hist_src->img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT);
      }
      engine::test::GoldenBarrier(
          &g, cb, hist_dst->img, VK_IMAGE_LAYOUT_UNDEFINED,
          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE,
          VK_ACCESS_2_NONE,
          VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
      // Resolve render.
      VkRenderingAttachmentInfo att{};
      att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      att.imageView = hist_dst->view;
      att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      att.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
      VkRenderingInfo ri{};
      ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
      ri.renderArea = {{0, 0}, {kW, kH}};
      ri.layerCount = 1;
      ri.colorAttachmentCount = 1;
      ri.pColorAttachments = &att;
      g.vk.cmd_begin_render(cb, &ri);
      const float fw = static_cast<float>(kW);
      const float fh = static_cast<float>(kH);
      VkViewport vp{0, 0, fw, fh, 0.0f, 1.0f};
      g.vk.cmd_viewport(cb, 0, 1, &vp);
      VkRect2D sc{{0, 0}, {kW, kH}};
      g.vk.cmd_scissor(cb, 0, 1, &sc);
      g.vk.cmd_bind_pipe(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, taa_pipe);
      g.vk.cmd_bind_sets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, taa_layout, 0,
                         1, &taa_set, 0, nullptr);
      g.vk.cmd_push(cb, taa_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                    sizeof(push), &push);
      g.vk.cmd_draw(cb, 3, 1, 0, 0);
      g.vk.cmd_end_render(cb);
      // Readback after frame 15 and frame 16 for the variance probe;
      // final readback after frame 16 doubles as the golden input.
      const bool want_read = (f == kFrames - 2) || (f == kFrames - 1);
      if (want_read) {
        engine::test::GoldenBarrier(
            &g, cb, hist_dst->img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy rc{};
        rc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rc.imageExtent = {kW, kH, 1};
        g.vk.cmd_copy_img_buf(cb, hist_dst->img,
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
      }
      rec_ok = g.vk.end_cmd(cb) == VK_SUCCESS;
    }
    if (rec_ok) {
      rec_ok = engine::test::GoldenSubmitWait(&g, cb);
    }
    g.vk.destroy_cmd_pool(g.device, cmd_pool, nullptr);
    if (!rec_ok) {
      check(false, "taa: frame submit");
      frames_ok = false;
      break;
    }
    if (f == kFrames - 2 || f == kFrames - 1) {
      void* p = nullptr;
      const std::size_t n = static_cast<std::size_t>(kPxBytes);
      std::vector<std::uint8_t>* dst =
          (f == kFrames - 2) ? &prev_px : &final_px;
      if (g.vk.map_mem(g.device, read_b.mem, 0, n, 0, &p) != VK_SUCCESS ||
          p == nullptr) {
        check(false, "taa: readback mapped");
        frames_ok = false;
        break;
      }
      const auto* src = static_cast<const std::uint8_t*>(p);
      for (std::size_t i = 0; i < n; ++i) {
        (*dst)[i] = src[i];
      }
      g.vk.unmap_mem(g.device, read_b.mem);
    }
    prev_jx = jx;
    prev_jy = jy;
    have_prev = true;
  }
  check(frames_ok, "taa: 16 frames submitted");
  if (!frames_ok) {
    bt.destroy_gpu();
    dev.shutdown();
    return 1;
  }

  // Acceptance (a): edge-band variance |frame16 - frame15| below the
  // stated threshold (kTaaEdgeVarianceMax). Edge band = columns within
  // 2px of the base edge (jitter never pushes it further).
  {
    float worst = 0.0f;
    for (std::uint32_t y = 0; y < kH; ++y) {
      for (std::uint32_t x = static_cast<std::uint32_t>(kEdgeX) - 2;
           x <= static_cast<std::uint32_t>(kEdgeX) + 2; ++x) {
        const std::size_t o = (static_cast<std::size_t>(y) * kW + x) * 4;
        const float d = std::fabs(static_cast<float>(final_px[o]) -
                                  static_cast<float>(prev_px[o])) /
                        255.0f;
        if (d > worst) {
          worst = d;
        }
      }
    }
    std::printf("[INFO] taa-variance: worst=%.4f max=%.4f\n", worst,
                engine::renderer::kTaaEdgeVarianceMax);
    check(worst <= engine::renderer::kTaaEdgeVarianceMax,
          "taa: edge variance below threshold");
  }

  failures += engine::test::golden_check(
      "p4_10_taa_320x240",
      std::span<const std::uint8_t>(final_px.data(), final_px.size()), kW,
      kH);

  // Acceptance (b): FXAA fallback lane — one pass over the aliased
  // frame-0. Edge band must change (smoothed), flat areas must pass
  // through bit-exact.
  {
    if (!engine::test::GoldenUploadBytes(
            &g, &upload_b,
            std::span<const std::uint8_t>(aliased_px.data(),
                                          aliased_px.size()))) {
      check(false, "fxaa: upload");
    } else {
      write_fxaa_set(cur_img.view);
      VkCommandPool cmd_pool = nullptr;
      VkCommandBuffer cb = nullptr;
      VkCommandPoolCreateInfo cpi{};
      cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      cpi.queueFamilyIndex = g.queue_family;
      bool ok =
          g.vk.create_cmd_pool(g.device, &cpi, nullptr, &cmd_pool) == VK_SUCCESS;
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
      if (ok) {
        engine::test::GoldenBarrier(
            &g, cb, cur_img.img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy up{};
        up.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        up.imageExtent = {kW, kH, 1};
        g.vk.cmd_copy_buf_img(cb, upload_b.buf, cur_img.img,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up);
        engine::test::GoldenBarrier(
            &g, cb, cur_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_READ_BIT);
        engine::test::GoldenBarrier(
            &g, cb, fxaa_img.img, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE,
            VK_ACCESS_2_NONE,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo att{};
        att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        att.imageView = fxaa_img.view;
        att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        VkRenderingInfo ri{};
        ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
        ri.renderArea = {{0, 0}, {kW, kH}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &att;
        g.vk.cmd_begin_render(cb, &ri);
        const float fw = static_cast<float>(kW);
        const float fh = static_cast<float>(kH);
        VkViewport vp{0, 0, fw, fh, 0.0f, 1.0f};
        g.vk.cmd_viewport(cb, 0, 1, &vp);
        VkRect2D sc{{0, 0}, {kW, kH}};
        g.vk.cmd_scissor(cb, 0, 1, &sc);
        g.vk.cmd_bind_pipe(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, fxaa_pipe);
        g.vk.cmd_bind_sets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, fxaa_layout,
                           0, 1, &fxaa_set, 0, nullptr);
        g.vk.cmd_draw(cb, 3, 1, 0, 0);
        g.vk.cmd_end_render(cb);
        ok = engine::test::GoldenSubmitReadback(&g, cb, fxaa_img.img,
                                               read_b.buf, kW, kH,
                                               [&](VkCommandBuffer c,
                                                   VkImage img,
                                                   VkImageLayout lay,
                                                   VkBuffer buf,
                                                   std::uint32_t n,
                                                   const VkBufferImageCopy* r) {
                                                 g.vk.cmd_copy_img_buf(c, img,
                                                                       lay,
                                                                       buf, n,
                                                                       r);
                                               },
                                               [&](VkCommandBuffer c,
                                                   const VkDependencyInfo* d) {
                                                 g.vk.cmd_barrier2(c, d);
                                               },
                                               [&](VkCommandBuffer c) {
                                                 return g.vk.end_cmd(c) ==
                                                        VK_SUCCESS;
                                               });
      }
      if (ok) {
        ok = engine::test::GoldenSubmitWait(&g, cb);
      }
      g.vk.destroy_cmd_pool(g.device, cmd_pool, nullptr);
      check(ok, "fxaa: pass submitted");
      if (ok) {
        void* p = nullptr;
        const std::size_t n = static_cast<std::size_t>(kPxBytes);
        std::vector<std::uint8_t> fxaa_px(n, 0);
        if (g.vk.map_mem(g.device, read_b.mem, 0, n, 0, &p) != VK_SUCCESS ||
            p == nullptr) {
          check(false, "fxaa: readback mapped");
        } else {
          const auto* src = static_cast<const std::uint8_t*>(p);
          for (std::size_t i = 0; i < n; ++i) {
            fxaa_px[i] = src[i];
          }
          g.vk.unmap_mem(g.device, read_b.mem);
          // Edge band changed (smoothed): at least one pixel differs.
          std::size_t edge_diff = 0;
          for (std::uint32_t y = 0; y < kH; ++y) {
            for (std::uint32_t x =
                     static_cast<std::uint32_t>(kEdgeX) - 1;
                 x <= static_cast<std::uint32_t>(kEdgeX) + 1; ++x) {
              const std::size_t o = (static_cast<std::size_t>(y) * kW + x) * 4;
              if (fxaa_px[o] != aliased_px[o]) {
                ++edge_diff;
              }
            }
          }
          check(edge_diff > 0, "fxaa: edge band smoothed");
          // Flat areas pass through bit-exact (far left + far right).
          bool flat_ok = true;
          for (std::uint32_t y = 0; y < kH && flat_ok; ++y) {
            for (std::uint32_t x : {10u, 20u, 300u, 310u}) {
              const std::size_t o = (static_cast<std::size_t>(y) * kW + x) * 4;
              if (fxaa_px[o] != aliased_px[o]) {
                flat_ok = false;
                break;
              }
            }
          }
          check(flat_ok, "fxaa: flat areas pass through");
        }
      }
    }
  }

  g.vk.destroy_pool(g.device, pool, nullptr);
  g.vk.destroy_sampler(g.device, sampler, nullptr);
  engine::test::GoldenFreeImage(&g, &cur_img);
  engine::test::GoldenFreeImage(&g, &hist_a);
  engine::test::GoldenFreeImage(&g, &hist_b);
  engine::test::GoldenFreeImage(&g, &fxaa_img);
  engine::test::GoldenFreeHost(&g, &upload_b);
  engine::test::GoldenFreeHost(&g, &read_b);
  g.vk.destroy_pipe(g.device, taa_pipe, nullptr);
  g.vk.destroy_pipe(g.device, fxaa_pipe, nullptr);
  g.vk.destroy_pipe_layout(g.device, taa_layout, nullptr);
  g.vk.destroy_pipe_layout(g.device, fxaa_layout, nullptr);
  g.vk.destroy_set_layout(g.device, taa_set1, nullptr);
  g.vk.destroy_set_layout(g.device, fxaa_set1, nullptr);
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
  std::printf("[SKIP] taa-gpu: SDK absent (stub build)\n");
#endif
  std::printf(failures == 0 ? "test_taa_fxaa: all checks passed\n"
                            : "test_taa_fxaa: %d FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}

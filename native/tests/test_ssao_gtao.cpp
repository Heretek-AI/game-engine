// test_ssao_gtao.cpp — P4-09 half-resolution GTAO occlusion pass proof.
//
// Scope of THIS card (t_390bf7c4): the occlusion pass only. No golden
// from this card (goldens belong to t_08cad87f); no blur, no composite,
// no toggle. The GPU proof renders the half-res occlusion texture from
// a synthetic depth step (near left 0.2, far right 0.8, 320x240) and
// checks every half-res texel against the ssao.hpp CPU reference
// (bit-exact before u8 rounding; u8 tolerance 1 LSB). Flat-area
// texels must read fully open (255); far-side texels adjacent to the
// step must dip below 255 (occluded).
//
// CPU contract (no GPU; also runs on stub builds): half sizes,
// tap/occlusion reference math, push size.
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

#include "engine/renderer/bindless_table.hpp"
#include "engine/renderer/pbr_pipeline.hpp"
#include "engine/renderer/ssao.hpp"
#include "engine/renderer/vulkan_device.hpp"

#include "engine/renderer/shaders/pbr_shade_frag_spv.inc"
#include "engine/renderer/shaders/pbr_shade_vert_spv.inc"
#include "engine/renderer/shaders/pbr_vis_frag_spv.inc"
#include "engine/renderer/shaders/pbr_vis_vert_spv.inc"
#include "engine/renderer/shaders/post_frag_spv.inc"
#include "engine/renderer/shaders/post_vert_spv.inc"
#include "engine/renderer/shaders/ssao_gtao_frag_spv.inc"

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

#if defined(ENGINE_RENDERER_HAS_VULKAN)
auto gpu_missing(std::string_view what) -> int {
  return engine::test::GoldenGpuMissing(what);
}
#endif

// CPU contract: sizes, tap math, full-pixel occlusion, push size.
void test_cpu() {
  using engine::renderer::SsaoGtaoPush;
  using engine::renderer::SsaoHalfSize;
  using engine::renderer::SsaoGtaoTap;
  using engine::renderer::SsaoOcclusionCpu;
  // Golden frame: 320x240 -> 160x120 half-res.
  std::uint32_t hw = 0;
  std::uint32_t hh = 0;
  check(SsaoHalfSize(320, 240, &hw, &hh), "ssao: half size ok");
  check(hw == 160 && hh == 120, "ssao: half is 160x120");
  check(!SsaoHalfSize(320, 240, nullptr, &hh), "ssao: null w rejected");
  check(!SsaoHalfSize(0, 240, &hw, &hh), "ssao: zero w rejected");
  std::uint32_t o1 = 0;
  std::uint32_t o2 = 0;
  check(SsaoHalfSize(1, 1, &o1, &o2) && o1 == 1 && o2 == 1,
        "ssao: 1x1 never 0");
  check(SsaoHalfSize(321, 241, &o1, &o2) && o1 == 161 && o2 == 121,
        "ssao: odd ceil-halves");
  // Taps: closer neighbours occlude, farther/equal do not.
  check(SsaoGtaoTap(0.8f, 0.2f, 4.0f) == 1.0f, "ssao: near neighbor full");
  check(SsaoGtaoTap(0.8f, 0.8f, 4.0f) == 0.0f, "ssao: equal none");
  check(SsaoGtaoTap(0.2f, 0.8f, 4.0f) == 0.0f, "ssao: far neighbor none");
  const float half = SsaoGtaoTap(0.8f, 0.675f, 4.0f);
  check(std::fabs(half - 0.5f) < 1e-6f, "ssao: tap is linear");
  // Full pixel: flat neighbourhood -> exactly open.
  float flat[8];
  for (float& v : flat) {
    v = 0.8f;
  }
  check(SsaoOcclusionCpu(0.8f, flat, 4.0f, 1.0f) == 1.0f,
        "ssao: flat is open");
  // Step pixel: center 0.8 with five 0.2 neighbours occludes.
  float step[8] = {0.2f, 0.2f, 0.2f, 0.2f, 0.2f, 0.8f, 0.8f, 0.8f};
  const float occ = SsaoOcclusionCpu(0.8f, step, 4.0f, 1.0f);
  check(std::fabs(occ - 0.375f) < 1e-5f, "ssao: step occludes to 0.375");
  check(sizeof(SsaoGtaoPush) == 16, "ssao: gtao push 16B");
}

#if defined(ENGINE_RENDERER_HAS_VULKAN)

constexpr std::uint32_t kW = 320;
constexpr std::uint32_t kH = 240;
constexpr std::uint32_t kHw = 160;
constexpr std::uint32_t kHh = 120;

struct SsaoGtaoPushUbo {
  float texel[2]{0.0f, 0.0f};
  float depth_scale{engine::renderer::kSsaoDepthScale};
  float strength{engine::renderer::kSsaoStrength};
};
static_assert(sizeof(SsaoGtaoPushUbo) == 16, "ssao test push is 16B");

inline auto open_ssao_device(engine::renderer::VulkanDevice* dev,
                             engine::test::GoldenVk* g,
                             engine::renderer::PbrPipeline* pbr,
                             engine::renderer::BindlessTable* bt) -> bool {
  auto check_fn = [&](bool ok, const char* name) { check(ok, name); };
  auto missing_fn = [&](const char* /*what*/) {
    failures += gpu_missing("ssao-gtao: no physical device");
  };
  return engine::test::GoldenOpenDeviceTagged(check_fn, missing_fn,
                                              "P4-09-ssao-gtao", dev, g, pbr,
                                              bt);
}

auto test_gpu() -> int {
  using engine::renderer::BindlessTable;
  using engine::renderer::PbrPipeline;
  using engine::renderer::VulkanDevice;
  VulkanDevice dev;
  engine::test::GoldenVk g{};
  PbrPipeline pbr;
  BindlessTable bt;
  if (!open_ssao_device(&dev, &g, &pbr, &bt)) {
    return 1;
  }
  const engine::test::GoldenDeviceCalls dc{&g};

  // Set-0 layout: depth SAMPLED_IMAGE + sampler.
  VkDescriptorSetLayout set0 = nullptr;
  {
    VkDescriptorSetLayoutBinding b[2]{};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 2;
    li.pBindings = b;
    check(dc(g.device, &li, nullptr, &set0) == VK_SUCCESS,
          "ssao-gtao: set layout");
    if (set0 == nullptr) {
      bt.destroy_gpu();
      dev.shutdown();
      return 1;
    }
  }

  // Pipeline: fullscreen vert + ssao_gtao frag -> R32_SFLOAT.
  VkShaderModule vert = nullptr;
  VkShaderModule frag = nullptr;
  bool mods_ok =
      engine::test::GoldenMakeModule(&g, kPbr_shade_vertSpv,
                                     kPbr_shade_vertSpvWords, &vert) &&
      engine::test::GoldenMakeModule(&g, kSsaoGtaoFragSpv,
                                     kSsaoGtaoFragSpvWords, &frag);
  check(mods_ok, "ssao-gtao: shader modules");
  VkPipelineLayout layout = nullptr;
  VkPipeline pipe = nullptr;
  if (mods_ok) {
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset = 0;
    push.size = sizeof(SsaoGtaoPushUbo);
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set0;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    mods_ok = dc(&pli, &layout);
    if (mods_ok) {
      mods_ok = engine::test::GoldenBuildPipe(
          dc, g.device, vert, frag, VK_FORMAT_R32_SFLOAT, layout, &pipe);
    }
  }
  dc(vert);
  dc(frag);
  check(mods_ok, "ssao-gtao: pipeline builds");
  if (!mods_ok) {
    bt.destroy_gpu();
    dev.shutdown();
    return 1;
  }

  // Images: full-res depth (TRANSFER_DST+SAMPLED, R32_SFLOAT),
  // half-res occlusion target (ATTACHMENT+SAMPLED+TRANSFER_SRC),
  // 4-byte-per-texel float readback of the half-res target.
  engine::test::GoldenImage depth_img{};
  engine::test::GoldenImage occ_img{};
  engine::test::GoldenHostBuf upload_b{};
  engine::test::GoldenHostBuf read_b{};
  const std::uint64_t kDepthBytes =
      static_cast<std::uint64_t>(kW) * kH * 4;
  const std::uint64_t kOccBytes =
      static_cast<std::uint64_t>(kHw) * kHh * 4;
  bool res_ok =
      engine::test::GoldenMakeImage(
          &g, VK_FORMAT_R32_SFLOAT, kW, kH,
          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
          &depth_img) &&
      engine::test::GoldenMakeImage(
          &g, VK_FORMAT_R32_SFLOAT, kHw, kHh,
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
              VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
          &occ_img) &&
      engine::test::GoldenAllocHost(&g, kDepthBytes,
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                    &upload_b) &&
      engine::test::GoldenAllocHost(&g, kOccBytes,
                                    VK_BUFFER_USAGE_TRANSFER_DST_BIT, &read_b);
  check(res_ok, "ssao-gtao: images + staging allocated");
  if (!res_ok) {
    bt.destroy_gpu();
    dev.shutdown();
    return 1;
  }
  VkSampler sampler = nullptr;
  check(engine::test::GoldenMakeSampler(&g, &sampler),
        "ssao-gtao: sampler");
  if (sampler == nullptr) {
    bt.destroy_gpu();
    dev.shutdown();
    return 1;
  }

  // Descriptor pool + set.
  VkDescriptorPool pool = nullptr;
  VkDescriptorSet set = nullptr;
  {
    VkDescriptorPoolSize sizes[2]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    sizes[0].descriptorCount = 1;
    sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    sizes[1].descriptorCount = 1;
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 1;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    bool ok = dc(g.device, &pi, nullptr, &pool) == VK_SUCCESS;
    if (ok) {
      VkDescriptorSetAllocateInfo ai{};
      ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      ai.descriptorPool = pool;
      ai.descriptorSetCount = 1;
      ai.pSetLayouts = &set0;
      ok = dc(g.device, &ai, &set) == VK_SUCCESS;
    }
    check(ok, "ssao-gtao: descriptor pool + set");
    if (!ok) {
      bt.destroy_gpu();
      dev.shutdown();
      return 1;
    }
    VkDescriptorImageInfo di{};
    di.imageView = depth_img.view;
    di.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo si{};
    si.sampler = sampler;
    VkWriteDescriptorSet ws[2]{};
    for (int i = 0; i < 2; ++i) {
      ws[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      ws[i].dstSet = set;
      ws[i].descriptorCount = 1;
    }
    ws[0].dstBinding = 0;
    ws[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    ws[0].pImageInfo = &di;
    ws[1].dstBinding = 1;
    ws[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    ws[1].pImageInfo = &si;
    g.vk.update_sets(g.device, 2, ws, 0, nullptr);
  }

  // Depth-step upload: left half near 0.2, right half far 0.8.
  std::vector<float> depth_px(static_cast<std::size_t>(kW) * kH, 0.0f);
  for (std::uint32_t y = 0; y < kH; ++y) {
    for (std::uint32_t x = 0; x < kW; ++x) {
      depth_px[static_cast<std::size_t>(y) * kW + x] =
          (x < kW / 2) ? engine::renderer::kSsaoNearDepth
                       : engine::renderer::kSsaoFarDepth;
    }
  }
  if (!engine::test::GoldenUploadBytes(
          &g, &upload_b,
          std::span<const std::uint8_t>(
              reinterpret_cast<const std::uint8_t*>(depth_px.data()),
              depth_px.size() * sizeof(float)))) {
    check(false, "ssao-gtao: depth upload");
    bt.destroy_gpu();
    dev.shutdown();
    return 1;
  }

  // Record: upload depth, render occlusion, read back half-res floats.
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
    engine::test::GoldenBarrier(
        &g, cb, depth_img.img, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE,
        VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_COPY_BIT,
        VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kW, kH, 1};
    g.vk.cmd_copy_buf_img(cb, upload_b.buf, depth_img.img,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    engine::test::GoldenBarrier(
        &g, cb, depth_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
        VK_ACCESS_2_SHADER_READ_BIT);
    engine::test::GoldenBarrier(
        &g, cb, occ_img.img, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_NONE,
        VK_ACCESS_2_NONE,
        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo att{};
    att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    att.imageView = occ_img.view;
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue.color = {{1.0f, 0.0f, 0.0f, 1.0f}};
    VkRenderingInfo ri{};
    ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    ri.renderArea = {{0, 0}, {kHw, kHh}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    g.vk.cmd_begin_render(cb, &ri);
    const float fw = static_cast<float>(kHw);
    const float fh = static_cast<float>(kHh);
    VkViewport vp{0, 0, fw, fh, 0.0f, 1.0f};
    g.vk.cmd_viewport(cb, 0, 1, &vp);
    VkRect2D sc{{0, 0}, {kHw, kHh}};
    g.vk.cmd_scissor(cb, 0, 1, &sc);
    g.vk.cmd_bind_pipe(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    g.vk.cmd_bind_sets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                       &set, 0, nullptr);
    SsaoGtaoPushUbo push{};
    push.texel[0] = 1.0f / static_cast<float>(kHw);
    push.texel[1] = 1.0f / static_cast<float>(kHh);
    g.vk.cmd_push(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                  &push);
    g.vk.cmd_draw(cb, 3, 1, 0, 0);
    g.vk.cmd_end_render(cb);
    rec_ok = engine::test::GoldenSubmitReadback(
        &g, cb, occ_img.img, read_b.buf, kHw, kHh,
        [&](VkCommandBuffer c, VkImage img, VkImageLayout lay, VkBuffer buf,
            std::uint32_t n, const VkBufferImageCopy* r) {
          g.vk.cmd_copy_img_buf(c, img, lay, buf, n, r);
        },
        [&](VkCommandBuffer c, const VkDependencyInfo* d) {
          g.vk.cmd_barrier2(c, d);
        },
        [&](VkCommandBuffer c) { return g.vk.end_cmd(c) == VK_SUCCESS; });
  }
  if (rec_ok) {
    rec_ok = engine::test::GoldenSubmitWait(&g, cb);
  }
  g.vk.destroy_cmd_pool(g.device, cmd_pool, nullptr);
  check(rec_ok, "ssao-gtao: pass submitted");

  // Readback + compare against the CPU reference per texel.
  bool proof_ok = false;
  if (rec_ok) {
    void* p = nullptr;
    const std::size_t n = static_cast<std::size_t>(kOccBytes);
    std::vector<float> occ(static_cast<std::size_t>(kHw) * kHh, 0.0f);
    if (g.vk.map_mem(g.device, read_b.mem, 0, n, 0, &p) != VK_SUCCESS ||
        p == nullptr) {
      check(false, "ssao-gtao: readback mapped");
    } else {
      const auto* src = static_cast<const float*>(p);
      for (std::size_t i = 0; i < occ.size(); ++i) {
        occ[i] = src[i];
      }
      g.vk.unmap_mem(g.device, read_b.mem);
      // Reference: replicate the full-res depth step on the CPU and
      // run SsaoOcclusionCpu for each half-res texel's 2x2-block
      // top-left full-res texel (same mapping as the shader).
      std::uint32_t mism = 0;
      bool flat_open = true;
      bool step_dips = false;
      for (std::uint32_t hy = 0; hy < kHh; ++hy) {
        for (std::uint32_t hx = 0; hx < kHw; ++hx) {
          const std::uint32_t fx = hx * 2;
          const std::uint32_t fy = hy * 2;
          auto at = [&](std::uint32_t x, std::uint32_t y) -> float {
            const std::uint32_t cx = (x < kW) ? x : (kW - 1);
            (void)y;
            return (cx < kW / 2) ? engine::renderer::kSsaoNearDepth
                                 : engine::renderer::kSsaoFarDepth;
          };
          const float center = at(fx, fy);
          float nb[8];
          int k = 0;
          for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
              if (dx == 0 && dy == 0) {
                continue;
              }
              const int qx = static_cast<int>(fx) + dx;
              const int qy = static_cast<int>(fy) + dy;
              const std::uint32_t ccx =
                  qx < 0 ? 0 : static_cast<std::uint32_t>(qx);
              const std::uint32_t ccy =
                  qy < 0 ? 0 : static_cast<std::uint32_t>(qy);
              nb[k++] = at(ccx, ccy);
            }
          }
          const float ref = engine::renderer::SsaoOcclusionCpu(
              center, nb, engine::renderer::kSsaoDepthScale,
              engine::renderer::kSsaoStrength);
          const float got = occ[static_cast<std::size_t>(hy) * kHw + hx];
          if (std::fabs(got - ref) > (1.0f / 255.0f)) {
            ++mism;
          }
          // Flat far area (well right of the step): fully open.
          if (hx >= 90 && hx <= 150) {
            if (got < 0.999f) {
              flat_open = false;
            }
          }
          // Far-side column hugging the step (full-res x=160 -> hx=80):
          // occluded below 1.0.
          if (hx == 80 && got < 0.999f) {
            step_dips = true;
          }
        }
      }
      std::printf("[INFO] ssao-gtao: texel mismatches=%u flat_open=%d "
                  "step_dips=%d\n",
                  mism, flat_open ? 1 : 0, step_dips ? 1 : 0);
      check(mism == 0, "ssao-gtao: half-res matches CPU reference");
      check(flat_open, "ssao-gtao: flat areas fully open");
      check(step_dips, "ssao-gtao: step edge occluded");
      proof_ok = (mism == 0) && flat_open && step_dips;
    }
  }

  g.vk.destroy_pool(g.device, pool, nullptr);
  g.vk.destroy_sampler(g.device, sampler, nullptr);
  engine::test::GoldenFreeImage(&g, &depth_img);
  engine::test::GoldenFreeImage(&g, &occ_img);
  engine::test::GoldenFreeHost(&g, &upload_b);
  engine::test::GoldenFreeHost(&g, &read_b);
  g.vk.destroy_pipe(g.device, pipe, nullptr);
  g.vk.destroy_pipe_layout(g.device, layout, nullptr);
  g.vk.destroy_set_layout(g.device, set0, nullptr);
  bt.destroy_gpu();
  dev.shutdown();
  return proof_ok ? 0 : 1;
}

#endif

}  // namespace

auto main() -> int {
  test_cpu();
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  const int gpu_rc = test_gpu();
  if (gpu_rc != 0) {
    ++failures;
  }
#else
  std::printf("[SKIP] ssao-gtao-gpu: SDK absent (stub build)\n");
#endif
  std::printf(failures == 0 ? "test_ssao_gtao: all checks passed\n"
                            : "test_ssao_gtao: %d FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}

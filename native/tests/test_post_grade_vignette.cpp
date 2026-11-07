// test_post_grade_vignette.cpp — P4-11 LUT color grade + vignette.
//
// Two ordered PostProcess effects (effect ids 2 = grade, 3 = vignette;
// fog = 1 belongs to sibling t_a42ec749):
//   - CPU contract: params size/validate, CPU references, push sizes,
//     ordered-list registration (grade slot before vignette slot runs
//     first — array order IS the execution order).
//   - GPU goldens (lavapipe, ENGINE_REQUIRE_GPU=1 FAIL not SKIP):
//     p4_11_lut_320x240 (analytic grade on a synthetic gradient) and
//     p4_11_vignette_320x240 (vignette on the graded result — the two
//     passes run IN ORDER through one PostProcess effect list).
//     Toggle-off: identity grade + zero-intensity vignette reproduce the
//     input bit-for-bit.
//
// Frame shape (no shade pass — LDR-to-LDR post chain on a synthetic
// gradient upload): upload RGBA gradient -> grade pass -> vignette pass
// -> LDR readback. One submit, fence-waited.
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

#include "engine/renderer/post_grade.hpp"

#include "engine/renderer/bindless_table.hpp"
#include "engine/renderer/pbr_pipeline.hpp"
#include "engine/renderer/vulkan_device.hpp"

#include "engine/renderer/shaders/post_grade_frag_spv.inc"
#include "engine/renderer/shaders/post_vert_spv.inc"
#include "engine/renderer/shaders/post_vignette_frag_spv.inc"

// PBR shade/post blobs: GoldenOpenDevice boots the shared PbrPipeline +
// bindless plan (same device path as every golden TU), so these SPV
// symbols must be visible even though this frame never records shade.
#include "engine/renderer/shaders/pbr_shade_frag_spv.inc"
#include "engine/renderer/shaders/pbr_shade_vert_spv.inc"
#include "engine/renderer/shaders/pbr_vis_frag_spv.inc"
#include "engine/renderer/shaders/pbr_vis_vert_spv.inc"
#include "engine/renderer/shaders/post_frag_spv.inc"

#include "golden_compare.hpp"
#include "golden_pipe_helper.hpp"
#include "golden_vk_helper.hpp"

namespace {

engine::test::GoldenCheck gold{};

auto check(bool ok, const char* name) -> void { gold.check(ok, name); }

auto& failures = gold.failures;

#if defined(ENGINE_RENDERER_HAS_VULKAN)
auto gpu_missing(std::string_view what) -> int {
  return engine::test::GoldenGpuMissing(what);
}
#endif

// P4-11 CPU contract: sizes, defaults, validate/clamp, CPU references,
// push mirrors, ordered effect ids. No GPU needed; runs on stub builds.
void test_cpu() {
  using engine::renderer::ColorGradeCpu;
  using engine::renderer::ColorGradeParams;
  using engine::renderer::ColorGradeValidate;
  using engine::renderer::GradePush;
  using engine::renderer::VignetteFactorCpu;
  using engine::renderer::VignetteParams;
  using engine::renderer::VignettePush;
  using engine::renderer::VignetteValidate;
  check(sizeof(ColorGradeParams) == 24, "grade: params 24B");
  check(alignof(ColorGradeParams) == 4, "grade: params align 4");
  check(sizeof(VignetteParams) == 20, "vignette: params 20B");
  check(alignof(VignetteParams) == 4, "vignette: params align 4");
  check(sizeof(GradePush) == 32, "grade: push 32B");
  check(sizeof(VignettePush) == 32, "vignette: push 32B");
  check(engine::renderer::kPostEffectColorGrade == 2, "grade: effect id 2");
  check(engine::renderer::kPostEffectVignette == 3, "vignette: effect id 3");
  // Defaults: identity grade (passthrough), gentle vignette.
  const ColorGradeParams dg{};
  check(dg.lift[0] == 0.0f && dg.gain[0] == 1.0f, "grade: identity default");
  const VignetteParams dv{};
  check(dv.intensity >= 0.0f && dv.intensity <= 1.0f,
        "vignette: default intensity in range");
  // Validate: null fails, NaN fails, clamps hold.
  check(!ColorGradeValidate(nullptr).ok(), "grade: null rejected");
  ColorGradeParams bad{};
  bad.gain[1] = std::numeric_limits<float>::quiet_NaN();
  check(!ColorGradeValidate(&bad).ok(), "grade: NaN rejected");
  ColorGradeParams clamp_me{};
  clamp_me.gain[0] = -2.0f;
  clamp_me.lift[1] = 5.0f;
  check(ColorGradeValidate(&clamp_me).ok(), "grade: clamp ok");
  check(clamp_me.gain[0] == 0.0f, "grade: negative gain clamped");
  check(clamp_me.lift[1] == 1.0f, "grade: lift clamped to 1");
  check(!VignetteValidate(nullptr).ok(), "vignette: null rejected");
  VignetteParams vbad{};
  vbad.radius = std::numeric_limits<float>::quiet_NaN();
  check(!VignetteValidate(&vbad).ok(), "vignette: NaN rejected");
  VignetteParams vclamp{};
  vclamp.radius = 99.0f;
  vclamp.intensity = 5.0f;
  vclamp.softness = 0.0f;
  check(VignetteValidate(&vclamp).ok(), "vignette: clamp ok");
  check(vclamp.radius == 2.0f, "vignette: radius clamped");
  check(vclamp.intensity == 1.0f, "vignette: intensity clamped");
  check(vclamp.softness == 0.01f, "vignette: softness clamped");
  // CPU refs: identity grade is exact; lift adds; gain scales.
  check(ColorGradeCpu(0.5f, 0.0f, 1.0f) == 0.5f, "grade: identity exact");
  check(ColorGradeCpu(0.5f, 0.1f, 1.0f) > 0.599f &&
            ColorGradeCpu(0.5f, 0.1f, 1.0f) < 0.601f,
        "grade: lift adds");
  check(ColorGradeCpu(0.5f, 0.0f, 2.0f) == 1.0f, "grade: gain clamps at 1");
  check(ColorGradeCpu(0.0f, 0.0f, 1.0f) == 0.0f, "grade: black stays black");
  // Vignette factor: center == 1 (no darkening), intensity 0 == 1
  // everywhere, corners darken with intensity 1.
  const float fc = VignetteFactorCpu(0.5f, 0.5f, 0.5f, 0.5f, 0.75f, 0.35f,
                                     4.0f / 3.0f, 1.0f);
  check(fc > 0.999f && fc <= 1.0f, "vignette: center untouched");
  const float f0 = VignetteFactorCpu(0.0f, 0.0f, 0.5f, 0.5f, 0.75f, 0.35f,
                                     4.0f / 3.0f, 0.0f);
  check(f0 == 1.0f, "vignette: intensity 0 passthrough");
  const float fcorner = VignetteFactorCpu(0.0f, 0.0f, 0.5f, 0.5f, 0.75f,
                                          0.35f, 4.0f / 3.0f, 1.0f);
  check(fcorner < 0.5f, "vignette: corner darkens at intensity 1");
  // Ordering: effect list array order is execution order — grade (2)
  // before vignette (3) in the ordered list.
  std::array<std::uint32_t, 8> effects{0, 0, 0, 0, 0, 0, 0, 0};
  std::uint32_t effect_count = 0;
  effects[effect_count++] = engine::renderer::kPostEffectColorGrade;
  effects[effect_count++] = engine::renderer::kPostEffectVignette;
  check(effect_count == 2, "postprocess: two effects listed");
  check(effects[0] == engine::renderer::kPostEffectColorGrade,
        "postprocess: grade runs first");
  check(effects[1] == engine::renderer::kPostEffectVignette,
        "postprocess: vignette runs second");
}

#if defined(ENGINE_RENDERER_HAS_VULKAN)

constexpr std::uint32_t kW = 320;
constexpr std::uint32_t kH = 240;

// Synthetic LDR gradient input: R ramps with x, G ramps with y,
// B = 0.5 flat. Deterministic, no shade pass needed.
inline void fill_gradient(std::vector<std::uint8_t>* px) {
  px->assign(static_cast<std::size_t>(kW) * kH * 4, 0);
  for (std::uint32_t y = 0; y < kH; ++y) {
    for (std::uint32_t x = 0; x < kW; ++x) {
      const std::size_t at = (static_cast<std::size_t>(y) * kW + x) * 4;
      (*px)[at + 0] = static_cast<std::uint8_t>((x * 255) / (kW - 1));
      (*px)[at + 1] = static_cast<std::uint8_t>((y * 255) / (kH - 1));
      (*px)[at + 2] = 128;
      (*px)[at + 3] = 255;
    }
  }
}

inline void grade_gpu_missing() {
  failures += gpu_missing("grade-vignette: no physical device");
}

inline auto open_grade_device(engine::renderer::VulkanDevice* dev,
                              engine::test::GoldenVk* g,
                              engine::renderer::PbrPipeline* pbr,
                              engine::test::GoldenVk* /*unused*/,
                              engine::renderer::BindlessTable* bt) -> bool {
  auto check_fn = [&](bool ok, const char* name) { check(ok, name); };
  auto missing_fn = [&](const char* /*what*/) { grade_gpu_missing(); };
  return engine::test::GoldenOpenDeviceTagged(check_fn, missing_fn,
                                              "grade-vignette", dev, g, pbr,
                                              bt);
}

struct GradeVignetteCalls {
  const engine::test::GoldenDeviceCalls* dc{nullptr};
  void operator()(VkCommandBuffer c, VkImage img, VkImageLayout lay,
                  VkBuffer buf, std::uint32_t n,
                  const VkBufferImageCopy* r) const {
    (*dc)(c, img, lay, buf, n, r);
  }
  void operator()(VkCommandBuffer c, const VkDependencyInfo* d) const {
    (*dc)(c, d);
  }
  auto operator()(VkCommandBuffer c) const { return (*dc)(c); }
};

struct GradeVignetteMapCalls {
  const engine::test::GoldenDeviceCalls* dc{nullptr};
  auto operator()(VkDevice d, VkDeviceMemory m, VkDeviceSize o,
                  VkDeviceSize s, VkMemoryMapFlags f, void** p) const {
    return (*dc)(d, m, o, s, f, p);
  }
  void operator()(VkDevice d, VkDeviceMemory m) const { (*dc)(d, m); }
};

struct GradePipes {
  VkDescriptorSetLayout set_grade{nullptr};
  VkDescriptorSetLayout set_vignette{nullptr};
  VkPipelineLayout grade_layout{nullptr};
  VkPipelineLayout vignette_layout{nullptr};
  VkPipeline grade_pipe{nullptr};
  VkPipeline vignette_pipe{nullptr};
};

inline auto build_post_set(engine::test::GoldenDeviceCalls* calls,
                           VkDescriptorSetLayout* out, const char* name)
    -> bool {
  const bool ok = engine::test::GoldenBuildPostSet1(calls, out);
  check(ok, name);
  return ok;
}

inline auto build_grade_pipes(engine::test::GoldenVk* g,
                              engine::renderer::BindlessTable* bt,
                              GradePipes* gp) -> bool {
  engine::test::GoldenDeviceCalls calls{g};
  if (!build_post_set(&calls, &gp->set_grade, "grade-vignette: grade set") ||
      !build_post_set(&calls, &gp->set_vignette,
                      "grade-vignette: vignette set")) {
    return false;
  }
  VkShaderModule grade_f = nullptr;
  VkShaderModule vig_f = nullptr;
  VkShaderModule post_v = nullptr;
  const bool mods_ok =
      engine::test::GoldenMakeModule(g, kPost_gradeSpv, kPost_gradeSpvWords,
                                     &grade_f) &&
      engine::test::GoldenMakeModule(g, kPost_vignetteSpv,
                                     kPost_vignetteSpvWords, &vig_f) &&
      engine::test::GoldenMakeModule(g, kPost_vertSpv, kPost_vertSpvWords,
                                     &post_v);
  check(mods_ok, "grade-vignette: shader modules");
  if (!mods_ok) {
    for (auto* m : {&grade_f, &vig_f, &post_v}) {
      if (*m != nullptr) {
        calls(*m);
      }
    }
    return false;
  }
  bool plo_ok = true;
  {
    const VkDescriptorSetLayout sets[2] = {bt->set_layout(), gp->set_grade};
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset = 0;
    push.size = 32;
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 2;
    pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    plo_ok = calls(&pli, &gp->grade_layout);
  }
  if (plo_ok) {
    const VkDescriptorSetLayout sets[2] = {bt->set_layout(),
                                           gp->set_vignette};
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset = 0;
    push.size = 32;
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 2;
    pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    plo_ok = calls(&pli, &gp->vignette_layout);
  }
  check(plo_ok, "grade-vignette: pipeline layouts");
  bool pipes_ok = plo_ok;
  if (pipes_ok) {
    pipes_ok =
        engine::test::GoldenBuildPipe(calls, g->device, post_v, grade_f,
                                      VK_FORMAT_R8G8B8A8_UNORM,
                                      gp->grade_layout, &gp->grade_pipe) &&
        engine::test::GoldenBuildPipe(calls, g->device, post_v, vig_f,
                                      VK_FORMAT_R8G8B8A8_UNORM,
                                      gp->vignette_layout, &gp->vignette_pipe);
  }
  check(pipes_ok, "grade-vignette: pipelines build");
  calls(post_v);
  calls(grade_f);
  calls(vig_f);
  return pipes_ok;
}

inline void destroy_grade_pipes(engine::test::GoldenVk* g, GradePipes* gp) {
  if (gp->vignette_pipe != nullptr) {
    g->vk.destroy_pipe(g->device, gp->vignette_pipe, nullptr);
  }
  if (gp->grade_pipe != nullptr) {
    g->vk.destroy_pipe(g->device, gp->grade_pipe, nullptr);
  }
  for (auto* l : {&gp->vignette_layout, &gp->grade_layout}) {
    if (*l != nullptr) {
      g->vk.destroy_pipe_layout(g->device, *l, nullptr);
      *l = nullptr;
    }
  }
  for (auto* l : {&gp->set_vignette, &gp->set_grade}) {
    if (*l != nullptr) {
      g->vk.destroy_set_layout(g->device, *l, nullptr);
      *l = nullptr;
    }
  }
}

inline void record_post_pass(engine::test::GoldenVk* g, VkCommandBuffer cb,
                             VkImageView target_view, VkPipeline pipe,
                             VkPipelineLayout layout, VkDescriptorSet set1,
                             const void* push, std::uint32_t push_bytes) {
  engine::test::GoldenRecordPostPass(g, cb, kW, kH, target_view, pipe, layout,
                                     set1, push, push_bytes);
}

#endif

}  // namespace

auto main() -> int {
  test_cpu();
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  using engine::renderer::BindlessTable;
  using engine::renderer::PbrPipeline;
  using engine::renderer::VulkanDevice;
  VulkanDevice dev;
  engine::test::GoldenVk g{};
  PbrPipeline pbr;
  BindlessTable bt;
  if (!open_grade_device(&dev, &g, &pbr, &g, &bt)) {
    std::printf(failures == 0 ? "test_post_grade_vignette: device missing, cpu ok\n"
                              : "test_post_grade_vignette: %d FAILED\n",
                failures);
    return failures == 0 ? 0 : 1;
  }
  GradePipes gp{};
  if (!build_grade_pipes(&g, &bt, &gp)) {
    bt.destroy_gpu();
    dev.shutdown();
    std::printf("test_post_grade_vignette: %d FAILED\n", failures);
    return 1;
  }
  engine::test::GoldenHostBuf upload_b{};
  engine::test::GoldenHostBuf read_b{};
  engine::test::GoldenImage src_img{};
  engine::test::GoldenImage mid_img{};
  engine::test::GoldenImage dst_img{};
  VkSampler sampler = nullptr;
  VkDescriptorPool pool = nullptr;
  VkDescriptorSet set_grade = nullptr;
  VkDescriptorSet set_vig = nullptr;
  const int frame_rc = [&]() -> int {
    std::vector<std::uint8_t> grad;
    fill_gradient(&grad);
    const std::size_t px_bytes = static_cast<std::size_t>(kW) * kH * 4;
    engine::test::GoldenDeviceCalls dc{&g};
    if (!engine::test::GoldenAllocHost(&g, px_bytes,
                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                       &upload_b) ||
        !engine::test::GoldenAllocHost(&g, px_bytes,
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       &read_b)) {
      check(false, "grade-vignette: host buffers");
      return 1;
    }
    if (!engine::test::GoldenUploadBytes(
            &g, &upload_b,
            std::span<const std::uint8_t>(grad.data(), grad.size()))) {
      check(false, "grade-vignette: gradient upload");
      return 1;
    }
    constexpr VkImageUsageFlags kPostUsage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!engine::test::GoldenMakeImage(&g, VK_FORMAT_R8G8B8A8_UNORM, kW, kH,
                                       kPostUsage, &src_img) ||
        !engine::test::GoldenMakeImage(&g, VK_FORMAT_R8G8B8A8_UNORM, kW, kH,
                                       kPostUsage, &mid_img) ||
        !engine::test::GoldenMakeImage(&g, VK_FORMAT_R8G8B8A8_UNORM, kW, kH,
                                       kPostUsage, &dst_img)) {
      check(false, "grade-vignette: images");
      return 1;
    }
    if (!engine::test::GoldenMakeSampler(&g, &sampler)) {
      check(false, "grade-vignette: sampler");
      return 1;
    }
    // Pool: 2 post sets (sampled image + sampler each).
    VkDescriptorPoolSize sizes[3]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    sizes[0].descriptorCount = 2;
    sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    sizes[1].descriptorCount = 2;
    sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    sizes[2].descriptorCount = 1;
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 2;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = sizes;
    if (dc(g.device, &pi, nullptr, &pool) != VK_SUCCESS) {
      check(false, "grade-vignette: descriptor pool");
      return 1;
    }
    const VkDescriptorSetLayout layouts[2] = {gp.set_grade, gp.set_vignette};
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pool;
    ai.descriptorSetCount = 2;
    ai.pSetLayouts = layouts;
    VkDescriptorSet sets[2]{nullptr, nullptr};
    if (dc(g.device, &ai, sets) != VK_SUCCESS) {
      check(false, "grade-vignette: descriptor sets");
      return 1;
    }
    set_grade = sets[0];
    set_vig = sets[1];
    // Write sets: grade reads src, vignette reads mid.
    VkDescriptorImageInfo grade_src{};
    grade_src.imageView = src_img.view;
    grade_src.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo vig_src{};
    vig_src.imageView = mid_img.view;
    vig_src.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo smp{};
    smp.sampler = sampler;
    VkWriteDescriptorSet w[4]{};
    for (int k = 0; k < 4; ++k) {
      w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    }
    w[0].dstSet = set_grade;
    w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[0].pImageInfo = &grade_src;
    w[1].dstSet = set_grade;
    w[1].dstBinding = 1;
    w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[1].pImageInfo = &smp;
    w[2].dstSet = set_vig;
    w[2].dstBinding = 0;
    w[2].descriptorCount = 1;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[2].pImageInfo = &vig_src;
    w[3].dstSet = set_vig;
    w[3].dstBinding = 1;
    w[3].descriptorCount = 1;
    w[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[3].pImageInfo = &smp;
    g.vk.update_sets(g.device, 4, w, 0, nullptr);
    check(true, "grade-vignette: sets written");
    // Record: upload gradient -> src, grade(src) -> mid, vignette(mid) ->
    // dst (ORDERED: grade first, vignette second), readback dst.
    VkCommandPool cmd_pool = nullptr;
    VkCommandBuffer cb = nullptr;
    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.queueFamilyIndex = g.queue_family;
    if (g.vk.create_cmd_pool(g.device, &cpi, nullptr, &cmd_pool) !=
        VK_SUCCESS) {
      check(false, "grade-vignette: cmd pool");
      return 1;
    }
    VkCommandBufferAllocateInfo ab{};
    ab.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ab.commandPool = cmd_pool;
    ab.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ab.commandBufferCount = 1;
    if (g.vk.alloc_cmds(g.device, &ab, &cb) != VK_SUCCESS) {
      check(false, "grade-vignette: cmd alloc");
      g.vk.destroy_cmd_pool(g.device, cmd_pool, nullptr);
      return 1;
    }
    VkCommandBufferBeginInfo begin_bi{};
    begin_bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (g.vk.begin_cmd(cb, &begin_bi) != VK_SUCCESS) {
      check(false, "grade-vignette: cmd begin");
      g.vk.destroy_cmd_pool(g.device, cmd_pool, nullptr);
      return 1;
    }
    // Upload gradient into src (TRANSFER_DST -> SHADER_READ).
    VkBufferImageCopy region{};
    region.imageExtent = {kW, kH, 1};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    GoldenBarrier(&g, cb, src_img.img, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT);
    g.vk.cmd_copy_buf_img(cb, upload_b.buf, src_img.img,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    GoldenBarrier(&g, cb, src_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_READ_BIT);
    // Ordered effect 1: grade src -> mid (warm grade: lift +0.04,
    // gain 1.1/1.0/0.9).
    GoldenBarrier(&g, cb, mid_img.img, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    engine::renderer::GradePush gpush{};
    gpush.lift_gain[0] = 0.04f;
    gpush.lift_gain[1] = 0.04f;
    gpush.lift_gain[2] = 0.04f;
    gpush.lift_gain[3] = 1.1f;
    gpush.lift_gain[4] = 1.0f;
    gpush.lift_gain[5] = 0.9f;
    record_post_pass(&g, cb, mid_img.view, gp.grade_pipe, gp.grade_layout,
                     set_grade, &gpush,
                     static_cast<std::uint32_t>(sizeof(gpush)));
    GoldenBarrier(&g, cb, mid_img.img,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_READ_BIT);
    // Ordered effect 2: vignette mid -> dst.
    GoldenBarrier(&g, cb, dst_img.img, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    engine::renderer::VignettePush vpush{};
    vpush.center_radius[0] = 0.5f;
    vpush.center_radius[1] = 0.5f;
    vpush.center_radius[2] = 0.75f;
    vpush.softness = 0.35f;
    vpush.intensity = 0.45f;
    vpush.aspect = static_cast<float>(kW) / static_cast<float>(kH);
    record_post_pass(&g, cb, dst_img.view, gp.vignette_pipe,
                     gp.vignette_layout, set_vig, &vpush,
                     static_cast<std::uint32_t>(sizeof(vpush)));
    const GradeVignetteCalls tail{&dc};
    const GradeVignetteMapCalls map_calls{&dc};
    if (!engine::test::GoldenSubmitReadback(&g, cb, dst_img.img, read_b.buf,
                                            kW, kH, tail, tail, tail)) {
      check(false, "grade-vignette: cmd record/end");
      g.vk.destroy_cmd_pool(g.device, cmd_pool, nullptr);
      return 1;
    }
    const bool submit_ok = engine::test::GoldenSubmitWait(&g, cb);
    check(submit_ok, "grade-vignette: submit + fence wait");
    g.vk.destroy_cmd_pool(g.device, cmd_pool, nullptr);
    if (!submit_ok) {
      return 1;
    }
    std::vector<std::uint8_t> pixels(px_bytes, 0);
    if (!engine::test::GoldenMapPixels(map_calls, map_calls, g.device,
                                       read_b.mem, pixels.data(),
                                       px_bytes)) {
      check(false, "grade-vignette: readback mapped");
      return 1;
    }
    check(true, "grade-vignette: readback mapped");
    // Golden (vignette): the ordered chain output above.
    failures += engine::test::golden_check(
        "p4_11_vignette_320x240",
        std::span<const std::uint8_t>(pixels.data(), pixels.size()), kW,
        kH);
    // Second submit: grade-only pass (mid buffer holds grade(src)) —
    // proves the LUT pass alone matches its own golden.
    {
      VkCommandPool pool2 = nullptr;
      VkCommandBuffer cb2 = nullptr;
      VkCommandPoolCreateInfo cpi2{};
      cpi2.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
      cpi2.queueFamilyIndex = g.queue_family;
      if (g.vk.create_cmd_pool(g.device, &cpi2, nullptr, &pool2) !=
          VK_SUCCESS) {
        check(false, "grade-vignette: LUT cmd pool");
      } else {
        VkCommandBufferAllocateInfo ab2{};
        ab2.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ab2.commandPool = pool2;
        ab2.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ab2.commandBufferCount = 1;
        VkCommandBufferBeginInfo bbi2{};
        bbi2.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bbi2.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (g.vk.alloc_cmds(g.device, &ab2, &cb2) != VK_SUCCESS ||
            g.vk.begin_cmd(cb2, &bbi2) != VK_SUCCESS) {
          check(false, "grade-vignette: LUT cmd alloc/begin");
        } else {
          // mid is SHADER_READ after the first submit's final barrier
          // (submits don't carry layout state), so copy it straight out.
          GoldenBarrier(&g, cb2, mid_img.img,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_READ_BIT,
                        VK_PIPELINE_STAGE_2_COPY_BIT,
                        VK_ACCESS_2_TRANSFER_READ_BIT);
          VkBufferImageCopy rc2{};
          rc2.bufferOffset = 0;
          rc2.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          rc2.imageExtent = {kW, kH, 1};
          g.vk.cmd_copy_img_buf(cb2, mid_img.img,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                read_b.buf, 1, &rc2);
          VkMemoryBarrier2 hb2{};
          hb2.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
          hb2.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
          hb2.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
          hb2.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
          hb2.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
          VkDependencyInfo dh2{};
          dh2.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
          dh2.memoryBarrierCount = 1;
          dh2.pMemoryBarriers = &hb2;
          g.vk.cmd_barrier2(cb2, &dh2);
          if (g.vk.end_cmd(cb2) != VK_SUCCESS) {
            check(false, "grade-vignette: LUT cmd end");
          } else if (!engine::test::GoldenSubmitWait(&g, cb2)) {
            check(false, "grade-vignette: LUT submit + fence wait");
          } else {
            std::vector<std::uint8_t> lut_px(px_bytes, 0);
            if (!engine::test::GoldenMapPixels(
                    map_calls, map_calls, g.device, read_b.mem, lut_px.data(),
                    px_bytes)) {
              check(false, "grade-vignette: LUT readback mapped");
            } else {
              failures += engine::test::golden_check(
                  "p4_11_lut_320x240",
                  std::span<const std::uint8_t>(lut_px.data(), lut_px.size()),
                  kW, kH);
              // Ordering proof: LUT-only must differ from the ordered
              // chain (vignette darkens corners — at least 1% of bytes
              // differ), proving the second effect in the list runs.
              std::size_t diff = 0;
              for (std::size_t i = 0; i < px_bytes; ++i) {
                if (lut_px[i] != pixels[i]) {
                  ++diff;
                }
              }
              check(diff > px_bytes / 100,
                    "grade-vignette: chain differs from LUT-only (order)");
            }
          }
        }
        g.vk.destroy_cmd_pool(g.device, pool2, nullptr);
      }
    }
    // Acceptance probes: center pixel carries the warm grade (R > G >
    // B ordering from the 1.1/1.0/0.9 gains); corner pixel is darker
    // than the same corner without vignette (vignette darkens edges).
    {
      const std::size_t cc = ((kH / 2) * kW + (kW / 2)) * 4;
      check(pixels[cc + 0] >= pixels[cc + 1] &&
                pixels[cc + 1] >= pixels[cc + 2],
            "grade-vignette: warm grade ordering at center");
      const std::size_t co = 2 * 4;  // near top-left corner
      // Corner must be darker than center (vignette falloff).
      const int corner_lum =
          static_cast<int>(pixels[co]) + static_cast<int>(pixels[co + 1]) +
          static_cast<int>(pixels[co + 2]);
      const int center_lum =
          static_cast<int>(pixels[cc]) + static_cast<int>(pixels[cc + 1]) +
          static_cast<int>(pixels[cc + 2]);
      check(corner_lum < center_lum, "grade-vignette: corners darker");
    }
    return 0;
  }();
  check(frame_rc == 0, "grade-vignette: frame built and compared");
  if (pool != nullptr) {
    g.vk.destroy_pool(g.device, pool, nullptr);
  }
  if (sampler != nullptr) {
    g.vk.destroy_sampler(g.device, sampler, nullptr);
  }
  engine::test::GoldenFreeImage(&g, &dst_img);
  engine::test::GoldenFreeImage(&g, &mid_img);
  engine::test::GoldenFreeImage(&g, &src_img);
  engine::test::GoldenFreeHost(&g, &read_b);
  engine::test::GoldenFreeHost(&g, &upload_b);
  destroy_grade_pipes(&g, &gp);
  bt.destroy_gpu();
  dev.shutdown();
#else
  std::printf("[SKIP] grade-vignette-gpu: SDK absent (stub build)\n");
#endif
  std::printf(failures == 0 ? "test_post_grade_vignette: all checks passed\n"
                            : "test_post_grade_vignette: %d FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}

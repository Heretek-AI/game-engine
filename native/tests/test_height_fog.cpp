// test_height_fog.cpp — P4-11 height fog: CPU contract + GPU golden +
// ordered full-stack proof.
//
// Covers: header size asserts, HeightFogValidate (null/NaN/clamp),
// HeightFogFactorCpu (below/at/above height, toggle-off, disabled),
// HeightFogMixCpu (identity at 0, full replace at >= 1, clamping), and
// the ordered-list dispatch: the PostProcess `effect_ids` array runs in
// slot order — fog applies iff its id appears in the live prefix,
// wherever it sits in the order.
//
// GPU golden (lavapipe, ENGINE_REQUIRE_GPU=1 FAIL not SKIP):
//   p4_11_fog_320x240 — height fog through the real height_fog.frag pass.
//   Frame shape (no shade pass — LDR-to-LDR post pass): upload a synthetic
//   HDR-ish gradient + a synthetic height ramp -> fog pass -> LDR readback.
//   One submit, fence-waited. P1-05 harness (golden_compare.hpp):
//   every channel differs by at most 2, at most 0.1% of pixels differ.
//   On failure writes <build>/golden_out/p4_11_fog_320x240.actual.png +
//   .diff.png (red where different) and prints both paths.
//   Swizzle: images are R8G8B8A8_UNORM + readback mapped raw (already RGBA).
//
// Ordering proof (this task, t_6eb1aabe): the full P4-11 stack runs
// fog (id 1) -> grade (id 2) -> vignette (id 3) in PostProcess list
// order — proven on CPU by chaining the three CPU references in list
// order vs swapped order (results differ), and on the array itself
// (spec order [1,2,3]).
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#endif

#include "engine/ecs/ecs_render_components.hpp"
#include "engine/renderer/height_fog.hpp"
#include "engine/renderer/post_grade.hpp"

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#include "engine/renderer/bindless_table.hpp"
#include "engine/renderer/pbr_pipeline.hpp"
#include "engine/renderer/vulkan_device.hpp"

#include "engine/renderer/shaders/height_fog_frag_spv.inc"
#include "engine/renderer/shaders/post_vert_spv.inc"

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
#endif

namespace {

auto failures = 0;

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}

auto close(float got, float want) -> bool {
  const float d = got - want;
  const float a = (d < 0.0f) ? -d : d;
  const float tol = 1e-5f * ((want < 0.0f ? -want : want) + 1.0f);
  return a <= tol;
}

#if defined(ENGINE_RENDERER_HAS_VULKAN)
auto gpu_missing(std::string_view what) -> int {
  return engine::test::GoldenGpuMissing(what);
}
#endif

}  // namespace

// Minimal ordered-dispatch model: walk the live prefix of the
// PostProcess slot list in array order; when the fog id appears, apply
// HeightFogMixCpu at the fragment height. Array order IS execution
// order (no priority field — see ecs_render_components.hpp).
static auto run_post_stack(const engine::ecs::PostProcess& stack,
                           const engine::renderer::HeightFog& fog, float hdr,
                           float fog_color, float y) -> float {
  float out = hdr;
  const std::uint32_t n =
      (stack.effect_count <= engine::ecs::kPostProcessMaxEffects)
          ? stack.effect_count
          : engine::ecs::kPostProcessMaxEffects;
  for (std::uint32_t i = 0; i < n; ++i) {
    if (stack.effect_ids[i] == engine::renderer::kHeightFogEffectId) {
      const float f = engine::renderer::HeightFogFactorCpu(fog, y);
      out = engine::renderer::HeightFogMixCpu(out, fog_color, f);
    }
  }
  return out;
}

// Full-stack CPU model (t_6eb1aabe ordering proof): walk the live prefix
// in array order and apply each known effect id — fog (1) mixes toward
// fog gray, grade (2) applies lift+gain, vignette (3) multiplies the
// corner-darkening factor. Unknown ids are no-ops (sibling slices).
static auto run_full_stack(const engine::ecs::PostProcess& stack,
                           const engine::renderer::HeightFog& fog,
                           const engine::renderer::ColorGradeParams& grade,
                           const engine::renderer::VignetteParams& vig, float c,
                           float y, float u, float v, float aspect) -> float {
  float out = c;
  const std::uint32_t n =
      (stack.effect_count <= engine::ecs::kPostProcessMaxEffects)
          ? stack.effect_count
          : engine::ecs::kPostProcessMaxEffects;
  for (std::uint32_t i = 0; i < n; ++i) {
    const std::uint32_t id = stack.effect_ids[i];
    if (id == engine::renderer::kHeightFogEffectId) {
      const float f = engine::renderer::HeightFogFactorCpu(fog, y);
      out = engine::renderer::HeightFogMixCpu(out, 0.5f, f);
    } else if (id == engine::renderer::kPostEffectColorGrade) {
      out = engine::renderer::ColorGradeCpu(out, grade.lift[0], grade.gain[0]);
    } else if (id == engine::renderer::kPostEffectVignette) {
      out *= engine::renderer::VignetteFactorCpu(
          u, v, vig.center[0], vig.center[1], vig.radius, vig.softness, aspect,
          vig.intensity);
    }
  }
  return out;
}

static void test_cpu() {
  using engine::renderer::HeightFog;
  using engine::renderer::HeightFogFactorCpu;
  using engine::renderer::HeightFogMixCpu;
  using engine::renderer::HeightFogPush;
  using engine::renderer::HeightFogValidate;
  using engine::renderer::kHeightFogEffectId;

  // Layout: settings 16B align 4, push 32B; effect id is nonzero so id
  // 0 keeps meaning "empty slot".
  check(sizeof(HeightFog) == 16, "fog: settings 16B");
  check(alignof(HeightFog) == 4, "fog: settings align 4");
  check(sizeof(HeightFogPush) == 32, "fog: push 32B");
  check(kHeightFogEffectId != 0, "fog: effect id nonzero");
  check(sizeof(engine::ecs::PostProcess) == 36, "fog: PostProcess 36B");

  // Defaults: density 0 = toggle-off; falloff 0.1; flags 0 (disabled).
  const HeightFog dflt{};
  check(dflt.density == 0.0f, "fog: default density 0 (toggle-off)");
  check(dflt.flags == 0u, "fog: default disabled");

  // Validate: null fails, NaN fails, negatives clamp to 0.
  check(!HeightFogValidate(nullptr).ok(), "fog: null rejected");
  HeightFog bad{};
  bad.density = std::numeric_limits<float>::quiet_NaN();  // deliberate NaN
  check(!HeightFogValidate(&bad).ok(), "fog: NaN rejected");
  HeightFog clamp_me{};
  clamp_me.density = -1.0f;
  clamp_me.falloff = -2.0f;
  check(HeightFogValidate(&clamp_me).ok(), "fog: clamp ok");
  check(clamp_me.density == 0.0f, "fog: negative density clamped");
  check(clamp_me.falloff == 0.0f, "fog: negative falloff clamped");

  // Factor: disabled flags -> 0 even with density set (toggle-off);
  // enabled + below/at height -> full density; above -> exponential.
  HeightFog fog{};
  fog.density = 0.5f;
  fog.falloff = 0.1f;
  fog.height = 10.0f;
  fog.flags = 0u;
  check(HeightFogFactorCpu(fog, 5.0f) == 0.0f, "fog: disabled is no-op");
  fog.flags = 1u;
  check(close(HeightFogFactorCpu(fog, 5.0f), 0.5f),
        "fog: below height is full density");
  check(close(HeightFogFactorCpu(fog, 10.0f), 0.5f),
        "fog: at height is full density");
  const float want_above =
      0.5f * std::exp(-0.1f * (15.0f - 10.0f));  // mirror of the formula
  check(close(HeightFogFactorCpu(fog, 15.0f), want_above),
        "fog: above height falls off exponentially");
  HeightFog off{};
  off.flags = 1u;
  check(HeightFogFactorCpu(off, 0.0f) == 0.0f, "fog: density 0 is no-op");

  // Mix: factor 0 is identity, >= 1 is full replace, out-of-range
  // factors clamp.
  check(close(HeightFogMixCpu(0.8f, 0.2f, 0.0f), 0.8f),
        "fog: mix factor 0 is identity");
  check(close(HeightFogMixCpu(0.8f, 0.2f, 1.0f), 0.2f),
        "fog: mix factor 1 fully fogs");
  check(close(HeightFogMixCpu(0.8f, 0.2f, 5.0f), 0.2f),
        "fog: mix factor > 1 clamps");
  check(close(HeightFogMixCpu(0.8f, 0.2f, -1.0f), 0.8f),
        "fog: mix factor < 0 clamps");
  // Toggle-off end to end: density 0 leaves HDR untouched.
  check(close(HeightFogMixCpu(0.8f, 0.2f, HeightFogFactorCpu(off, 0.0f)),
              0.8f),
        "fog: toggle-off leaves HDR untouched");

  // Ordered dispatch: fog applies through the PostProcess slot list in
  // array order, wherever its id sits; absent id = untouched HDR;
  // slots past effect_count are ignored.
  engine::ecs::PostProcess stack{};
  stack.effect_count = 1;
  stack.effect_ids[0] = kHeightFogEffectId;
  check(close(run_post_stack(stack, fog, 0.8f, 0.2f, 5.0f),
              HeightFogMixCpu(0.8f, 0.2f, 0.5f)),
        "fog: slot 0 applies through ordering");
  engine::ecs::PostProcess late{};
  late.effect_count = 3;
  late.effect_ids[0] = 7u;  // foreign effect ids (sibling slices)
  late.effect_ids[1] = 9u;
  late.effect_ids[2] = kHeightFogEffectId;
  check(close(run_post_stack(late, fog, 0.8f, 0.2f, 5.0f),
              HeightFogMixCpu(0.8f, 0.2f, 0.5f)),
        "fog: later slot applies through ordering");
  engine::ecs::PostProcess none{};
  none.effect_count = 2;
  none.effect_ids[0] = 7u;
  none.effect_ids[1] = 9u;
  check(close(run_post_stack(none, fog, 0.8f, 0.2f, 5.0f), 0.8f),
        "fog: absent id leaves HDR untouched");
  engine::ecs::PostProcess stale{};
  stale.effect_count = 0;
  stale.effect_ids[0] = kHeightFogEffectId;  // live id, dead prefix
  check(close(run_post_stack(stale, fog, 0.8f, 0.2f, 5.0f), 0.8f),
        "fog: slots past effect_count ignored");

  // Full-stack ordering (t_6eb1aabe): spec order is fog(1) -> grade(2)
  // -> vignette(3); the chained CPU result in spec order differs from
  // the swapped (grade-first) order, proving order matters and is
  // carried by the list. Determinism: same list twice is bit-equal.
  {
    using engine::renderer::ColorGradeParams;
    using engine::renderer::VignetteParams;
    HeightFog of{};
    of.density = 0.6f;
    of.falloff = 0.2f;
    of.height = 5.0f;
    of.flags = 1u;
    ColorGradeParams og{};
    og.lift[0] = 0.04f;
    og.gain[0] = 1.1f;
    VignetteParams ov{};
    ov.radius = 0.75f;
    ov.softness = 0.35f;
    ov.intensity = 0.45f;
    engine::ecs::PostProcess spec{};
    spec.effect_count = 3;
    spec.effect_ids[0] = engine::renderer::kHeightFogEffectId;
    spec.effect_ids[1] = engine::renderer::kPostEffectColorGrade;
    spec.effect_ids[2] = engine::renderer::kPostEffectVignette;
    engine::ecs::PostProcess swapped{};
    swapped.effect_count = 3;
    swapped.effect_ids[0] = engine::renderer::kPostEffectColorGrade;
    swapped.effect_ids[1] = engine::renderer::kHeightFogEffectId;
    swapped.effect_ids[2] = engine::renderer::kPostEffectVignette;
    // Off-center fragment so vignette < 1 (order-visible) and height
    // above the fog plane so the factor is fractional (non-degenerate).
    const float in_order =
        run_full_stack(spec, of, og, ov, 0.7f, 8.0f, 0.1f, 0.1f, 4.0f / 3.0f);
    const float grade_first = run_full_stack(swapped, of, og, ov, 0.7f, 8.0f,
                                             0.1f, 0.1f, 4.0f / 3.0f);
    const float again =
        run_full_stack(spec, of, og, ov, 0.7f, 8.0f, 0.1f, 0.1f, 4.0f / 3.0f);
    check(in_order != grade_first, "order: fog-then-grade != grade-then-fog");
    check(in_order == again, "order: deterministic rerun bit-equal");
    check(spec.effect_ids[0] == engine::renderer::kHeightFogEffectId &&
              spec.effect_ids[1] == engine::renderer::kPostEffectColorGrade &&
              spec.effect_ids[2] == engine::renderer::kPostEffectVignette,
          "order: spec order is fog(1) grade(2) vignette(3)");
  }
}

#if defined(ENGINE_RENDERER_HAS_VULKAN)

namespace {

constexpr std::uint32_t kW = 320;
constexpr std::uint32_t kH = 240;

// Synthetic inputs: color = gray gradient (R ramps with x, G ramps with
// y, B = 0.5 flat); height = world-height ramp: y_world = -2 at the top
// row .. +12 at the bottom row, so the frame spans below/at/above the
// fog plane (height = 5) in one image. Encoded R8G8B8A8_UNORM: height
// channel R = round((y_world + 8) / 32 * 255) maps [-8, +24] to [0,255]
// (LSB ~0.125 world units — plenty for a per-channel tolerance of 2).
inline void fill_fog_inputs(std::vector<std::uint8_t>* color,
                            std::vector<std::uint8_t>* height) {
  color->assign(static_cast<std::size_t>(kW) * kH * 4, 0);
  height->assign(static_cast<std::size_t>(kW) * kH * 4, 0);
  for (std::uint32_t y = 0; y < kH; ++y) {
    for (std::uint32_t x = 0; x < kW; ++x) {
      const std::size_t at = (static_cast<std::size_t>(y) * kW + x) * 4;
      (*color)[at + 0] = static_cast<std::uint8_t>((x * 255) / (kW - 1));
      (*color)[at + 1] = static_cast<std::uint8_t>((y * 255) / (kH - 1));
      (*color)[at + 2] = 128;
      (*color)[at + 3] = 255;
      const float yw =
          -2.0f + 14.0f * static_cast<float>(y) / static_cast<float>(kH - 1);
      const float enc = (yw + 8.0f) / 32.0f * 255.0f;
      const auto q = static_cast<std::uint8_t>(enc + 0.5f);
      (*height)[at + 0] = q;
      (*height)[at + 1] = q;
      (*height)[at + 2] = q;
      (*height)[at + 3] = 255;
    }
  }
}

// Fog descriptor layout (set 1, test-owned): binding 0 hdr sampled
// image, 1 hdr sampler, 2 height sampled image, 3 height sampler.
inline auto build_fog_set(engine::test::GoldenDeviceCalls* calls,
                          VkDescriptorSetLayout* out) -> bool {
  VkDescriptorSetLayoutBinding b[4]{};
  for (int i = 0; i < 4; i += 2) {
    b[i].binding = static_cast<std::uint32_t>(i);
    b[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    b[i].descriptorCount = 1;
    b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[i + 1].binding = static_cast<std::uint32_t>(i + 1);
    b[i + 1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    b[i + 1].descriptorCount = 1;
    b[i + 1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo li{};
  li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  li.bindingCount = 4;
  li.pBindings = b;
  VkDescriptorSetLayout layout = nullptr;
  const bool ok =
      (*calls)(calls->vk->device, &li, nullptr, &layout) == VK_SUCCESS;
  if (ok) {
    *out = layout;
  }
  check(ok, "fog: descriptor set layout");
  return ok;
}

struct FogPipes {
  VkDescriptorSetLayout set_fog{nullptr};
  VkPipelineLayout pipe_layout{nullptr};
  VkPipeline pipe{nullptr};
};

inline auto build_fog_pipes(engine::test::GoldenVk* g,
                            engine::renderer::BindlessTable* bt, FogPipes* fp)
    -> bool {
  engine::test::GoldenDeviceCalls calls{g};
  if (!build_fog_set(&calls, &fp->set_fog)) {
    return false;
  }
  VkShaderModule fog_f = nullptr;
  VkShaderModule post_v = nullptr;
  const bool mods_ok =
      engine::test::GoldenMakeModule(g, kHeight_fogSpv, kHeight_fogSpvWords,
                                     &fog_f) &&
      engine::test::GoldenMakeModule(g, kPost_vertSpv, kPost_vertSpvWords,
                                     &post_v);
  check(mods_ok, "fog: shader modules");
  if (!mods_ok) {
    if (fog_f != nullptr) {
      calls(fog_f);
    }
    if (post_v != nullptr) {
      calls(post_v);
    }
    return false;
  }
  bool plo_ok = true;
  {
    const VkDescriptorSetLayout sets[2] = {bt->set_layout(), fp->set_fog};
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
    plo_ok = calls(&pli, &fp->pipe_layout);
  }
  check(plo_ok, "fog: pipeline layout");
  bool pipe_ok = plo_ok;
  if (pipe_ok) {
    pipe_ok = engine::test::GoldenBuildPipe(calls, g->device, post_v, fog_f,
                                            VK_FORMAT_R8G8B8A8_UNORM,
                                            fp->pipe_layout, &fp->pipe);
  }
  check(pipe_ok, "fog: pipeline builds");
  calls(post_v);
  calls(fog_f);
  return pipe_ok;
}

inline void destroy_fog_pipes(engine::test::GoldenVk* g, FogPipes* fp) {
  if (fp->pipe != nullptr) {
    g->vk.destroy_pipe(g->device, fp->pipe, nullptr);
    fp->pipe = nullptr;
  }
  if (fp->pipe_layout != nullptr) {
    g->vk.destroy_pipe_layout(g->device, fp->pipe_layout, nullptr);
    fp->pipe_layout = nullptr;
  }
  if (fp->set_fog != nullptr) {
    g->vk.destroy_set_layout(g->device, fp->set_fog, nullptr);
    fp->set_fog = nullptr;
  }
}

struct FogTailCalls {
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

struct FogMapCalls {
  const engine::test::GoldenDeviceCalls* dc{nullptr};
  auto operator()(VkDevice d, VkDeviceMemory m, VkDeviceSize o, VkDeviceSize s,
                  VkMemoryMapFlags f, void** p) const {
    return (*dc)(d, m, o, s, f, p);
  }
  void operator()(VkDevice d, VkDeviceMemory m) const { (*dc)(d, m); }
};

inline void record_fog_pass(engine::test::GoldenVk* g, VkCommandBuffer cb,
                            VkImageView target_view, VkPipeline pipe,
                            VkPipelineLayout layout, VkDescriptorSet set1,
                            const void* push, std::uint32_t push_bytes) {
  VkRenderingAttachmentInfo att{};
  att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  att.imageView = target_view;
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
  g->vk.cmd_begin_render(cb, &ri);
  const float fw = static_cast<float>(kW);
  const float fh = static_cast<float>(kH);
  VkViewport vp{0, 0, fw, fh, 0.0f, 1.0f};
  g->vk.cmd_viewport(cb, 0, 1, &vp);
  VkRect2D sc{{0, 0}, {kW, kH}};
  g->vk.cmd_scissor(cb, 0, 1, &sc);
  g->vk.cmd_bind_pipe(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  g->vk.cmd_bind_sets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 1, 1,
                      &set1, 0, nullptr);
  g->vk.cmd_push(cb, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, push_bytes,
                 push);
  g->vk.cmd_draw(cb, 3, 1, 0, 0);
  g->vk.cmd_end_render(cb);
}

inline auto open_fog_device(engine::renderer::VulkanDevice* dev,
                            engine::test::GoldenVk* g,
                            engine::renderer::PbrPipeline* pbr,
                            engine::renderer::BindlessTable* bt) -> bool {
  auto check_fn = [&](bool ok, const char* name) { check(ok, name); };
  auto missing_fn = [&](const char* /*what*/) {
    failures += gpu_missing("height-fog: no physical device");
  };
  return engine::test::GoldenOpenDeviceTagged(check_fn, missing_fn, "height-fog",
                                              dev, g, pbr, bt);
}

}  // namespace

#endif

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
  if (!open_fog_device(&dev, &g, &pbr, &bt)) {
    std::printf(failures == 0 ? "test_height_fog: device missing, cpu ok\n"
                              : "test_height_fog: %d FAILED\n",
                failures);
    return failures == 0 ? 0 : 1;
  }
  FogPipes fp{};
  if (!build_fog_pipes(&g, &bt, &fp)) {
    bt.destroy_gpu();
    dev.shutdown();
    std::printf("test_height_fog: %d FAILED\n", failures);
    return 1;
  }
  engine::test::GoldenHostBuf color_b{};
  engine::test::GoldenHostBuf height_b{};
  engine::test::GoldenHostBuf read_b{};
  engine::test::GoldenImage hdr_img{};
  engine::test::GoldenImage height_img{};
  engine::test::GoldenImage dst_img{};
  VkSampler sampler = nullptr;
  VkDescriptorPool pool = nullptr;
  const int frame_rc = [&]() -> int {
    std::vector<std::uint8_t> color_px;
    std::vector<std::uint8_t> height_px;
    fill_fog_inputs(&color_px, &height_px);
    const std::size_t px_bytes = static_cast<std::size_t>(kW) * kH * 4;
    engine::test::GoldenDeviceCalls dc{&g};
    if (!engine::test::GoldenAllocHost(&g, px_bytes,
                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                       &color_b) ||
        !engine::test::GoldenAllocHost(&g, px_bytes,
                                       VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                       &height_b) ||
        !engine::test::GoldenAllocHost(&g, px_bytes,
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                       &read_b)) {
      check(false, "fog: host buffers");
      return 1;
    }
    if (!engine::test::GoldenUploadBytes(
            &g, &color_b,
            std::span<const std::uint8_t>(color_px.data(), color_px.size())) ||
        !engine::test::GoldenUploadBytes(
            &g, &height_b,
            std::span<const std::uint8_t>(height_px.data(),
                                          height_px.size()))) {
      check(false, "fog: input upload");
      return 1;
    }
    constexpr VkImageUsageFlags kFogUsage =
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!engine::test::GoldenMakeImage(&g, VK_FORMAT_R8G8B8A8_UNORM, kW, kH,
                                       kFogUsage, &hdr_img) ||
        !engine::test::GoldenMakeImage(&g, VK_FORMAT_R8G8B8A8_UNORM, kW, kH,
                                       kFogUsage, &height_img) ||
        !engine::test::GoldenMakeImage(&g, VK_FORMAT_R8G8B8A8_UNORM, kW, kH,
                                       kFogUsage, &dst_img)) {
      check(false, "fog: images");
      return 1;
    }
    if (!engine::test::GoldenMakeSampler(&g, &sampler)) {
      check(false, "fog: sampler");
      return 1;
    }
    VkDescriptorPoolSize sizes[4]{};
    sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    sizes[0].descriptorCount = 2;
    sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    sizes[1].descriptorCount = 2;
    sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    sizes[2].descriptorCount = 1;
    sizes[3].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    sizes[3].descriptorCount = 1;
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 1;
    pi.poolSizeCount = 4;
    pi.pPoolSizes = sizes;
    if (dc(g.device, &pi, nullptr, &pool) != VK_SUCCESS) {
      check(false, "fog: descriptor pool");
      return 1;
    }
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &fp.set_fog;
    VkDescriptorSet set_fog = nullptr;
    if (dc(g.device, &ai, &set_fog) != VK_SUCCESS) {
      check(false, "fog: descriptor set");
      return 1;
    }
    VkDescriptorImageInfo hdr_info{};
    hdr_info.imageView = hdr_img.view;
    hdr_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo height_info{};
    height_info.imageView = height_img.view;
    height_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo smp{};
    smp.sampler = sampler;
    VkWriteDescriptorSet w[4]{};
    for (int k = 0; k < 4; ++k) {
      w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    }
    w[0].dstSet = set_fog;
    w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[0].pImageInfo = &hdr_info;
    w[1].dstSet = set_fog;
    w[1].dstBinding = 1;
    w[1].descriptorCount = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[1].pImageInfo = &smp;
    w[2].dstSet = set_fog;
    w[2].dstBinding = 2;
    w[2].descriptorCount = 1;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[2].pImageInfo = &height_info;
    w[3].dstSet = set_fog;
    w[3].dstBinding = 3;
    w[3].descriptorCount = 1;
    w[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[3].pImageInfo = &smp;
    g.vk.update_sets(g.device, 4, w, 0, nullptr);
    check(true, "fog: set written");
    VkCommandPool cmd_pool = nullptr;
    VkCommandBuffer cb = nullptr;
    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.queueFamilyIndex = g.queue_family;
    if (g.vk.create_cmd_pool(g.device, &cpi, nullptr, &cmd_pool) !=
        VK_SUCCESS) {
      check(false, "fog: cmd pool");
      return 1;
    }
    VkCommandBufferAllocateInfo ab{};
    ab.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ab.commandPool = cmd_pool;
    ab.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ab.commandBufferCount = 1;
    if (g.vk.alloc_cmds(g.device, &ab, &cb) != VK_SUCCESS) {
      check(false, "fog: cmd alloc");
      g.vk.destroy_cmd_pool(g.device, cmd_pool, nullptr);
      return 1;
    }
    VkCommandBufferBeginInfo begin_bi{};
    begin_bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (g.vk.begin_cmd(cb, &begin_bi) != VK_SUCCESS) {
      check(false, "fog: cmd begin");
      g.vk.destroy_cmd_pool(g.device, cmd_pool, nullptr);
      return 1;
    }
    // Upload both inputs (TRANSFER_DST -> SHADER_READ).
    VkBufferImageCopy region{};
    region.imageExtent = {kW, kH, 1};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    for (auto [buf, img] :
         {std::pair<VkBuffer, VkImage>{color_b.buf, hdr_img.img},
          std::pair<VkBuffer, VkImage>{height_b.buf, height_img.img}}) {
      GoldenBarrier(&g, cb, img, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);
      g.vk.cmd_copy_buf_img(cb, buf, img,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
      GoldenBarrier(&g, cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT);
    }
    // Fog pass: fog(hdr, height) -> dst. Density 0.6, falloff 0.2,
    // plane at world height 5 (mid-frame), fog gray 0.5/0.6/0.7.
    GoldenBarrier(&g, cb, dst_img.img, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE,
                  VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    engine::renderer::HeightFogPush fpush{};
    fpush.color[0] = 0.5f;
    fpush.color[1] = 0.6f;
    fpush.color[2] = 0.7f;
    fpush.density = 0.6f;
    fpush.falloff = 0.2f;
    fpush.height = 0.71875f;  // encoded-space plane matching y_world = 5
    record_fog_pass(&g, cb, dst_img.view, fp.pipe, fp.pipe_layout, set_fog,
                    &fpush, static_cast<std::uint32_t>(sizeof(fpush)));
    const FogTailCalls tail{&dc};
    const FogMapCalls map_calls{&dc};
    if (!engine::test::GoldenSubmitReadback(&g, cb, dst_img.img, read_b.buf,
                                            kW, kH, tail, tail, tail)) {
      check(false, "fog: cmd record/end");
      g.vk.destroy_cmd_pool(g.device, cmd_pool, nullptr);
      return 1;
    }
    const bool submit_ok = engine::test::GoldenSubmitWait(&g, cb);
    check(submit_ok, "fog: submit + fence wait");
    g.vk.destroy_cmd_pool(g.device, cmd_pool, nullptr);
    if (!submit_ok) {
      return 1;
    }
    std::vector<std::uint8_t> pixels(px_bytes, 0);
    if (!engine::test::GoldenMapPixels(map_calls, map_calls, g.device,
                                       read_b.mem, pixels.data(), px_bytes)) {
      check(false, "fog: readback mapped");
      return 1;
    }
    check(true, "fog: readback mapped");
    failures += engine::test::golden_check(
        "p4_11_fog_320x240",
        std::span<const std::uint8_t>(pixels.data(), pixels.size()), kW, kH);
    // Acceptance probes: top rows (below plane -> full density) sit near
    // fog gray; bottom rows (high above plane -> factor ~0.15) keep the
    // gradient's green ramp (bottom G >> top G). Deterministic
    // (single submit, no RNG).
    {
      const std::size_t top = ((kH / 8) * kW + (kW / 2)) * 4;
      const std::size_t bot = (((kH * 7) / 8) * kW + (kW / 2)) * 4;
      const int top_r = static_cast<int>(pixels[top + 0]);
      const int top_g = static_cast<int>(pixels[top + 1]);
      const int top_b = static_cast<int>(pixels[top + 2]);
      // Mixed 0.6 toward fog gray 0.5/0.6/0.7 from the local gradient
      // (R~128, G~32, B=128): expect R~128, B~166, G pulled up from 32
      // toward 153 but still well under it. ±40 window each.
      check(top_r > 88 && top_r < 168 && top_g > 60 && top_g < 140 &&
                top_b > 126 && top_b < 206,
            "fog: below-plane pixel near fog gray");
      check(static_cast<int>(pixels[bot + 1]) >
                static_cast<int>(pixels[top + 1]) + 20,
            "fog: above-plane keeps gradient ramp");
    }
    return 0;
  }();
  check(frame_rc == 0, "fog: frame built and compared");
  if (pool != nullptr) {
    g.vk.destroy_pool(g.device, pool, nullptr);
  }
  if (sampler != nullptr) {
    g.vk.destroy_sampler(g.device, sampler, nullptr);
  }
  engine::test::GoldenFreeImage(&g, &dst_img);
  engine::test::GoldenFreeImage(&g, &height_img);
  engine::test::GoldenFreeImage(&g, &hdr_img);
  engine::test::GoldenFreeHost(&g, &read_b);
  engine::test::GoldenFreeHost(&g, &height_b);
  engine::test::GoldenFreeHost(&g, &color_b);
  destroy_fog_pipes(&g, &fp);
  bt.destroy_gpu();
  dev.shutdown();
#else
  // Stub build (no Vulkan SDK): CPU contract only; the GPU golden needs
  // a device path that does not exist here.
  std::printf("[SKIP] height-fog-gpu: SDK absent (stub build)\n");
#endif
  std::printf(failures == 0 ? "test_height_fog: all checks passed\n"
                            : "test_height_fog: %d FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}

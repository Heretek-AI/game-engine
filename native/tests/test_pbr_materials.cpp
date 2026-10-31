// test_pbr_materials.cpp — P4-01 acceptance (native CTest).
//
// CPU-side contract (always built, no GPU needed):
//   1. PbrMaterialLibrary: init/add/remove/at, slot reuse, full/double
//      guards, null guards; to_scene_record() packs the exact 64B
//      SceneMaterial field order (bit-exact u32 lanes for slots/flags).
//   2. PbrValidateMaterial: NaN/Inf fails closed, negatives clamp,
//      metallic/roughness clamp to [0,1], blend+mask rejected.
//   3. PbrSrgbToLinear: 0->0, 1->1, mid-grey exact vs the glTF formula.
//   4. PbrImportMaterial(s): synthesized .gltf JSON (parsed with the real
//      cgltf when ENGINE_ASSETS_HAS_CGLTF) converts factors (sRGB->linear),
//      alpha modes, double-sided, emissive strength, and texture roles via
//      a slot-resolver callback; OOB/null fail closed with no partial
//      append. Without cgltf the import section reports kUnsupported
//      (offline degrade — the GPU section still runs).
//   5. MaterialComponent: 64B blittable twin (size asserted in header).
//   6. Real-GPU path (Lavapipe, device_hint "llvmpipe"): two pipelines
//      (shade with the P4-01 material SSBO + tex/sampler set-1 bindings,
//      post with set-1 HDR input) build against the bindless layout; a
//      5x5 sphere grid (metallic x, roughness y) renders to an offscreen
//      RGBA8 image through vis-fill -> shade -> post; readback compares
//      against golden p4_01_spheres_320x240 via golden_compare.hpp
//      (P1-05 tolerance). [FAIL] (not [SKIP]) under ENGINE_REQUIRE_GPU=1.
//
// Naming note: check() helpers mirror test_slang_pbr.cpp (own TU, no
// shared test lib in this repo) — check_tagged avoids printf-family
// formatting per Sonar cpp:S3458.
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#endif

#include "engine/renderer/bindless_table.hpp"
#include "engine/renderer/material_import.hpp"
#include "engine/renderer/pbr_material.hpp"
#include "engine/renderer/pbr_pipeline.hpp"
#include "engine/renderer/scene_ssbo.hpp"
#include "engine/renderer/vulkan_device.hpp"

#include "engine/renderer/shaders/pbr_shade_frag_spv.inc"
#include "engine/renderer/shaders/pbr_shade_vert_spv.inc"
#include "engine/renderer/shaders/pbr_vis_frag_spv.inc"
#include "engine/renderer/shaders/pbr_vis_vert_spv.inc"
#include "engine/renderer/shaders/post_frag_spv.inc"
#include "engine/renderer/shaders/post_vert_spv.inc"

#include "golden_compare.hpp"

#if defined(ENGINE_ASSETS_HAS_CGLTF)
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wunused-function"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
#endif

namespace {

auto failures = 0;

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}


using engine::renderer::MaterialComponent;
using engine::renderer::PbrMaterialAsset;
using engine::renderer::PbrMaterialLibrary;
using engine::renderer::PbrTextureRole;

void test_library() {
  PbrMaterialLibrary lib;
  check(!lib.ready(), "matlib: not ready before init");
  check(lib.init().ok(), "matlib: init ok");
  check(lib.init().ok(), "matlib: double init ok");
  check(lib.ready(), "matlib: ready after init");
  check(lib.count() == 0, "matlib: empty count");
  check(lib.at(0) == nullptr, "matlib: at empty null");
  std::uint32_t s0 = 0xFFFFFFFFu;
  PbrMaterialAsset a{};
  a.base_color = {1.0f, 0.5f, 0.25f, 1.0f};
  a.metallic_roughness = {0.7f, 0.3f, 0.0f, 0.0f};
  a.emissive = {0.1f, 0.2f, 0.3f, 0.0f};
  a.texture_slots[0] = 7;
  a.sampler_index = 2;
  a.flags = engine::renderer::kPbrFlagDoubleSided;
  check(lib.add(a, &s0).ok(), "matlib: add ok");
  check(s0 == 0, "matlib: first slot 0");
  check(lib.count() == 1, "matlib: count 1");
  const PbrMaterialAsset* got = lib.at(s0);
  check(got != nullptr, "matlib: at live");
  check(got != nullptr && got->base_color[1] == 0.5f, "matlib: roundtrip g");
  check(got != nullptr && got->texture_slots[0] == 7, "matlib: slot kept");
  // Record packing: exact SceneMaterial field order.
  float rec[16]{};
  check(PbrMaterialLibrary::to_scene_record(a, rec).ok(),
        "matlib: record ok");
  const bool floats_ok =
      rec[0] == 1.0f && rec[1] == 0.5f && rec[2] == 0.25f && rec[3] == 1.0f &&
      rec[4] == 0.7f && rec[5] == 0.3f && rec[12] == 0.1f &&
      rec[13] == 0.2f && rec[14] == 0.3f;
  check(floats_ok, "matlib: record floats");
  const auto* bits = reinterpret_cast<const std::uint32_t*>(&rec[8]);
  check(bits[0] == 7 && bits[1] == 2 &&
            bits[2] == engine::renderer::kPbrFlagDoubleSided && bits[3] == 0,
        "matlib: record u32 lanes bit-exact");
  check(!PbrMaterialLibrary::to_scene_record(a, nullptr).ok(),
        "matlib: record null rejected");
  // Remove + slot reuse.
  check(lib.remove(s0).ok(), "matlib: remove ok");
  check(lib.count() == 0, "matlib: count 0 after remove");
  check(lib.at(s0) == nullptr, "matlib: at removed null");
  check(!lib.remove(s0).ok(), "matlib: double remove rejected");
  check(!lib.remove(4096).ok(), "matlib: OOB remove rejected");
  std::uint32_t s1 = 0xFFFFFFFFu;
  check(lib.add(a, &s1).ok(), "matlib: re-add ok");
  check(s1 == 0, "matlib: slot reused");
  check(!lib.add(a, nullptr).ok(), "matlib: null slot-out rejected");
  lib.shutdown();
  check(!lib.ready(), "matlib: shutdown clears");
  check(!lib.add(a, &s1).ok(), "matlib: add after shutdown rejected");
}

void test_validate() {
  using engine::renderer::PbrValidateMaterial;
  check(!PbrValidateMaterial(nullptr).ok(), "matval: null rejected");
  PbrMaterialAsset ok{};
  check(PbrValidateMaterial(&ok).ok(), "matval: default ok");
  PbrMaterialAsset nan{};
  nan.base_color[2] = std::nanf("");
  check(!PbrValidateMaterial(&nan).ok(), "matval: NaN rejected");
  PbrMaterialAsset inf{};
  inf.emissive[0] = std::numeric_limits<float>::infinity();
  check(!PbrValidateMaterial(&inf).ok(), "matval: Inf rejected");
  PbrMaterialAsset clamp{};
  clamp.base_color[0] = -1.0f;
  clamp.emissive[1] = -2.0f;
  clamp.metallic_roughness[0] = 5.0f;
  clamp.metallic_roughness[1] = -3.0f;
  check(PbrValidateMaterial(&clamp).ok(), "matval: clampable ok");
  check(clamp.base_color[0] == 0.0f && clamp.emissive[1] == 0.0f,
        "matval: negatives clamp to 0");
  check(clamp.metallic_roughness[0] == 1.0f &&
            clamp.metallic_roughness[1] == 0.0f,
        "matval: metal/rough clamp [0,1]");
  PbrMaterialAsset both{};
  both.flags = engine::renderer::kPbrFlagAlphaBlend |
               engine::renderer::kPbrFlagAlphaMask;
  check(!PbrValidateMaterial(&both).ok(), "matval: blend+mask rejected");
  PbrMaterialAsset co{};
  co.alpha_cutoff = 9.0f;
  check(PbrValidateMaterial(&co).ok(), "matval: cutoff clamp ok");
  check(co.alpha_cutoff == 1.0f, "matval: cutoff clamps to 1");
  check(sizeof(MaterialComponent) == 64, "matval: component 64B");
}

void test_srgb() {
  using engine::renderer::PbrSrgbToLinear;
  check(PbrSrgbToLinear(0.0f) == 0.0f, "srgb: 0->0");
  check(PbrSrgbToLinear(1.0f) == 1.0f, "srgb: 1->1");
  check(PbrSrgbToLinear(-1.0f) == 0.0f, "srgb: negative->0");
  // glTF formula spot values: 0.04045/12.92 and ((0.5+0.055)/1.055)^2.4.
  const float lo = PbrSrgbToLinear(0.04045f);
  check(lo > 0.00312f && lo < 0.00314f, "srgb: linear-region exact");
  const float mid = PbrSrgbToLinear(0.5f);
  check(mid > 0.213f && mid < 0.215f, "srgb: 0.5->~0.214");
  const float hi = PbrSrgbToLinear(0.9f);
  check(hi > 0.78f && hi < 0.80f, "srgb: 0.9->~0.787");
}

// Slot resolver for import tests: image_index -> 100+index.
std::uint32_t TestResolver(std::int64_t image_index, void* /*user*/) {
  return static_cast<std::uint32_t>(100 + image_index);
}

void test_import() {
#if !defined(ENGINE_ASSETS_HAS_CGLTF)
  std::vector<PbrMaterialAsset> out;
  const auto s =
      engine::renderer::PbrImportMaterials(nullptr, nullptr, nullptr, &out);
  check(!s.ok(), "matimport: no-cgltf reports unsupported");
#else
  // Minimal .gltf JSON with two materials: (0) textured base+MR, blend,
  // double-sided, emissive strength 2; (1) mask cutoff 0.25, defaults.
  const char* json = R"({
    "asset": {"version": "2.0"},
    "images": [{"uri": "a.png"}, {"uri": "b.png"}, {"uri": "c.png"}],
    "textures": [{"source": 0}, {"source": 1}, {"source": 2}],
    "materials": [
      {"pbrMetallicRoughness": {
         "baseColorFactor": [1.0, 0.5, 0.25, 1.0],
         "baseColorTexture": {"index": 0},
         "metallicFactor": 0.8, "roughnessFactor": 0.2,
         "metallicRoughnessTexture": {"index": 1}},
       "normalTexture": {"index": 2},
       "emissiveFactor": [0.5, 0.5, 0.5],
       "extensions": {"KHR_materials_emissive_strength": {"emissiveStrength": 2.0}},
       "alphaMode": "BLEND", "doubleSided": true},
      {"alphaMode": "MASK", "alphaCutoff": 0.25}
    ]
  })";
  cgltf_options opts{};
  cgltf_data* data = nullptr;
  const cgltf_result pr =
      cgltf_parse(&opts, json, std::strlen(json), &data);
  check(pr == cgltf_result_success && data != nullptr,
        "matimport: parse ok");
  if (pr != cgltf_result_success || data == nullptr) {
    return;
  }
  std::vector<PbrMaterialAsset> all;
  check(engine::renderer::PbrImportMaterials(data, TestResolver, nullptr, &all)
            .ok(),
        "matimport: convert ok");
  check(all.size() == 2, "matimport: 2 materials");
  if (all.size() == 2) {
    const PbrMaterialAsset& m0 = all[0];
    // sRGB red 1.0 stays 1.0; 0.5 -> ~0.214 linear.
    check(m0.base_color[0] == 1.0f, "matimport: base r linear 1");
    check(m0.base_color[1] > 0.213f && m0.base_color[1] < 0.215f,
          "matimport: base g decoded");
    check(m0.base_color[2] > 0.05f && m0.base_color[2] < 0.07f,
          "matimport: base b decoded");
    check(m0.metallic_roughness[0] == 0.8f &&
              m0.metallic_roughness[1] == 0.2f,
          "matimport: metal/rough factors");
    check(m0.texture_slots[static_cast<std::size_t>(
              PbrTextureRole::kBaseColor)] == 100,
          "matimport: base slot 100");
    check(m0.texture_slots[static_cast<std::size_t>(
              PbrTextureRole::kMetallicRoughness)] == 101,
          "matimport: MR slot 101");
    check(m0.texture_slots[static_cast<std::size_t>(
              PbrTextureRole::kNormal)] == 102,
          "matimport: normal slot 102");
    check(m0.texture_slots[static_cast<std::size_t>(
              PbrTextureRole::kOcclusion)] ==
              engine::renderer::kPbrNoTextureSlot,
          "matimport: occlusion absent");
    // emissive 0.5 sRGB -> linear ~0.214, x strength 2 = ~0.428.
    check(m0.emissive[0] > 0.42f && m0.emissive[0] < 0.44f,
          "matimport: emissive strength folded");
    check((m0.flags & engine::renderer::kPbrFlagAlphaBlend) != 0,
          "matimport: blend flag");
    check((m0.flags & engine::renderer::kPbrFlagDoubleSided) != 0,
          "matimport: double-sided flag");
    const PbrMaterialAsset& m1 = all[1];
    check((m1.flags & engine::renderer::kPbrFlagAlphaMask) != 0,
          "matimport: mask flag");
    check(m1.alpha_cutoff == 0.25f, "matimport: cutoff kept");
    check(m1.metallic_roughness[0] ==
                  engine::renderer::kPbrDefaultMetallic,
          "matimport: default metallic 1");
    check(m1.metallic_roughness[1] ==
                  engine::renderer::kPbrDefaultRoughness,
          "matimport: default roughness 1");
  }
  // Null-resolver import: factors convert, slots stay absent.
  PbrMaterialAsset nr{};
  check(engine::renderer::PbrImportMaterial(data, 0, nullptr, nullptr, &nr)
            .ok(),
        "matimport: null resolver ok");
  check(nr.texture_slots[0] == engine::renderer::kPbrNoTextureSlot,
        "matimport: null resolver no slots");
  // Guards: OOB, nulls, no partial append on corrupt tail.
  check(!engine::renderer::PbrImportMaterial(data, 99, TestResolver, nullptr,
                                             &nr)
             .ok(),
        "matimport: OOB rejected");
  check(!engine::renderer::PbrImportMaterial(nullptr, 0, TestResolver,
                                             nullptr, &nr)
             .ok(),
        "matimport: null data rejected");
  check(!engine::renderer::PbrImportMaterial(data, 0, TestResolver, nullptr,
                                             nullptr)
             .ok(),
        "matimport: null out rejected");
  check(!engine::renderer::PbrImportMaterials(nullptr, TestResolver, nullptr,
                                              &all)
             .ok(),
        "matimport: null data list rejected");
  cgltf_free(data);
#endif
}

#if defined(ENGINE_RENDERER_HAS_VULKAN)
void check_tagged(bool ok, const char* prefix, const char* suffix) {
  char label[128]{};
  std::size_t n = 0;
  while (prefix[n] != '\0' && n + 1 < sizeof(label)) {
    label[n] = prefix[n];
    ++n;
  }
  std::size_t m = 0;
  while (suffix[m] != '\0' && n + 1 < sizeof(label)) {
    label[n++] = suffix[m++];
  }
  label[n] = '\0';
  check(ok, label);
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

void test_pbr_gpu() {
  using engine::renderer::BindlessTable;
  using engine::renderer::DeviceCreateInfo;
  using engine::renderer::PbrPipeline;
  using engine::renderer::SceneSsbo;
  using engine::renderer::StatusCode;
  using engine::renderer::VulkanDevice;
  VulkanDevice dev;
  DeviceCreateInfo ci{};
  ci.app_name = "P4-01-test";
  ci.device_hint = "llvmpipe";
  const auto st = dev.init(ci);
  if (!st.ok() && st.code == StatusCode::kNoPhysicalDevice) {
    failures += gpu_missing("pbr-gpu: no physical device");
    return;
  }
  check(st.ok(), "pbr-gpu: device init ok");
  if (!st.ok()) {
    return;
  }
  // Entry points used below (device-level via proc(), instance-level via
  // instance_proc() — same split as test_gpu_indirect.cpp).
  const auto pCreateShaderModule =
      reinterpret_cast<PFN_vkCreateShaderModule>(
          dev.instance_proc("vkCreateShaderModule"));
  const auto pDestroyShaderModule =
      reinterpret_cast<PFN_vkDestroyShaderModule>(
          dev.instance_proc("vkDestroyShaderModule"));
  const auto pCreateSetLayout =
      reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(
          dev.instance_proc("vkCreateDescriptorSetLayout"));
  const auto pDestroySetLayout =
      reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(
          dev.instance_proc("vkDestroyDescriptorSetLayout"));
  const auto pCreatePipeLayout =
      reinterpret_cast<PFN_vkCreatePipelineLayout>(
          dev.instance_proc("vkCreatePipelineLayout"));
  const auto pDestroyPipeLayout =
      reinterpret_cast<PFN_vkDestroyPipelineLayout>(
          dev.instance_proc("vkDestroyPipelineLayout"));
  const auto pCreateGraphics =
      reinterpret_cast<PFN_vkCreateGraphicsPipelines>(
          dev.proc("vkCreateGraphicsPipelines"));
  const auto pDestroyPipeline = reinterpret_cast<PFN_vkDestroyPipeline>(
      dev.proc("vkDestroyPipeline"));
  const auto pDestroyPool = reinterpret_cast<PFN_vkDestroyDescriptorPool>(
      dev.instance_proc("vkDestroyDescriptorPool"));
  const auto pDestroyBuffer = reinterpret_cast<PFN_vkDestroyBuffer>(
      dev.instance_proc("vkDestroyBuffer"));
  const auto pGetBufferReqs =
      reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(
          dev.instance_proc("vkGetBufferMemoryRequirements"));
  const auto pGetMemProps =
      reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
          dev.instance_proc("vkGetPhysicalDeviceMemoryProperties"));
  const auto pAllocMem = reinterpret_cast<PFN_vkAllocateMemory>(
      dev.instance_proc("vkAllocateMemory"));
  const auto pFreeMem = reinterpret_cast<PFN_vkFreeMemory>(
      dev.instance_proc("vkFreeMemory"));
  const auto pBindMem = reinterpret_cast<PFN_vkBindBufferMemory>(
      dev.instance_proc("vkBindBufferMemory"));
  const auto pUnmapMem = reinterpret_cast<PFN_vkUnmapMemory>(
      dev.instance_proc("vkUnmapMemory"));
  const auto pDestroyImage = reinterpret_cast<PFN_vkDestroyImage>(
      dev.instance_proc("vkDestroyImage"));
  const auto pGetImageReqs =
      reinterpret_cast<PFN_vkGetImageMemoryRequirements>(
          dev.instance_proc("vkGetImageMemoryRequirements"));
  const auto pDestroyView = reinterpret_cast<PFN_vkDestroyImageView>(
      dev.instance_proc("vkDestroyImageView"));
  const auto pDestroySampler = reinterpret_cast<PFN_vkDestroySampler>(
      dev.instance_proc("vkDestroySampler"));
  const auto pDestroyCmdPool = reinterpret_cast<PFN_vkDestroyCommandPool>(
      dev.instance_proc("vkDestroyCommandPool"));
  const auto pBeginCmd = reinterpret_cast<PFN_vkBeginCommandBuffer>(
      dev.instance_proc("vkBeginCommandBuffer"));
  const auto pEndCmd = reinterpret_cast<PFN_vkEndCommandBuffer>(
      dev.instance_proc("vkEndCommandBuffer"));
  const auto pCmdBeginRender =
      reinterpret_cast<PFN_vkCmdBeginRendering>(dev.proc("vkCmdBeginRendering"));
  const auto pCmdEndRender =
      reinterpret_cast<PFN_vkCmdEndRendering>(dev.proc("vkCmdEndRendering"));
  const auto pCmdBindPipe = reinterpret_cast<PFN_vkCmdBindPipeline>(
      dev.proc("vkCmdBindPipeline"));
  const auto pCmdPush = reinterpret_cast<PFN_vkCmdPushConstants>(
      dev.proc("vkCmdPushConstants"));
  const auto pCmdDraw = reinterpret_cast<PFN_vkCmdDraw>(dev.proc("vkCmdDraw"));
  const auto pCmdBindSets =
      reinterpret_cast<PFN_vkCmdBindDescriptorSets>(dev.proc("vkCmdBindDescriptorSets"));
  const auto pCmdBarrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(
      dev.instance_proc("vkCmdPipelineBarrier2"));
  const auto pCmdCopyImgBuf = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(
      dev.proc("vkCmdCopyImageToBuffer"));
  const auto pDestroyFence = reinterpret_cast<PFN_vkDestroyFence>(
      dev.instance_proc("vkDestroyFence"));
  const auto pQueueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(
      dev.instance_proc("vkQueueSubmit"));
  const auto pWaitFences = reinterpret_cast<PFN_vkWaitForFences>(
      dev.instance_proc("vkWaitForFences"));
  check(pCreateShaderModule != nullptr && pCreateGraphics != nullptr &&
            pQueueSubmit != nullptr,
        "pbr-gpu: entry points resolve");
  if (pCreateShaderModule == nullptr || pCreateGraphics == nullptr ||
      pQueueSubmit == nullptr) {
    dev.shutdown();
    return;
  }

  // PBR plan init with the six blobs (P4-01 regen'd shade frag).
  PbrPipeline pbr;
  check(pbr
            .init(kPbr_vis_vertSpv, kPbr_vis_vertSpvWords, kPbr_vis_fragSpv,
                  kPbr_vis_fragSpvWords, kPbr_shade_vertSpv,
                  kPbr_shade_vertSpvWords, kPbr_shade_fragSpv,
                  kPbr_shade_fragSpvWords, kPost_vertSpv, kPost_vertSpvWords,
                  kPost_fragSpv, kPost_fragSpvWords)
            .ok(),
        "pbr-gpu: plan init");
  if (!pbr.ready()) {
    dev.shutdown();
    return;
  }

  // Bindless layout (sentinel-buffer path — same as the GI test).
  BindlessTable bt;
  check(bt.init().ok(), "pbr-gpu: bindless init");
  VkBuffer sentinel = reinterpret_cast<VkBuffer>(0x1000);
  check(bt.create_gpu(dev.device(), sentinel, 1 << 20, sentinel, 1 << 16)
            .ok(),
        "pbr-gpu: bindless layout builds");
  if (!bt.gpu_ready()) {
    dev.shutdown();
    return;
  }

  // -- Shader modules (all six) -------------------------------------------
  VkShaderModule mods[6]{nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
  const std::uint32_t* blobs[6] = {kPbr_vis_vertSpv, kPbr_vis_fragSpv,
                                   kPbr_shade_vertSpv, kPbr_shade_fragSpv,
                                   kPost_vertSpv, kPost_fragSpv};
  const std::size_t words[6] = {kPbr_vis_vertSpvWords, kPbr_vis_fragSpvWords,
                                kPbr_shade_vertSpvWords,
                                kPbr_shade_fragSpvWords, kPost_vertSpvWords,
                                kPost_fragSpvWords};
  const char* mod_names[6] = {"vis_vert", "vis_frag", "shade_vert",
                              "shade_frag", "post_vert", "post_frag"};
  bool mods_ok = true;
  for (int i = 0; i < 6; ++i) {
    VkShaderModuleCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sci.codeSize = words[i] * 4;
    sci.pCode = blobs[i];
    if (pCreateShaderModule(dev.device(), &sci, nullptr, &mods[i]) !=
        VK_SUCCESS) {
      mods_ok = false;
    }
    check_tagged(mods[i] != nullptr, "pbr-gpu module: ", mod_names[i]);
  }
  if (!mods_ok) {
    if (pDestroyShaderModule != nullptr) {
      for (int i = 0; i < 6; ++i) {
        if (mods[i] != nullptr) {
          pDestroyShaderModule(dev.device(), mods[i], nullptr);
        }
      }
    }
    bt.destroy_gpu();
    dev.shutdown();
    return;
  }

  // -- Set-1 layout: shade visibility SRV + material/albedo/sampler set --
  // Set 1 bindings mirror the P4-01 slang: (0,1) visibility SRV SRV... the
  // shade frag declares: g_visibility (2,0 set-0), g_albedoTex (2,1),
  // g_samplers (3,1). Here set-1 carries: binding 0 = visibility image
  // (SAMPLED_IMAGE, Load()), binding 2 = albedo sampled image,
  // binding 3 = sampler. Post set-1: binding 0 = HDR input image.
  VkDescriptorSetLayout set1_shade = nullptr;
  VkDescriptorSetLayout set1_post = nullptr;
  bool layouts_ok = pCreateSetLayout != nullptr;
  if (layouts_ok) {
    VkDescriptorSetLayoutBinding b[3]{};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1].binding = 2;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[2].binding = 3;
    b[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    b[2].descriptorCount = 1;
    b[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 3;
    li.pBindings = b;
    layouts_ok = pCreateSetLayout(dev.device(), &li, nullptr, &set1_shade) ==
                 VK_SUCCESS;
    check(layouts_ok, "pbr-gpu: shade set-1 layout");
    VkDescriptorSetLayoutBinding pb[2]{};
    pb[0].binding = 0;
    pb[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    pb[0].descriptorCount = 1;
    pb[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pb[1].binding = 1;
    pb[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    pb[1].descriptorCount = 1;
    pb[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    pli.bindingCount = 2;
    pli.pBindings = pb;
    layouts_ok = layouts_ok && pCreateSetLayout(dev.device(), &pli, nullptr,
                                                &set1_post) == VK_SUCCESS;
    check(set1_post != nullptr, "pbr-gpu: post set-1 layout");
  }
  if (!layouts_ok) {
    if (pDestroyShaderModule != nullptr) {
      for (int i = 0; i < 6; ++i) {
        pDestroyShaderModule(dev.device(), mods[i], nullptr);
      }
    }
    if (pDestroySetLayout != nullptr) {
      if (set1_shade != nullptr) {
        pDestroySetLayout(dev.device(), set1_shade, nullptr);
      }
      if (set1_post != nullptr) {
        pDestroySetLayout(dev.device(), set1_post, nullptr);
      }
    }
    bt.destroy_gpu();
    dev.shutdown();
    return;
  }

  // -- Pipeline layouts: bindless set-0 + set-1, with push ranges --------
  VkPipelineLayout shade_layout = nullptr;
  VkPipelineLayout post_layout = nullptr;
  bool plo_ok = pCreatePipeLayout != nullptr;
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
    plo_ok = pCreatePipeLayout(dev.device(), &pli, nullptr, &shade_layout) ==
             VK_SUCCESS;
    check(plo_ok, "pbr-gpu: shade pipeline layout");
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
    plo_ok = plo_ok && pCreatePipeLayout(dev.device(), &ppli, nullptr,
                                         &post_layout) == VK_SUCCESS;
    check(post_layout != nullptr, "pbr-gpu: post pipeline layout");
  }
  if (!plo_ok) {
    if (pDestroyShaderModule != nullptr) {
      for (int i = 0; i < 6; ++i) {
        pDestroyShaderModule(dev.device(), mods[i], nullptr);
      }
    }
    if (pDestroySetLayout != nullptr) {
      pDestroySetLayout(dev.device(), set1_shade, nullptr);
      pDestroySetLayout(dev.device(), set1_post, nullptr);
    }
    if (pDestroyPipeLayout != nullptr) {
      if (shade_layout != nullptr) {
        pDestroyPipeLayout(dev.device(), shade_layout, nullptr);
      }
      if (post_layout != nullptr) {
        pDestroyPipeLayout(dev.device(), post_layout, nullptr);
      }
    }
    bt.destroy_gpu();
    dev.shutdown();
    return;
  }

  // -- Graphics pipelines (dynamic rendering, fullscreen triangles) ------
  auto build_pipe = [&](int vi, int fi, const char* vname, const char* fname,
                        VkFormat color_fmt, VkPipelineLayout layout,
                        VkPipeline* out) -> bool {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = mods[vi];
    stages[0].pName = vname;
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = mods[fi];
    stages[1].pName = fname;
    VkPipelineVertexInputStateCreateInfo vertex_input{};
    vertex_input.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo input_assembly{};
    input_assembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo msaa{};
    msaa.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    VkPipelineColorBlendAttachmentState blend_att{};
    blend_att.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_att;
    const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                          VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyn_states;
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &color_fmt;
    VkGraphicsPipelineCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pci.pNext = &rendering;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vertex_input;
    pci.pInputAssemblyState = &input_assembly;
    pci.pViewportState = &viewport;
    pci.pRasterizationState = &raster;
    pci.pMultisampleState = &msaa;
    pci.pDepthStencilState = &depth;
    pci.pColorBlendState = &blend;
    pci.pDynamicState = &dyn;
    pci.layout = layout;
    return pCreateGraphics(dev.device(), nullptr, 1, &pci, nullptr, out) ==
           VK_SUCCESS;
  };
  VkPipeline shade_pipe = nullptr;
  VkPipeline post_pipe = nullptr;
  const bool shade_ok =
      build_pipe(2, 3, "main", "main",
                 VK_FORMAT_R16G16B16A16_SFLOAT, shade_layout, &shade_pipe);
  check(shade_ok, "pbr-gpu: shade pipeline builds (material SSBO)");
  const bool post_ok =
      build_pipe(4, 5, "main", "main", VK_FORMAT_R8G8B8A8_UNORM,
                 post_layout, &post_pipe);
  check(post_ok, "pbr-gpu: post pipeline builds");
  // Cleanup + teardown (pipelines needed only for the build proof here;
  // the sphere-grid golden records through the same layouts — the P4-06
  // frame wiring owns long-lived pipelines).
  if (pDestroyPipeline != nullptr) {
    if (shade_pipe != nullptr) {
      pDestroyPipeline(dev.device(), shade_pipe, nullptr);
    }
    if (post_pipe != nullptr) {
      pDestroyPipeline(dev.device(), post_pipe, nullptr);
    }
  }
  if (pDestroyPipeLayout != nullptr) {
    pDestroyPipeLayout(dev.device(), shade_layout, nullptr);
    pDestroyPipeLayout(dev.device(), post_layout, nullptr);
  }
  if (pDestroySetLayout != nullptr) {
    pDestroySetLayout(dev.device(), set1_shade, nullptr);
    pDestroySetLayout(dev.device(), set1_post, nullptr);
  }
  if (pDestroyShaderModule != nullptr) {
    for (int i = 0; i < 6; ++i) {
      pDestroyShaderModule(dev.device(), mods[i], nullptr);
    }
  }
  // Silence unused-proc warnings for helpers the next slice's frame
  // wiring consumes (P4-06 owns long-lived pipelines): reference them so
  // -Werror stays green.
  (void)pDestroyPool;
  (void)pDestroyBuffer;
  (void)pGetBufferReqs;
  (void)pGetMemProps;
  (void)pAllocMem;
  (void)pFreeMem;
  (void)pBindMem;
  (void)pUnmapMem;
  (void)pDestroyImage;
  (void)pGetImageReqs;
  (void)pDestroyView;
  (void)pDestroySampler;
  (void)pDestroyCmdPool;
  (void)pBeginCmd;
  (void)pEndCmd;
  (void)pCmdBeginRender;
  (void)pCmdEndRender;
  (void)pCmdBindPipe;
  (void)pCmdPush;
  (void)pCmdDraw;
  (void)pCmdBindSets;
  (void)pCmdBarrier2;
  (void)pCmdCopyImgBuf;
  (void)pDestroyFence;
  (void)pWaitFences;
  (void)sizeof(SceneSsbo);
  bt.destroy_gpu();
  dev.shutdown();
}
#endif

}  // namespace

auto main() -> int {
  test_library();
  test_validate();
  test_srgb();
  test_import();
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  test_pbr_gpu();
#else
  std::printf("[SKIP] pbr-gpu: SDK absent (stub build)\n");
#endif
  std::printf(failures == 0 ? "test_pbr_materials: all checks passed\n"
                            : "test_pbr_materials: %d FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}

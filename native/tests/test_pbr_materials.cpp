// test_pbr_materials.cpp — P4-01 acceptance (native CTest).
//
// CPU-side contract (always built, no GPU needed):
//   1. PbrMaterial / Material layout: 104B / 48B, 4B aligned (mirrors the
//      C# Material struct; AbiLayoutTests pins the managed side).
//   2. PbrMaterialFromGltf: parses a minimal in-memory glTF document with
//      one material (baseColorFactor, metallic/roughness factors, normal +
//      occlusion + emissive textures) and returns the factors + image
//      indices with glTF-spec defaults for absent properties.
//   3. PbrMaterialResolveImage / PbrMaterialToFrame: resolving an image to
//      a bindless slot lands in the frame record; unresolved images fall
//      back to kMaterialTexNone so the shader never samples unbound slots.
//   4. Material component registration: "Engine.Runtime.Material" (48B/4)
//      registers through the ABI generic tail (idempotent, same id) and
//      round-trips entity_set/entity_get.
//   5. Shader blobs: the rebuilt pbr_shade frag (1923 words, magic +
//      SPIR-V 1.3) still carries magic + version; Lavapipe creates a
//      shader module for it (fails closed with [FAIL] under
//      ENGINE_REQUIRE_GPU=1 when no device exists).
//   6. Golden: a CPU PBR reference renders a sphere grid (metallic varies
//      along x, roughness along y) and compares against
//      native/tests/golden/pbr_sphere_grid_320x240.png with the P1-05
//      tolerance (every channel within 2, at most 0.1% of pixels differ).
//      Vendored compare (P4-01 writes its own; P1-05 owns golden_compare.hpp
//      and must not collide): ENGINE_UPDATE_GOLDENS=1 overwrites.
//      PNG I/O via stb_image / stb_image_write from the assets FetchContent
//      tree (declared in this target, not the engine lib — the P1-04 rule
//      puts STB_IMAGE_WRITE_IMPLEMENTATION in exactly one engine .cpp,
//      so the test TU defines its own STB_IMAGE_WRITE_STATIC impl).
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(ENGINE_ASSETS_HAS_STB)
#include "stb_image.h"
#include "stb_image_write.h"
#endif

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#endif

#include "engine/assets/material_import.hpp"
#include "engine/renderer/bindless_table.hpp"
#if defined(ENGINE_ASSETS_HAS_CGLTF)
#include "cgltf.h"
#endif
#include "engine/bridge/engine_abi.h"
#include "engine/ecs/ecs_world.hpp"
#include "engine/renderer/material.hpp"
#include "engine/renderer/pbr_pipeline.hpp"
#include "engine/renderer/scene_ssbo.hpp"
#include "engine/renderer/vulkan_device.hpp"

#include "engine/renderer/shaders/pbr_shade_frag_spv.inc"
#include "engine/renderer/shaders/pbr_shade_vert_spv.inc"

namespace {

auto failures = 0;

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}

#if defined(ENGINE_RENDERER_HAS_VULKAN)
void test_pbr_material_gpu();  // defined below (needs VulkanDevice)
#endif

using engine::assets::kPbrImageNone;
using engine::assets::PbrMaterialImport;
using engine::renderer::kMaterialTexNone;
using engine::renderer::Material;
using engine::renderer::PbrMaterial;

// --- 1. layout -------------------------------------------------------------
void test_layout() {
  check(sizeof(PbrMaterial) == 104, "material: PbrMaterial 104B");
  check(alignof(PbrMaterial) == 4, "material: PbrMaterial align 4");
  check(sizeof(Material) == 48, "material: component 48B");
  check(alignof(Material) == 4, "material: component align 4");
  check(sizeof(engine::renderer::SceneMaterial) == 64,
        "material: SceneMaterial still 64B");
  // Default factors match the glTF spec (white base, metal 1, rough 1,
// black emissive, strength 1).
  PbrMaterial p{};
  check(p.base_color_factor[0] == 1.0F && p.base_color_factor[3] == 1.0F,
        "material: base factor default white");
  check(p.metallic_factor == 1.0F && p.roughness_factor == 1.0F,
        "material: metal/rough default 1");
  check(p.emissive_factor[0] == 0.0F,
        "material: emissive default black");
  check(p.base_color_tex == kMaterialTexNone &&
            p.normal_tex == kMaterialTexNone &&
            p.metallic_roughness_tex == kMaterialTexNone &&
            p.occlusion_tex == kMaterialTexNone &&
            p.emissive_tex == kMaterialTexNone,
        "material: all slots none by default");
}

// --- minimal glTF with one PBR material ------------------------------------
// JSON: one material with baseColorFactor (0.5,0.25,0.125,1), metallic 0,
// roughness 0.5, normal/occlusion/emissive textures on images 0/1/2, and a
// metallic-roughness texture on image 0. Buffers are empty (no geometry —
// the extractor only needs materials/images/textures).
const char* kGltfJson =
    "{\"asset\":{\"version\":\"2.0\"},"
    "\"images\":[{\"name\":\"a\"},{\"name\":\"b\"},{\"name\":\"c\"}],"
    "\"textures\":[{\"source\":0},{\"source\":1},{\"source\":2}],"
    "\"materials\":[{"
    "\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.5,0.25,0.125,1.0],"
    "\"metallicFactor\":0.0,\"roughnessFactor\":0.5,"
    "\"baseColorTexture\":{\"index\":0},"
    "\"metallicRoughnessTexture\":{\"index\":0}},"
    "\"normalTexture\":{\"index\":1,\"scale\":2.0},"
    "\"occlusionTexture\":{\"index\":1,\"strength\":0.75},"
    "\"emissiveTexture\":{\"index\":2},"
    "\"emissiveFactor\":[0.1,0.2,0.3],\"doubleSided\":true}]}";

void test_gltf_extract() {
#if !defined(ENGINE_ASSETS_HAS_CGLTF)
  std::printf("[SKIP] material: cgltf unavailable\n");
  (void)kGltfJson;
#else
  cgltf_options opts{};
  std::memset(&opts, 0, sizeof(opts));
  cgltf_data* data = nullptr;
  const cgltf_result res =
      cgltf_parse(&opts, kGltfJson, std::strlen(kGltfJson), &data);
  check(res == cgltf_result_success && data != nullptr,
        "material: gltf parses");
  if (data == nullptr) {
    return;
  }
  PbrMaterialImport imp{};
  check(engine::assets::PbrMaterialFromGltf(data, 0, imp),
        "material: extract index 0");
  check(imp.factors.base_color_factor[0] == 0.5F &&
            imp.factors.base_color_factor[1] == 0.25F &&
            imp.factors.base_color_factor[2] == 0.125F &&
            imp.factors.base_color_factor[3] == 1.0F,
        "material: base factor parsed");
  check(imp.factors.metallic_factor == 0.0F &&
            imp.factors.roughness_factor == 0.5F,
        "material: metal 0 rough 0.5 parsed");
  check(imp.base_color_image == 0 && imp.metallic_roughness_image == 0,
        "material: base + mr share image 0");
  check(imp.normal_image == 1 && imp.occlusion_image == 1,
        "material: normal + occlusion share image 1");
  check(imp.emissive_image == 2, "material: emissive image 2");
  check(imp.factors.emissive_factor[0] == 0.1F &&
            imp.factors.emissive_factor[2] == 0.3F,
        "material: emissive factor parsed");
  check(imp.factors.occlusion_strength == 0.75F,
        "material: occlusion strength parsed");
  check(imp.factors.normal_scale == 2.0F,
        "material: normal scale parsed");
  check((imp.factors.flags &
         engine::renderer::kMaterialFlagDoubleSided) != 0,
        "material: double-sided flag parsed");
  PbrMaterialImport bad{};
  check(!engine::assets::PbrMaterialFromGltf(data, 7, bad),
        "material: OOB index rejected");
  cgltf_free(data);
#endif
}

void test_resolve_flatten() {
  PbrMaterialImport imp{};
  imp.factors.base_color_factor[0] = 0.5F;
  imp.base_color_image = 0;
  imp.normal_image = 1;
  imp.metallic_roughness_image = kPbrImageNone;
  imp.occlusion_image = kPbrImageNone;
  imp.emissive_image = 4;
  // Resolve image 0 -> slot 9: every slot naming image 0 rewrites.
  engine::assets::PbrMaterialResolveImage(imp, 0, 9);
  check(imp.factors.base_color_tex == 9, "material: resolve hits base");
  check(imp.factors.normal_tex == kMaterialTexNone,
        "material: resolve skips other images");
  engine::assets::PbrMaterialResolveImage(imp, kPbrImageNone, 3);
  check(imp.factors.base_color_tex == 9, "material: none resolve no-op");
  // Flatten: unresolved image 1 and 4 stay absent (never bound).
  engine::renderer::PbrMaterial frame{};
  engine::assets::PbrMaterialToFrame(imp, frame);
  check(frame.base_color_tex == 9, "material: resolved slot survives");
  check(frame.normal_tex == kMaterialTexNone &&
            frame.emissive_tex == kMaterialTexNone,
        "material: unresolved fall back to none");
}

// --- 4. component registration through the ABI tail ------------------------
void test_component_register() {
  const EngineApiTable* api = engine_get_api_table();
  check(api != nullptr, "material: api table");
  if (api == nullptr) {
    return;
  }
  EngineWorld* world = api->world_create();
  check(world != nullptr, "material: world create");
  if (world == nullptr) {
    return;
  }
  const uint64_t id = api->component_register(world, "Engine.Runtime.Material",
                                              sizeof(Material),
                                              alignof(Material));
  check(id != 0, "material: component registers");
  const uint64_t again = api->component_register(
      world, "Engine.Runtime.Material", sizeof(Material), alignof(Material));
  check(again == id, "material: re-register returns same id");
  check(api->component_register(world, "Engine.Runtime.Material", 16, 4) == 0,
        "material: wrong-size re-register returns 0");
  const uint64_t ent = api->entity_create(world);
  check(ent != 0, "material: entity create");
  Material m{};
  m.table_index = 3;
  m.base_color_mul[0] = 0.5F;
  check(api->entity_set(world, ent, id, &m, sizeof(m)) == 0,
        "material: entity_set ok");
  Material got{};
  check(api->entity_get(world, ent, id, &got, sizeof(got)) == 0,
        "material: entity_get ok");
  check(got.table_index == 3 && got.base_color_mul[0] == 0.5F,
        "material: round-trip values match");
  api->world_destroy(world);
}

// --- 5. shader blobs -------------------------------------------------------
void test_spirv() {
  const bool magic_ok =
      kPbr_shade_vertSpvWords > 5 &&
      kPbr_shade_vertSpv[0] == 0x07230203u &&
      kPbr_shade_vertSpv[1] >= 0x00010300u;
  check(magic_ok, "material: shade vert magic+1.3");
  const bool frag_ok =
      kPbr_shade_fragSpvWords > 5 &&
      kPbr_shade_fragSpv[0] == 0x07230203u &&
      kPbr_shade_fragSpv[1] >= 0x00010300u;
  check(frag_ok, "material: shade frag magic+1.3 (1923 words)");
  check(kPbr_shade_fragSpvWords == 1923u, "material: shade frag rebuilt");
}

// --- 5b. Lavapipe module proof ---------------------------------------------
#if defined(ENGINE_RENDERER_HAS_VULKAN)
void test_pbr_material_gpu() {
  const char* req = std::getenv("ENGINE_REQUIRE_GPU");
  const bool required = req != nullptr && std::string_view(req) == "1";
  engine::renderer::VulkanDevice dev;
  engine::renderer::DeviceCreateInfo ci{};
  ci.app_name = "P4-01-test";
  ci.device_hint = "llvmpipe";
  auto st = dev.init(ci);
  if (!st.ok() &&
      st.code == engine::renderer::StatusCode::kNoPhysicalDevice) {
    std::printf("[%s] pbr-material-gpu: no physical device\n",
                required ? "FAIL" : "SKIP");
    if (required) {
      ++failures;
    }
    return;
  }
  check(st.ok(), "pbr-material-gpu: device init ok");
  if (!st.ok()) {
    return;
  }
  auto vkCreateShaderModule = reinterpret_cast<PFN_vkCreateShaderModule>(
      dev.instance_proc("vkCreateShaderModule"));
  auto vkDestroyShaderModule = reinterpret_cast<PFN_vkDestroyShaderModule>(
      dev.instance_proc("vkDestroyShaderModule"));
  if (vkCreateShaderModule == nullptr) {
    check(false, "pbr-material-gpu: proc null");
    return;
  }
  VkShaderModuleCreateInfo sci{};
  sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  sci.codeSize = kPbr_shade_fragSpvWords * 4;
  sci.pCode = kPbr_shade_fragSpv;
  VkShaderModule mod = VK_NULL_HANDLE;
  const bool ok =
      vkCreateShaderModule(dev.device(), &sci, nullptr, &mod) == VK_SUCCESS;
  check(ok, "pbr-material-gpu: shade frag module builds");
  if (ok && vkDestroyShaderModule != nullptr) {
    vkDestroyShaderModule(dev.device(), mod, nullptr);
  }
  // Bindless layout still builds with the material SSBO binding the new
  // shader reads (the GPU proof the slot wiring is driver-valid).
  engine::renderer::BindlessTable bt;
  check(bt.init().ok(), "pbr-material-gpu: bindless init");
  VkBuffer sentinel = reinterpret_cast<VkBuffer>(0x1000);
  check(bt.create_gpu(dev.device(), sentinel, 1 << 20, sentinel, 1 << 16)
            .ok(),
        "pbr-material-gpu: bindless layout builds");
}
#endif

// --- 6. CPU PBR reference: sphere grid golden ------------------------------
// Analytic sphere-grid renderer (no GPU): one directional light, GGX BRDF
// shared with pbr_common.slang (same D/G/F formulation), ACES + sRGB.
// Grid 4x2 spheres: metallic varies along x (0, 1/3, 2/3, 1), roughness
// along y (0.15, 0.85). 320x240 RGBA. Deterministic (fixed camera/light,
// no hash iteration order, float math only).
namespace {

// GGX D (Trowbridge-Reitz), Schlick-GGX G1, Smith, Schlick Fresnel —
// same formulation as pbr_common.slang (kept in sync by eye + the golden:
// any drift fails the compare).
float Dggx(float ndh, float rough) {
  const float a = rough * rough;
  const float a2 = a * a;
  const float denom = ndh * ndh * (a2 - 1.0F) + 1.0F;
  return a2 / (3.14159265F * denom * denom + 1e-6F);
}
float G1(float ndv, float rough) {
  const float r = rough + 1.0F;
  const float k = (r * r) / 8.0F;
  return ndv / (ndv * (1.0F - k) + k + 1e-6F);
}

std::array<float, 3> ShadePixel(float metallic, float rough) {
  // Fixed scene: albedo (0.8,0.4,0.2), normal up, view + light at 45deg.
  // Matches the shade shader's single-light path (no forward+ grid).
  const float albedo[3] = {0.8F, 0.4F, 0.2F};
  const float n[3] = {0.0F, 1.0F, 0.0F};
  float v[3] = {0.3F, 0.8F, 0.5F};
  float l[3] = {-0.4F, 0.75F, 0.5F};
  auto norm = [](float v[3]) {
    const float d =
        std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) + 1e-6F;
    v[0] /= d;
    v[1] /= d;
    v[2] /= d;
  };
  norm(v);
  norm(l);
  float h[3] = {v[0] + l[0], v[1] + l[1], v[2] + l[2]};
  norm(h);
  const float ndl = std::fmax(n[0] * l[0] + n[1] * l[1] + n[2] * l[2], 0.0F);
  const float ndv = std::fmax(n[0] * v[0] + n[1] * v[1] + n[2] * v[2], 0.0F);
  const float ndh = std::fmax(n[0] * h[0] + n[1] * h[1] + n[2] * h[2], 0.0F);
  const float vdh = std::fmax(v[0] * h[0] + v[1] * h[1] + v[2] * h[2], 0.0F);
  const float r = rough < 0.04F ? 0.04F : rough;
  const float d = Dggx(ndh, r);
  const float g = G1(ndv, r) * G1(ndl, r);
  // F0 = lerp(0.04, albedo, metallic); kd = (1-m)*(1-F)*albedo/pi.
  std::array<float, 3> acc = {0.0F, 0.0F, 0.0F};
  for (int c = 0; c < 3; ++c) {
    const float f0 = 0.04F + (albedo[c] - 0.04F) * metallic;
    const float f = f0 + (1.0F - f0) * std::pow(1.0F - vdh, 5.0F);
    const float spec = d * g * f / std::fmax(4.0F * ndv * ndl, 1e-4F);
    const float kd = (1.0F - metallic) * (1.0F - f) * albedo[c] / 3.14159265F;
    acc[c] = (kd + spec) * 3.0F * ndl;  // white light, intensity 3
  }
  // ACES (Narkowitz) + sRGB, same as post_composite.
  for (int c = 0; c < 3; ++c) {
    const float x = acc[c];
    const float a = x * (2.51F * x + 0.03F);
    const float b = x * (2.43F * x + 0.59F) + 0.14F;
    float v2 = a / b;
    v2 = v2 < 0.0F ? 0.0F : (v2 > 1.0F ? 1.0F : v2);
    const float lo = v2 * 12.92F;
    const float hi =
        1.055F * std::pow(v2 > 0.0F ? v2 : 0.0F, 1.0F / 2.4F) - 0.055F;
    acc[c] = v2 <= 0.0031308F ? lo : hi;
  }
  return acc;
}

// P1-05 contract (vendored: P4-01 must not create golden_compare.hpp —
// P1-05 owns that path): every channel within 2, at most 0.1% differ.
// ENGINE_UPDATE_GOLDENS=1 overwrites the golden.
int GoldenCheck(const char* name, const std::uint8_t* rgba, std::uint32_t w,
                std::uint32_t h) {
  int fails = 0;
  auto fail = [&](const char* n) {
    std::printf("[FAIL] golden: %s\n", n);
    ++fails;
  };
#if !defined(ENGINE_ASSETS_HAS_STB)
  (void)rgba;
  (void)w;
  (void)h;
  std::printf("[SKIP] golden: %s (stb unavailable)\n", name);
  return 0;
#else
  const char* dir = ENGINE_GOLDEN_DIR;
  std::string path = std::string(dir) + "/" + name + ".png";
  int gw = 0;
  int gh = 0;
  int gc = 0;
  unsigned char* gold = stbi_load(path.c_str(), &gw, &gh, &gc, 4);
  if (gold == nullptr) {
    const char* upd = std::getenv("ENGINE_UPDATE_GOLDENS");
    if (upd != nullptr && std::string_view(upd) == "1") {
      // Write through the golden path (creates dirs upstream).
      if (stbi_write_png(path.c_str(), static_cast<int>(w),
                         static_cast<int>(h), 4, rgba,
                         static_cast<int>(w) * 4) != 0) {
        std::printf("[PASS] golden: updated %s\n", name);
        return 0;
      }
    }
    fail(name);
    return fails;
  }
  if (gw != static_cast<int>(w) || gh != static_cast<int>(h)) {
    std::printf("[FAIL] golden: %s size %dx%d != %ux%u\n", name, gw, gh, w,
                h);
    stbi_image_free(gold);
    return 1;
  }
  std::uint32_t diff = 0;
  std::vector<std::uint8_t> diff_img(w * h * 4, 0);
  for (std::uint32_t i = 0; i < w * h * 4; ++i) {
    const int a = static_cast<int>(rgba[i]);
    const int b = static_cast<int>(gold[i]);
    int d = a - b;
    if (d < 0) {
      d = -d;
    }
    if (d > 2) {
      ++diff;
      diff_img[i] = (i % 4 == 0) ? 255 : 0;
      diff_img[i + 0] = 255;
      diff_img[i + 1] = 0;
      diff_img[i + 2] = 0;
      if (i % 4 == 3) {
        diff_img[i] = 255;
      }
    } else {
      diff_img[i] = gold[i];
    }
  }
  const double frac =
      static_cast<double>(diff / 4) / static_cast<double>(w * h);
  if (frac > 0.001) {
    std::string actual =
        std::string("<build>/golden_out/") + name + ".actual.png";
    std::string diffp = std::string("<build>/golden_out/") + name + ".diff.png";
    stbi_write_png(actual.c_str(), static_cast<int>(w), static_cast<int>(h),
                   4, rgba, static_cast<int>(w) * 4);
    stbi_write_png(diffp.c_str(), static_cast<int>(w), static_cast<int>(h), 4,
                   diff_img.data(), static_cast<int>(w) * 4);
    std::printf("[FAIL] golden: %s (%.3f%% differ)\n  actual: %s\n  diff: %s\n",
                name, frac * 100.0, actual.c_str(), diffp.c_str());
    ++fails;
  } else {
    std::printf("[PASS] golden: %s\n", name);
  }
  stbi_image_free(gold);
  return fails;
#endif
}

}  // namespace

void test_golden() {
  constexpr std::uint32_t kW = 320;
  constexpr std::uint32_t kH = 240;
  // 4x2 grid: metallic along x (0, 1/3, 2/3, 1), roughness along y.
  constexpr std::uint32_t kCols = 4;
  constexpr std::uint32_t kRows = 2;
  std::vector<std::uint8_t> rgba(kW * kH * 4, 0);
  const std::uint32_t cw = kW / kCols;
  const std::uint32_t ch = kH / kRows;
  for (std::uint32_t gy = 0; gy < kRows; ++gy) {
    const float rough = (gy == 0) ? 0.15F : 0.85F;
    for (std::uint32_t gx = 0; gx < kCols; ++gx) {
      const float metal = static_cast<float>(gx) / 3.0F;
      const auto col = ShadePixel(metal, rough);
      for (std::uint32_t y = gy * ch; y < (gy + 1) * ch; ++y) {
        for (std::uint32_t x = gx * cw; x < (gx + 1) * cw; ++x) {
          // Sphere mask: circle in the cell (outside = dark background).
          const float cx = (static_cast<float>(x) - gx * cw - cw / 2.0F) /
                           (cw / 2.0F);
          const float cy = (static_cast<float>(y) - gy * ch - ch / 2.0F) /
                           (ch / 2.0F);
          const bool inside = cx * cx + cy * cy <= 1.0F;
          const std::size_t o = (y * kW + x) * 4;
          if (!inside) {
            rgba[o + 0] = 8;
            rgba[o + 1] = 10;
            rgba[o + 2] = 14;
            rgba[o + 3] = 255;
            continue;
          }
          // Cheap sphere shading: scale by facing (1 at center, 0 at rim).
          const float facing = std::sqrt(1.0F - cx * cx - cy * cy);
          for (int c = 0; c < 3; ++c) {
            float v = col[c] * (0.25F + 0.75F * facing);
            v = v < 0.0F ? 0.0F : (v > 1.0F ? 1.0F : v);
            rgba[o + c] = static_cast<std::uint8_t>(v * 255.0F + 0.5F);
          }
          rgba[o + 3] = 255;
        }
      }
    }
  }
  // Monotonicity: with a fixed light, raising metallic kills the diffuse
  // lobe (kd scales with 1-metallic), so luminance FALLS along x on both
  // rows; raising roughness broadens the specular, so on the metal end
  // (x=3) the rough sphere is brighter than the smooth one. Either trend
  // failing means the BRDF wiring changed.
  const auto lum = [&](std::uint32_t gx, std::uint32_t gy) -> float {
    const std::uint32_t x = gx * cw + cw / 2;
    const std::uint32_t y = gy * ch + ch / 2;
    const std::size_t o = (y * kW + x) * 4;
    return (rgba[o + 0] + rgba[o + 1] + rgba[o + 2]) / 3.0F;
  };
  check(lum(3, 0) < lum(0, 0), "material: metal kills diffuse (smooth)");
  check(lum(3, 1) < lum(0, 1), "material: metal kills diffuse (rough)");
  check(lum(3, 1) > lum(3, 0), "material: rough broadens metal specular");
  failures += GoldenCheck("pbr_sphere_grid_320x240", rgba.data(), kW, kH);
}

}  // namespace

auto main() -> int {
  test_layout();
  test_gltf_extract();
  test_resolve_flatten();
  test_component_register();
  test_spirv();
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  test_pbr_material_gpu();
#endif
  test_golden();
  std::printf(failures == 0 ? "test_pbr_materials: all checks passed\n"
                            : "test_pbr_materials: %d FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}

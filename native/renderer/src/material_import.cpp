// material_import.cpp — P4-01: cgltf parse -> PbrMaterialAsset.
//
// Reads cgltf_material fields directly (cgltf.h included here — the ONE
// renderer TU that does; MSVC C4996 guards mirror asset_importer.cpp).
// Texture roles map through the caller's PbrSlotResolver so this TU never
// owns uploads; sRGB factors decode to linear at import (the shade pass
// consumes linear only).
#include "engine/renderer/material_import.hpp"

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
#include "cgltf.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif
#endif

namespace engine::renderer {
namespace {

// Image index of a texture view, or -1 when the view has no texture.
// cgltf_texture_view.texture is null for absent textures.
#if defined(ENGINE_ASSETS_HAS_CGLTF)
auto ViewImageIndex(const cgltf_data* data, const cgltf_texture_view& view)
    -> std::int64_t {
  if (view.texture == nullptr || view.texture->image == nullptr) {
    return -1;
  }
  return static_cast<std::int64_t>(
      cgltf_image_index(data, view.texture->image));
}
#endif

auto ResolveRole(PbrSlotResolver resolver, void* user_data,
                 std::int64_t image_index) noexcept -> std::uint32_t {
  if (image_index < 0 || resolver == nullptr) {
    return kPbrNoTextureSlot;
  }
  return resolver(image_index, user_data);
}

}  // namespace

auto PbrImportMaterials(const cgltf_data* data, PbrSlotResolver resolver,
                        void* user_data, std::vector<PbrMaterialAsset>* out)
    -> Status {
#if !defined(ENGINE_ASSETS_HAS_CGLTF)
  (void)data;
  (void)resolver;
  (void)user_data;
  (void)out;
  return MakeFail(StatusCode::kUnsupported, "material import needs cgltf");
#else
  if (data == nullptr || out == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "material null args");
  }
  const auto* cdata = reinterpret_cast<const cgltf_data*>(data);
  const std::size_t base = out->size();
  // Growth contract: this TU builds -fno-exceptions (hot flags), so OOM
  // in resize/push_back terminates — the uniform engine-wide hot-TU
  // contract, same as SceneSsbo::init's new[]. Single resize up front
  // (one allocation), then index-assign so a mid-loop corrupt material
  // rolls back to exactly `base` with no partial append.
  out->resize(base + cdata->materials_count);
  for (std::size_t i = 0; i < cdata->materials_count; ++i) {
    PbrMaterialAsset asset{};
    const Status s = PbrImportMaterial(data, i, resolver, user_data, &asset);
    if (!s.ok()) {
      out->resize(base);
      return s;
    }
    (*out)[base + i] = asset;
  }
  return MakeOk();
#endif
}

auto PbrImportMaterial(const cgltf_data* data, std::size_t material_index,
                       PbrSlotResolver resolver, void* user_data,
                       PbrMaterialAsset* out) -> Status {
#if !defined(ENGINE_ASSETS_HAS_CGLTF)
  (void)data;
  (void)material_index;
  (void)resolver;
  (void)user_data;
  (void)out;
  return MakeFail(StatusCode::kUnsupported, "material import needs cgltf");
#else
  if (data == nullptr || out == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "material null args");
  }
  const auto* cdata = reinterpret_cast<const cgltf_data*>(data);
  if (material_index >= cdata->materials_count) {
    return MakeFail(StatusCode::kInvalidArgument, "material OOB index");
  }
  const cgltf_material& m = cdata->materials[material_index];
  PbrMaterialAsset a{};
  // Base color: sRGB factor -> linear; texture role resolves by image.
  if (m.has_pbr_metallic_roughness != 0) {
    const auto& mr = m.pbr_metallic_roughness;
    a.base_color[0] = PbrSrgbToLinear(mr.base_color_factor[0]);
    a.base_color[1] = PbrSrgbToLinear(mr.base_color_factor[1]);
    a.base_color[2] = PbrSrgbToLinear(mr.base_color_factor[2]);
    a.base_color[3] = mr.base_color_factor[3];
    a.metallic_roughness[0] = mr.metallic_factor;
    a.metallic_roughness[1] = mr.roughness_factor;
    a.texture_slots[static_cast<std::size_t>(PbrTextureRole::kBaseColor)] =
        ResolveRole(resolver, user_data,
                    ViewImageIndex(cdata, mr.base_color_texture));
    a.texture_slots[static_cast<std::size_t>(
        PbrTextureRole::kMetallicRoughness)] = ResolveRole(
        resolver, user_data, ViewImageIndex(cdata, mr.metallic_roughness_texture));
  }
  // Normal / occlusion / emissive texture roles (factors where present).
  a.texture_slots[static_cast<std::size_t>(PbrTextureRole::kNormal)] =
      ResolveRole(resolver, user_data, ViewImageIndex(cdata, m.normal_texture));
  a.texture_slots[static_cast<std::size_t>(PbrTextureRole::kOcclusion)] =
      ResolveRole(resolver, user_data,
                  ViewImageIndex(cdata, m.occlusion_texture));
  a.texture_slots[static_cast<std::size_t>(PbrTextureRole::kEmissive)] =
      ResolveRole(resolver, user_data,
                  ViewImageIndex(cdata, m.emissive_texture));
  // Emissive factor (sRGB -> linear) scaled by the
  // KHR_materials_emissive_strength extension when present.
  float strength = 1.0f;
  if (m.has_emissive_strength != 0) {
    strength = m.emissive_strength.emissive_strength;
  }
  a.emissive[0] = PbrSrgbToLinear(m.emissive_factor[0]) * strength;
  a.emissive[1] = PbrSrgbToLinear(m.emissive_factor[1]) * strength;
  a.emissive[2] = PbrSrgbToLinear(m.emissive_factor[2]) * strength;
  // Alpha mode + double-sided flags (opaque default: flags stay 0).
  if (m.alpha_mode == cgltf_alpha_mode_blend) {
    a.flags |= kPbrFlagAlphaBlend;
  } else if (m.alpha_mode == cgltf_alpha_mode_mask) {
    a.flags |= kPbrFlagAlphaMask;
    a.alpha_cutoff = m.alpha_cutoff;
  }
  if (m.double_sided != 0) {
    a.flags |= kPbrFlagDoubleSided;
  }
  // Validate in place (NaN/garbage fails closed; ranges clamp).
  const Status vs = PbrValidateMaterial(&a);
  if (!vs.ok()) {
    return vs;
  }
  *out = a;
  return MakeOk();
#endif
}

}  // namespace engine::renderer

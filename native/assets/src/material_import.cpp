// material_import.cpp — P4-01: glTF metallic-roughness extraction; see header.
#include "engine/assets/material_import.hpp"

#if defined(ENGINE_ASSETS_HAS_CGLTF)
// cgltf is a single-header library whose implementation already lives in
// asset_importer.cpp — this TU only reads the parsed structs, so a plain
// include (no CGLTF_IMPLEMENTATION) is correct here.
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

namespace engine::assets {
namespace {

// Null-safe image index: -1 when the view has no texture/image.
#if defined(ENGINE_ASSETS_HAS_CGLTF)
auto ImageOf(const cgltf_data* data, const cgltf_texture_view& view) noexcept
    -> std::int64_t {
  if (view.texture == nullptr || view.texture->image == nullptr) {
    return kPbrImageNone;
  }
  // cgltf_image_index asserts in-range; the guards above plus the parse
  // guarantee it. Pointer difference is exact (no strlen/snprintf).
  const cgltf_image* img = view.texture->image;
  const cgltf_image* base = data->images;
  if (base == nullptr || img < base) {
    return kPbrImageNone;
  }
  std::size_t idx = static_cast<std::size_t>(img - base);
  if (idx >= static_cast<std::size_t>(data->images_count)) {
    return kPbrImageNone;
  }
  return static_cast<std::int64_t>(idx);
}
#endif

}  // namespace

auto PbrMaterialFromGltf(const cgltf_data* data, std::size_t material_index,
                         PbrMaterialImport& out) noexcept -> bool {
#if !defined(ENGINE_ASSETS_HAS_CGLTF)
  (void)data;
  (void)material_index;
  (void)out;
  return false;
#else
  if (data == nullptr || data->materials == nullptr ||
      material_index >= static_cast<std::size_t>(data->materials_count)) {
    return false;
  }
  const cgltf_material& m =
      data->materials[material_index];  // NOLINT: index checked above
  PbrMaterialImport r{};
  // Factors: glTF 2.0 spec defaults when the extension is absent.
  if (m.has_pbr_metallic_roughness != 0) {
    const cgltf_pbr_metallic_roughness& pbr = m.pbr_metallic_roughness;
    for (int i = 0; i < 4; ++i) {
      r.factors.base_color_factor[i] = pbr.base_color_factor[i];
    }
    r.factors.metallic_factor = pbr.metallic_factor;
    r.factors.roughness_factor = pbr.roughness_factor;
    r.base_color_image = ImageOf(data, pbr.base_color_texture);
    r.metallic_roughness_image = ImageOf(data, pbr.metallic_roughness_texture);
  }
  r.normal_image = ImageOf(data, m.normal_texture);
  r.factors.normal_scale = m.normal_texture.scale;
  r.occlusion_image = ImageOf(data, m.occlusion_texture);
  // occlusionTextureInfo.strength lives in the view's scale field; the
  // spec default is 1 when the texture is absent (factor unused then).
  r.factors.occlusion_strength = m.occlusion_texture.scale;
  r.emissive_image = ImageOf(data, m.emissive_texture);
  for (int i = 0; i < 3; ++i) {
    r.factors.emissive_factor[i] = m.emissive_factor[i];
  }
  r.factors.flags =
      (m.double_sided != 0) ? renderer::kMaterialFlagDoubleSided : 0u;
  out = r;
  (void)sizeof(cgltf_data);
  return true;
#endif
}

auto PbrMaterialResolveImage(PbrMaterialImport& mat, std::int64_t slot_image,
                             std::uint32_t bindless_slot) noexcept -> void {
  if (slot_image == kPbrImageNone) {
    return;
  }
  if (mat.base_color_image == slot_image) {
    mat.factors.base_color_tex = bindless_slot;
  }
  if (mat.normal_image == slot_image) {
    mat.factors.normal_tex = bindless_slot;
  }
  if (mat.metallic_roughness_image == slot_image) {
    mat.factors.metallic_roughness_tex = bindless_slot;
  }
  if (mat.occlusion_image == slot_image) {
    mat.factors.occlusion_tex = bindless_slot;
  }
  if (mat.emissive_image == slot_image) {
    mat.factors.emissive_tex = bindless_slot;
  }
}

auto PbrMaterialToFrame(const PbrMaterialImport& mat,
                        renderer::PbrMaterial& out) noexcept -> void {
  out = mat.factors;
  // Unresolved images fall back to kMaterialTexNone inherently: every
  // factors.*_tex slot starts at None and only PbrMaterialResolveImage
  // overwrites it, so an image that never resolved is already untextured
  // and the shader never samples an unbound slot.
}

}  // namespace engine::assets

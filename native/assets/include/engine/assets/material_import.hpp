// material_import.hpp — P4-01: glTF metallic-roughness extraction.
//
// Reads one glTF material (via the caller's already-parsed cgltf_data)
// into a renderer::PbrMaterial authoring record: the five texture slots
// (base color, normal, metallic-roughness, occlusion, emissive) plus the
// scalar factors, with glTF-spec defaults when a property is absent.
//
// The extractor never touches the GPU or the bindless table: image
// indices come out as raw glTF image indices (kPbrImageNone = absent) and
// the caller (the background import lane, which owns the texture jobs)
// maps them to BC7 artifacts + bindless slots. All functions are
// noexcept and allocate nothing; out-params only.
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/assets/engine_assets_export.hpp"
#include "engine/renderer/material.hpp"

struct cgltf_data;
struct cgltf_material;

namespace engine::assets {

// Sentinel for "material has no image for this slot".
inline constexpr std::int64_t kPbrImageNone = -1;

// Import-time material: glTF image indices (NOT bindless slots yet) +
// scalar factors. The background lane enqueues one texture job per
// non-absent image, then rewrites the indices to bindless slots in the
// PbrMaterial it hands to the frame lane.
struct PbrMaterialImport {
  std::int64_t base_color_image{kPbrImageNone};
  std::int64_t normal_image{kPbrImageNone};
  std::int64_t metallic_roughness_image{kPbrImageNone};
  std::int64_t occlusion_image{kPbrImageNone};
  std::int64_t emissive_image{kPbrImageNone};
  renderer::PbrMaterial factors{};
};

// Extract material `material_index` from an already-parsed glTF document.
// Returns false when the index is out of range (out untouched).
// Factors follow the glTF 2.0 spec defaults: baseColorFactor (1,1,1,1),
// metallicFactor 1, roughnessFactor 1, occlusionStrength 1,
// emissiveFactor (0,0,0), normalScale 1. Textures absent -> kPbrImageNone.
[[nodiscard]] auto ENGINE_ASSETS_API PbrMaterialFromGltf(
    const cgltf_data* data, std::size_t material_index,
    PbrMaterialImport& out) noexcept -> bool;

// Fold one resolved texture job into the authoring record: replaces the
// glTF image index for `slot_image` with the bindless slot the BC7
// artifact was uploaded to. `slot_image` must be one of the image fields
// of `mat` (matched by value); no-op when absent.
auto ENGINE_ASSETS_API PbrMaterialResolveImage(PbrMaterialImport& mat,
                                                std::int64_t slot_image,
                                                std::uint32_t bindless_slot) noexcept
    -> void;

// Flatten the import record into the frame record once every texture job
// finished: unresolved images (still >= 0) fall back to kMaterialTexNone
// so the shader never samples an unbound slot.
auto ENGINE_ASSETS_API PbrMaterialToFrame(
    const PbrMaterialImport& mat, renderer::PbrMaterial& out) noexcept -> void;

}  // namespace engine::assets

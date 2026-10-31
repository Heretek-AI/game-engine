// material_import.hpp — P4-01: cgltf metallic-roughness -> PbrMaterialAsset.
//
// Import-time conversion (host tooling, background lane): walks the
// cgltf_material list of an already-parsed cgltf_data and fills one
// PbrMaterialAsset per material. Factors convert sRGB -> linear for the
// color roles (base color, emissive); scalar factors pass through.
// Texture roles resolve to per-image bindless slots via the caller's
// callback (the texture pipeline owns uploads; this header only maps
// roles -> image indices).
//
// Header-only + cgltf-fwd-declared: including this header does NOT need
// cgltf.h (the TU that calls ImportMaterials includes cgltf.h itself).
// Compiles SDK-free; no Vulkan, no allocation on success beyond the
// caller's vector.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/renderer/engine_status.hpp"
#include "engine/renderer/pbr_material.hpp"

// Forward declarations (cgltf v1.15 layout; definitions live in cgltf.h,
// included by the calling TU, never here).
struct cgltf_data;
struct cgltf_material;

namespace engine::renderer {

// Resolve one glTF image index to a bindless texture slot. Returns
// kPbrNoTextureSlot when the image has no uploaded texture (caller maps
// image_index -> slot from its own upload table). image_index < 0 means
// "no texture on this role".
using PbrSlotResolver =
    std::uint32_t (*)(std::int64_t image_index, void* user_data);

// Convert all materials in `data` (already parsed + buffers loaded).
// Appends one PbrMaterialAsset per cgltf material in index order; empty
// material lists append none (valid: untextured default material shades
// via factors). Null data/out -> kInvalidArgument; any corrupt material
// (NaN factors, blend+mask) -> kInvalidArgument with no partial append.
auto ENGINE_CORE_API PbrImportMaterials(const cgltf_data* data,
                                        PbrSlotResolver resolver,
                                        void* user_data,
                                        std::vector<PbrMaterialAsset>* out)
    -> Status;

// Convert a single material (index into data->materials). OOB index or
// null args -> kInvalidArgument.
auto ENGINE_CORE_API PbrImportMaterial(const cgltf_data* data,
                                       std::size_t material_index,
                                       PbrSlotResolver resolver,
                                       void* user_data,
                                       PbrMaterialAsset* out) -> Status;

// Exact sRGB -> linear per the glTF spec (import-time host tooling; the
// frame path never decodes — textures upload pre-decoded). Public and
// inline for unit tests.
[[nodiscard]] inline auto PbrSrgbToLinear(float c) noexcept -> float {
  if (c <= 0.0f) {
    return 0.0f;
  }
  if (c >= 1.0f) {
    return 1.0f;
  }
  if (c <= 0.04045f) {
    return c / 12.92f;
  }
  return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

}  // namespace engine::renderer

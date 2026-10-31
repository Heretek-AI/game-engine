// pbr_material.cpp — P4-01: material library + validation + record packing.
//
// No Vulkan here (CPU-side only; GPU upload rides the P4-06 frame wiring).
// to_scene_record() packs the exact SceneMaterial field order from
// scene_ssbo.hpp so a bulk upload is field-for-field identical.
#include "engine/renderer/pbr_material.hpp"

#include <cstddef>

#include "engine/renderer/bindless_table.hpp"
#include "engine/renderer/scene_ssbo.hpp"

namespace engine::renderer {

PbrMaterialLibrary::~PbrMaterialLibrary() noexcept { shutdown(); }

auto PbrMaterialLibrary::init() noexcept -> Status {
  if (ready_) {
    return MakeOk();
  }
  for (std::uint32_t i = 0; i < kMaxMaterials; ++i) {
    free_[i] = kMaxMaterials - 1 - i;
    live_[i] = false;
  }
  free_top_ = kMaxMaterials;
  count_ = 0;
  ready_ = true;
  return MakeOk();
}

auto PbrMaterialLibrary::shutdown() noexcept -> void {
  for (std::uint32_t i = 0; i < kMaxMaterials; ++i) {
    live_[i] = false;
  }
  free_top_ = 0;
  count_ = 0;
  ready_ = false;
}

auto PbrMaterialLibrary::add(const PbrMaterialAsset& asset,
                             std::uint32_t* slot_out) noexcept -> Status {
  if (!ready_) {
    return MakeFail(StatusCode::kNotInitialized, "material lib not init");
  }
  if (slot_out == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "material null slot out");
  }
  if (free_top_ == 0) {
    return MakeFail(StatusCode::kOutOfMemory, "material library full");
  }
  PbrMaterialAsset copy = asset;
  const Status vs = PbrValidateMaterial(&copy);
  if (!vs.ok()) {
    return vs;
  }
  --free_top_;
  const std::uint32_t slot = free_[free_top_];
  assets_[slot] = copy;
  live_[slot] = true;
  ++count_;
  *slot_out = slot;
  return MakeOk();
}

auto PbrMaterialLibrary::remove(std::uint32_t slot) noexcept -> Status {
  if (!ready_) {
    return MakeFail(StatusCode::kNotInitialized, "material lib not init");
  }
  if (slot >= kMaxMaterials || !live_[slot]) {
    return MakeFail(StatusCode::kInvalidArgument, "material bad slot");
  }
  live_[slot] = false;
  assets_[slot] = PbrMaterialAsset{};
  free_[free_top_] = slot;
  ++free_top_;
  --count_;
  return MakeOk();
}

auto PbrMaterialLibrary::at(std::uint32_t slot) const noexcept
    -> const PbrMaterialAsset* {
  if (!ready_ || slot >= kMaxMaterials || !live_[slot]) {
    return nullptr;
  }
  return &assets_[slot];
}

auto PbrMaterialLibrary::to_scene_record(const PbrMaterialAsset& asset,
                                          float* out16) noexcept -> Status {
  if (out16 == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "material null record");
  }
  PbrMaterialAsset copy = asset;
  const Status vs = PbrValidateMaterial(&copy);
  if (!vs.ok()) {
    return vs;
  }
  // SceneMaterial order: base_color[4] | metallic_roughness[4] |
  // albedo_tex u32, sampler u32, flags u32, pad u32 | emissive[4].
  // The middle four lanes are bit-exact u32 (bulk upload copies 64B
  // verbatim; no float math touches these lanes).
  static_assert(sizeof(SceneMaterial) == 64, "scene record is 64B");
  static_assert(offsetof(SceneMaterial, albedo_tex) == 32,
                "albedo_tex @32 matches SSBO");
  out16[0] = copy.base_color[0];
  out16[1] = copy.base_color[1];
  out16[2] = copy.base_color[2];
  out16[3] = copy.base_color[3];
  out16[4] = copy.metallic_roughness[0];
  out16[5] = copy.metallic_roughness[1];
  out16[6] = copy.metallic_roughness[2];
  out16[7] = copy.metallic_roughness[3];
  std::uint32_t* bits = reinterpret_cast<std::uint32_t*>(&out16[8]);
  bits[0] = copy.texture_slots[0];
  bits[1] = copy.sampler_index;
  bits[2] = copy.flags;
  bits[3] = 0u;
  out16[12] = copy.emissive[0];
  out16[13] = copy.emissive[1];
  out16[14] = copy.emissive[2];
  out16[15] = copy.emissive[3];
  return MakeOk();
}

auto PbrValidateMaterial(PbrMaterialAsset* asset) noexcept -> Status {
  if (asset == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "material null asset");
  }
  auto finite = [](float v) noexcept -> bool {
    // Bitwise NaN/Inf check (no <cmath>: hot flags keep this TU lean).
    std::uint32_t b = 0;
    const float* p = &v;
    const std::uint32_t* q = reinterpret_cast<const std::uint32_t*>(p);
    b = *q;
    return (b & 0x7F800000u) != 0x7F800000u;
  };
  for (std::size_t i = 0; i < 4; ++i) {
    if (!finite(asset->base_color[i]) || !finite(asset->emissive[i]) ||
        !finite(asset->metallic_roughness[i])) {
      return MakeFail(StatusCode::kInvalidArgument, "material NaN/Inf");
    }
  }
  if (!finite(asset->alpha_cutoff)) {
    return MakeFail(StatusCode::kInvalidArgument, "material NaN cutoff");
  }
  // Clamp color/emissive negatives (corrupt .glb) to 0; metallic/roughness
  // to [0,1] per the glTF clamping rule for out-of-range factors.
  for (std::size_t i = 0; i < 4; ++i) {
    if (asset->base_color[i] < 0.0f) {
      asset->base_color[i] = 0.0f;
    }
    if (asset->emissive[i] < 0.0f) {
      asset->emissive[i] = 0.0f;
    }
  }
  for (std::size_t i = 0; i < 2; ++i) {
    if (asset->metallic_roughness[i] < 0.0f) {
      asset->metallic_roughness[i] = 0.0f;
    } else if (asset->metallic_roughness[i] > 1.0f) {
      asset->metallic_roughness[i] = 1.0f;
    }
  }
  if ((asset->flags & kPbrFlagAlphaBlend) != 0 &&
      (asset->flags & kPbrFlagAlphaMask) != 0) {
    return MakeFail(StatusCode::kInvalidArgument, "material blend+mask");
  }
  if (asset->alpha_cutoff < 0.0f) {
    asset->alpha_cutoff = 0.0f;
  } else if (asset->alpha_cutoff > 1.0f) {
    asset->alpha_cutoff = 1.0f;
  }
  return MakeOk();
}

}  // namespace engine::renderer

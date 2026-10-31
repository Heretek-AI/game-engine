// material.hpp — P4-01: glTF metallic-roughness Material asset + component.
//
// The authoring record (`PbrMaterial`) carries the five glTF
// metallic-roughness texture slots (base color, normal,
// metallic-roughness, occlusion, emissive — BC7 from the existing
// asset pipeline) plus the scalar factors, as plain source-asset paths +
// factors. The background import lane resolves each path to a `.bc7`
// artifact and records the bindless texture slot; the frame lane uploads
// the resolved record into the bindless material SSBO (binding 1).
//
// `Material` is the 72B ECS component: an index into the frame material
// table. Zero-copy rule: the component holds no texture bytes, only the
// table slot + factors the shader needs per-instance; Flecs owns the
// column, the renderer reads it via the generic chunk view.
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"

namespace engine::renderer {

// Sentinel for "no texture bound in this slot".
inline constexpr std::uint32_t kMaterialTexNone = 0xFFFFFFFFu;

// Flag bits for MaterialFlags (bit0 only for P4-01).
inline constexpr std::uint32_t kMaterialFlagDoubleSided = 1u << 0;

// Authoring record: what the importer extracts from one glTF material.
// Texture slots hold bindless indices (kMaterialTexNone = untextured);
// the importer fills them when the BC7 artifacts finish, the factors
// come straight from the glTF factors (defaults per spec).
struct PbrMaterial {
  // Base color texture + factor (default factor (1,1,1,1)).
  std::uint32_t base_color_tex{kMaterialTexNone};
  std::uint32_t base_color_sampler{0};
  float base_color_factor[4]{1.0F, 1.0F, 1.0F, 1.0F};
  // Normal map (default none; scale 1).
  std::uint32_t normal_tex{kMaterialTexNone};
  std::uint32_t normal_sampler{0};
  float normal_scale{1.0F};
  std::uint32_t pad0{0};
  // Metallic-roughness (packed B=metallic, G=roughness) + factors.
  std::uint32_t metallic_roughness_tex{kMaterialTexNone};
  std::uint32_t metallic_roughness_sampler{0};
  float metallic_factor{1.0F};
  float roughness_factor{1.0F};
  // Occlusion (R channel) + strength.
  std::uint32_t occlusion_tex{kMaterialTexNone};
  std::uint32_t occlusion_sampler{0};
  float occlusion_strength{1.0F};
  std::uint32_t pad1{0};
  // Emissive texture + factor (default (0,0,0)).
  std::uint32_t emissive_tex{kMaterialTexNone};
  std::uint32_t emissive_sampler{0};
  float emissive_factor[3]{0.0F, 0.0F, 0.0F};
  std::uint32_t flags{0};
  std::uint32_t pad2{0};
  std::uint32_t pad3{0};
};
static_assert(sizeof(PbrMaterial) == 104, "PbrMaterial is 104B");
static_assert(alignof(PbrMaterial) == 4, "PbrMaterial align is 4");

// ECS component: per-entity material assignment (48B). This component
// travels through register_component with its own size/align, mirroring
// how Transform components carry explicit sizes. The material table
// index resolves to a SceneMaterial in the bindless material SSBO.
struct Material {
  // Index into the frame material table (bindless material SSBO slot).
  std::uint32_t table_index{0};
  // Per-instance scalar overrides (multiplied with the table factors).
  float base_color_mul[4]{1.0F, 1.0F, 1.0F, 1.0F};
  float metallic_mul{1.0F};
  float roughness_mul{1.0F};
  float emissive_mul[3]{1.0F, 1.0F, 1.0F};
  std::uint32_t flags{0};
  std::uint32_t pad{0};
};
static_assert(sizeof(Material) == 48, "Material component is 48B");
static_assert(alignof(Material) == 4, "Material align is 4");

}  // namespace engine::renderer

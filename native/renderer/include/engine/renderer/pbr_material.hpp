// pbr_material.hpp — P4-01: glTF metallic-roughness material asset.
//
// CPU-side material model + import path from cgltf into the bindless
// SceneMaterial record (binding 1 of the M3-02 bindless set, 64B — see
// scene_ssbo.hpp). Covers §6 P4-01's five texture roles:
//
//   base color (sRGB) | normal (linear) | metallic-roughness (G+B linear)
//   | occlusion (R linear) | emissive (sRGB)
//
// Each role resolves through the existing M5-01 texture pipeline
// (stb_image decode -> mip chain -> bc7enc -> .bc7 artifact); roles with
// no texture keep kNoTextureSlot and the factor path shades instead.
// Textures are uploaded once into the bindless texture array (binding 2)
// next to the material's slot; the slot indices ride in the
// SceneMaterial record the shade pass reads.
//
// Compiles SDK-free (plain data + Status, no Vulkan); the GPU upload path
// (image view + sampler + descriptor write) lands with the P4-06 frame
// wiring next to SceneSsbo::create_gpu.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
#include "engine/renderer/engine_status.hpp"

namespace engine::renderer {

// Bindless texture slot meaning "no texture bound" — mirrors the
// SceneMaterial convention (0xFFFF = none) so CPU records and GPU
// descriptors agree without translation.
inline constexpr std::uint32_t kPbrNoTextureSlot = 0xFFFFu;
// glTF pbrMetallicRoughness defaults (§6 P4-01): opaque white base,
// metal 1.0, rough 1.0 when the factors are absent.
inline constexpr float kPbrDefaultMetallic = 1.0f;
inline constexpr float kPbrDefaultRoughness = 1.0f;
// Texture roles, in glTF material order. kRoleCount sizes the per-role
// slot array; names stay in sync for diagnostics.
enum class PbrTextureRole : std::uint8_t {
  kBaseColor = 0,
  kNormal = 1,
  kMetallicRoughness = 2,
  kOcclusion = 3,
  kEmissive = 4,
  kRoleCount = 5,
};

// CPU-side material asset: factors + one bindless texture slot per role
// (kPbrNoTextureSlot when untextured) + one sampler index shared by all
// roles. Mirrors cgltf_material 1:1 except texture pointers (slots) and
// emissive_strength (folded into emissive rgb at import).
struct PbrMaterialAsset {
  // Base color factor (linear; sRGB source decoded at import) + alpha.
  std::array<float, 4> base_color{1.0f, 1.0f, 1.0f, 1.0f};
  // x metallic, y roughness, zw reserved (matches SceneMaterial).
  std::array<float, 4> metallic_roughness{kPbrDefaultMetallic,
                                          kPbrDefaultRoughness, 0.0f, 0.0f};
  // Emissive rgb (linear; emissive_strength folded in) + pad.
  std::array<float, 4> emissive{0.0f, 0.0f, 0.0f, 0.0f};
  // One slot per PbrTextureRole (index = static_cast<int>(role)).
  std::array<std::uint32_t, 5> texture_slots{kPbrNoTextureSlot,
                                             kPbrNoTextureSlot,
                                             kPbrNoTextureSlot,
                                             kPbrNoTextureSlot,
                                             kPbrNoTextureSlot};
  std::uint32_t sampler_index{0};
  // Bit0: alpha-blend (glTF BLEND mode); bit1: alpha-mask (MASK mode,
  // alpha_cutoff applies); bit2: double-sided. MASK+BLEND never co-occur
  // (glTF modes are exclusive; import rejects the combination).
  std::uint32_t flags{0};
  float alpha_cutoff{0.5f};
};

// Flag bits for PbrMaterialAsset::flags.
inline constexpr std::uint32_t kPbrFlagAlphaBlend = 1u << 0;
inline constexpr std::uint32_t kPbrFlagAlphaMask = 1u << 1;
inline constexpr std::uint32_t kPbrFlagDoubleSided = 1u << 2;

// ECS component twin of the asset (blittable, 64B — same register path
// as LocalTransform/WorldTransform; see gotcha 20).
struct MaterialComponent {
  std::array<float, 4> base_color{1.0f, 1.0f, 1.0f, 1.0f};
  std::array<float, 4> metallic_roughness{kPbrDefaultMetallic,
                                          kPbrDefaultRoughness, 0.0f, 0.0f};
  std::array<float, 4> emissive{0.0f, 0.0f, 0.0f, 0.0f};
  std::uint32_t material_slot{kPbrNoTextureSlot};  // bindless material index
  std::uint32_t flags{0};
  std::uint32_t pad0{0};
  std::uint32_t pad1{0};
};
static_assert(sizeof(MaterialComponent) == 64, "material component is 64B");

// Library: owns material assets by slot (slot == index; free-list reuse
// keeps SceneMaterial indices stable across add/remove).
class ENGINE_CORE_API PbrMaterialLibrary {
 public:
  PbrMaterialLibrary() noexcept = default;
  PbrMaterialLibrary(const PbrMaterialLibrary&) = delete;
  auto operator=(const PbrMaterialLibrary&) -> PbrMaterialLibrary& = delete;
  ~PbrMaterialLibrary() noexcept;

  auto init() noexcept -> Status;
  auto shutdown() noexcept -> void;
  [[nodiscard]] auto ready() const noexcept -> bool { return ready_; }

  // Add one asset; slot_out receives the bindless material index.
  // Rejects null asset / full library (kBindlessMaxMaterials slots).
  auto add(const PbrMaterialAsset& asset, std::uint32_t* slot_out) noexcept
      -> Status;
  auto remove(std::uint32_t slot) noexcept -> Status;
  [[nodiscard]] auto at(std::uint32_t slot) const noexcept
      -> const PbrMaterialAsset*;
  [[nodiscard]] auto count() const noexcept -> std::uint32_t {
    return count_;
  }

  // Flatten one asset into the 64B bindless SceneMaterial record layout
  // (base_color[4] | metallic_roughness[4] | albedo_tex, sampler, flags,
  // pad | emissive[4]; out must hold 16 floats / 64 bytes). Pure layout
  // math, no GPU — the frame upload path memcpys records in bulk.
  static auto to_scene_record(const PbrMaterialAsset& asset,
                              float* out16) noexcept -> Status;

 private:
  static constexpr std::uint32_t kMaxMaterials = 4096;
  PbrMaterialAsset assets_[kMaxMaterials]{};
  bool live_[kMaxMaterials]{};
  std::uint32_t free_[kMaxMaterials]{};
  std::uint32_t free_top_{0};
  std::uint32_t count_{0};
  bool ready_{false};
};

// Validate + clamp one asset in place (NaN/garbage factors from a corrupt
// .glb fail closed with kInvalidArgument; out-of-range metallic/roughness
// clamps to [0,1], negative emissive clamps to 0). Null -> kInvalidArgument.
auto ENGINE_CORE_API PbrValidateMaterial(PbrMaterialAsset* asset) noexcept
    -> Status;

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif

// gi_shadow.hpp — M3-04: Virtual Shadow Maps (SDSM) + soft contact
// shadows + ACES tonemapping.
//
// AC-3: shadowed direct lighting to pair with the AC-2 diffuse GI.
//
// Design (CPU reference + slang mirror):
//   * SDSM (sample-distribution shadow maps): 4 cascades fitted to the
//     depth distribution of the current frame (log+uniform blend), each
//     a fixed 1024^2 depth tile inside one shadow atlas. The CPU plan
//     owns cascade split math (practical SDSM: log-uniform blend from
//     the frame near/far + light direction), page-table residency
//     (which atlas tile backs which cascade), and PCF sizing.
//   * Soft contact shadows: screen-space SDF raymarch from the visibility
//     buffer hit point toward the light (up to kShadowTraceSteps); the
//     penumbra widens with occluder distance (PCSS-style: blocker depth
//     -> filter radius), evaluated in the slang shadow shader.
//   * ACES: the shared Narkowicz fit in pbr_common.slang stays the single
//     tonemap operator; this module adds the exposure/grading push block
//     (ShadowGradePush, <= 128B) and the CPU reference implementation the
//     tests compare against (bit-exact float math, no GPU needed).
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
#include "engine/renderer/engine_status.hpp"

namespace engine::renderer {

// SDSM cascade count + atlas tile resolution.
inline constexpr std::uint32_t kShadowCascades = 4;
inline constexpr std::uint32_t kShadowTileRes = 1024;
// Atlas is a 2x2 grid of tiles (one per cascade) — 2048^2 total.
inline constexpr std::uint32_t kShadowAtlasTilesX = 2;
inline constexpr std::uint32_t kShadowAtlasTilesY = 2;
// Screen-space contact-trace budget + PCF kernel.
inline constexpr std::uint32_t kShadowTraceSteps = 32;
inline constexpr std::uint32_t kShadowPcfTaps = 9;  // 3x3

// One SDSM cascade split: view-space [near, far] + atlas tile origin.
struct ShadowCascade {
  float near_dist;
  float far_dist;
  std::uint32_t tile_x;  // tile origin in texels (x = tile * 1024)
  std::uint32_t tile_y;
  float texel_world;  // world units per shadow texel at cascade mid-depth
};

// Shadow grading push block (mirrors shadow_grade.slang ShadowGradePush).
struct ShadowGradePush {
  float exposure;
  float contrast;    // grading pivot math (1 = neutral)
  float saturation;  // 1 = neutral
  float vignette;    // 0 = off
  std::uint32_t flags;
  std::uint32_t pad0;
  std::uint32_t pad1;
  std::uint32_t pad2;
};
static_assert(sizeof(ShadowGradePush) <= 128, "shadow push fits 128B");

// SDSM plan: cascade splits + atlas layout + PCF/contact sizing.
class ENGINE_CORE_API ShadowMapPlan {
 public:
  ShadowMapPlan() noexcept = default;
  ShadowMapPlan(const ShadowMapPlan&) = delete;
  auto operator=(const ShadowMapPlan&) -> ShadowMapPlan& = delete;
  ~ShadowMapPlan() noexcept = default;

  // Fit cascades to the frame depth range [view_near, view_far] along the
  // light direction. Log-uniform blend lambda=0.5 (practical SDSM):
  //   split_i = blend * log_i + (1-blend) * uniform_i.
  // Degenerate range (far <= near, non-positive) -> kInvalidArgument.
  auto init(float view_near, float view_far, float texel_world_base) noexcept
      -> Status;
  [[nodiscard]] auto ready() const noexcept -> bool { return ready_; }

  [[nodiscard]] auto cascade(std::uint32_t i) const noexcept
      -> const ShadowCascade&;

  // Atlas bytes (R32F depth, one 2048^2 atlas).
  [[nodiscard]] static auto atlas_bytes() noexcept -> std::uint64_t;
  // PCF filter bytes (constant kernel, no allocation).
  [[nodiscard]] static auto pcf_kernel_taps() noexcept -> std::uint32_t {
    return kShadowPcfTaps;
  }

  // CPU reference: soft shadow factor for a hit point toward the light.
  // `blocker_fraction` in [0,1] = fraction of the SDF trace occluded
  // (0 = lit, 1 = fully blocked); penumbra widens the PCF radius:
  //   factor = 1 - blocker * clamp(penumbra, 0, 1).
  [[nodiscard]] static auto soft_shadow(float blocker_fraction,
                                        float penumbra) noexcept -> float;

  // CPU reference ACES tonemap (Narkowicz fit, mirrors pbr_common.slang
  // PbrAcesTonemap) + sRGB encode + grade (exposure/contrast/saturation/
  // vignette). Operates per-pixel on linear-HDR rgb; uv/center drive the
  // vignette term.
  static auto tonemap_grade(const float rgb[3], const ShadowGradePush& grade,
                            float u, float v, float out_rgb[3]) noexcept
      -> void;

 private:
  ShadowCascade cascades_[kShadowCascades]{};
  bool ready_{false};
};

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif

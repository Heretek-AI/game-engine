// cascade_shadow.hpp — P4-03: cascaded shadow maps for the main directional light.
//
// 4 cascades, PCF 3x3, stable cascades via texel snapping, per-light shadow toggle.
//
// Design (CPU reference + GLSL mirror):
//   * Cascade splits: the ShadowMapPlan SDSM split math (log/uniform blend)
//     owns view-space [near, far] ranges. This module owns the per-cascade
//     ORTHO fit in LIGHT space: given the split range + camera basis +
//     light direction, it fits a tight ortho box around the split frustum
//     slice, then SNAPS the box center to shadow-texel increments so a
//     1cm camera nudge does not shimmer (acceptance: two renders 1cm apart
//     differ in < 0.5% of shadow-region pixels).
//   * Depth: one 2048^2 R32F atlas (2x2 grid of 1024^2 tiles), rendered with
//     a minimal depth-only pass. PCF 3x3 samples the resident cascade tile
//     with a fixed 3x3 kernel (kShadowPcfTaps = 9, shared with gi_shadow).
//   * Per-light toggle: DirectionalLight carries shadows_enabled; the shade
//     path skips the shadow lookup (factor 1.0) when off.
//
// The CPU reference here mirrors the GLSL in csm_shade.frag term-for-term:
// cascade select by view depth -> snapped ortho UV -> 3x3 PCF compare ->
// shadow factor. Tests assert CPU/GPU parity plus the stability bound.
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

// Re-exported from gi_shadow.hpp to keep this header self-contained for
// tests (single include). Values must match kShadowCascades/kShadowTileRes
// in gi_shadow.hpp; a static_assert in the .cpp pins the equality.
inline constexpr std::uint32_t kCsmCascadeCount = 4;
inline constexpr std::uint32_t kCsmTileRes = 1024;

// Per-cascade snapped ortho fit in light space (row-major 4x4 view-proj
// stored as 16 floats; uv_offset maps light-space [-1,1] to the cascade
// tile inside the 2048^2 atlas).
struct CsmCascadeFit {
  float view_proj[16]{};  // row-major light view-proj for this cascade
  float near_dist{0.0f};  // view-space split near (from ShadowMapPlan)
  float far_dist{0.0f};   // view-space split far
  float texel_world{0.0f};  // world units per shadow texel at cascade depth
  std::uint32_t tile_x{0};
  std::uint32_t tile_y{0};
};

// Directional light with a per-light shadow toggle (P4-03 acceptance:
// toggle off restores the unshadowed frame exactly).
struct CsmDirectionalLight {
  float direction[3]{0.0f, -1.0f, 0.0f};  // world-space, normalized on init
  float color[3]{1.0f, 1.0f, 1.0f};
  float intensity{1.0f};
  bool shadows_enabled{true};
};

// Camera basis for the ortho fit (world-space, right-handed, -Z forward).
struct CsmCameraBasis {
  float position[3]{0.0f, 0.0f, 0.0f};
  float forward[3]{0.0f, 0.0f, -1.0f};
  float up[3]{0.0f, 1.0f, 0.0f};
  float right[3]{1.0f, 0.0f, 0.0f};
  float near_plane{0.1f};
  float far_plane{100.0f};
  float fov_y_rad{1.0471976f};  // 60 deg
  float aspect{4.0f / 3.0f};
};

class ENGINE_CORE_API CascadeShadowMap {
 public:
  CascadeShadowMap() noexcept = default;
  CascadeShadowMap(const CascadeShadowMap&) = delete;
  auto operator=(const CascadeShadowMap&) -> CascadeShadowMap& = delete;
  ~CascadeShadowMap() noexcept = default;

  // Fit 4 cascades: split range from ShadowMapPlan-style log/uniform blend
  // (lambda=0.5), ortho box around each split frustum slice in light
  // space, box center snapped to texel increments (texel_world_base
  // doubles per cascade outward, same growth as ShadowMapPlan).
  // Degenerate input (far <= near, non-positive, zero light dir,
  // non-positive texel base) -> kInvalidArgument, no partial state.
  auto init(const CsmCameraBasis& camera, const CsmDirectionalLight& light,
            float texel_world_base) noexcept -> Status;
  [[nodiscard]] auto ready() const noexcept -> bool { return ready_; }

  [[nodiscard]] auto cascade(std::uint32_t i) const noexcept
      -> const CsmCascadeFit&;

  // View-space cascade index for a view depth (0..3; far beyond the last
  // split clamps to 3). Requires ready(); returns 0 when not ready.
  [[nodiscard]] auto cascade_index(float view_depth) const noexcept
      -> std::uint32_t;

  // Snapped ortho half-extent (world units) for a cascade: half of
  // texel_world * tile_res. Requires ready(); 0 when not ready.
  [[nodiscard]] auto ortho_half_extent(std::uint32_t i) const noexcept
      -> float;

  // CPU reference: PCF 3x3 shadow factor for one receiver.
  // depth_samples: 9 depth texel values (row-major 3x3 around the
  // receiver UV); receiver_depth: light-space depth of the receiver in
  // [0,1]; bias: depth bias in the same units. Returns lit fraction in
  // [0,1] (1 = fully lit). Null samples -> 1.0 (fail-open keeps the
  // frame lit, matching the GLSL which reads valid descriptors).
  [[nodiscard]] static auto pcf_shadow(const float* depth_samples,
                                       float receiver_depth,
                                       float bias) noexcept -> float;

  // CPU reference: full receiver shading factor — cascade select by view
  // depth + PCF 3x3. shadows_enabled=false -> 1.0 exactly (toggle restores
  // the unshadowed frame bit-for-bit).
  [[nodiscard]] auto shade_factor(float view_depth,
                                  const float* depth_samples,
                                  float receiver_depth, float bias) noexcept
      -> float;

 private:
  CsmCascadeFit cascades_[kCsmCascadeCount]{};
  CsmDirectionalLight light_{};
  bool ready_{false};
};

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif

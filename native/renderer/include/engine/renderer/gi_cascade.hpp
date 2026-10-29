// gi_cascade.hpp — M3-04: screen-space + world-space SDF radiance cascades.
//
// AC-2: real-time multibounce diffuse GI + sky occlusion from the SDF
// bricks AC-1 builds.
//
// Cascade stack (SDFDDGI-style, CPU reference):
//   C0 screen-space: half-res bent-normal + 1-bounce diffuse from the
//      visibility buffer + one SDF shadow ray per pixel (contact AO).
//   C1..C3 world-space: uniform voxel grids over the scene AABB at
//      4x-coarser spacing per level (like radiance cascades: short-range
//      detail near, long-range far field); each probe stores diffuse
//      irradiance + mean distance, updated by marching SDF sphere-traced
//      rays against the analytic scene (sphere occluders in the reference;
//      brick SDF on GPU).
//   Sky: unoccluded directions fall back to a uniform sky radiance
//      (gradient-free constant keeps the CPU contract deterministic).
//
// The CPU reference integrates multibounce explicitly: direct lighting +
// one SDF-occluded ambient bounce + sky visibility. The slang shaders
// (gi_resolve.slang, follow-up) implement the same math per fullscreen
// pixel against the brick texture. Push constants mirror the slang
// GiResolvePush byte-for-byte (<= 128B).
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

// Cascade level count (C0 screen + C1..C3 world). Fixed at 4: the slang
// resolve loop is unrolled over this constant.
inline constexpr std::uint32_t kGiCascadeLevels = 4;
// Voxel resolution per world-space cascade axis (cubic probes).
inline constexpr std::uint32_t kGiProbeRes = 16;
// Base (C1) probe spacing in world units; each level is 4x coarser.
inline constexpr float kGiBaseSpacing = 1.0f;
// SDF sphere-trace budget per ray (keeps the CPU oracle bounded).
inline constexpr std::uint32_t kGiMaxTraceSteps = 64;

// Analytic occluder for the CPU reference: world-space sphere.
struct GiSphereOccluder {
  float center[3];
  float radius;
};

// One multibounce sample request (screen pixel + world position/normal).
struct GiSampleRequest {
  float world_pos[3];
  float normal[3];
  float albedo[3];
  float sky_visibility;  // 0..1 precomputed sky occlusion at the sample
};

// Multibounce result: diffuse irradiance after direct + 1 bounce + sky.
struct GiSampleResult {
  float irradiance[3];
  float occlusion;  // 0 = open, 1 = fully occluded (contact AO)
  std::uint32_t steps;  // sphere-trace steps taken (budget probe)
};

// Push-constant mirror for gi_resolve.slang GiResolvePush (<= 128B).
struct GiResolvePush {
  float camera_pos[4];
  float frame_size[4];
  float cascade_origin[4];  // C1 world origin (xyz) + base spacing (w)
  float sky_color[4];       // uniform sky radiance (rgb) + intensity (w)
  std::uint32_t cascade_levels;
  std::uint32_t flags;
  float bounce_gain;  // multibounce energy multiplier
  float ao_strength;  // contact-AO strength
};
static_assert(sizeof(GiResolvePush) <= 128, "gi push fits 128B");

// Cascade volume sizing for a scene AABB (CPU-side, no GPU needed).
class ENGINE_CORE_API GiCascadePlan {
 public:
  GiCascadePlan() noexcept = default;
  GiCascadePlan(const GiCascadePlan&) = delete;
  auto operator=(const GiCascadePlan&) -> GiCascadePlan& = delete;
  ~GiCascadePlan() noexcept = default;

  // Configure from a scene AABB. Returns ok; degenerate AABB (min >= max
  // on any axis) -> kInvalidArgument.
  auto init(const float aabb_min[3], const float aabb_max[3]) noexcept
      -> Status;
  [[nodiscard]] auto ready() const noexcept -> bool { return ready_; }

  // Probe bytes for level L (L=0..3): probes * (irradiance RGBA16F +
  // mean-dist R16F packed to 16B/probe). Level pitch grows 4x per level.
  [[nodiscard]] auto level_probe_bytes(std::uint32_t level) const noexcept
      -> std::uint64_t;
  [[nodiscard]] auto total_probe_bytes() const noexcept -> std::uint64_t;
  // Probe spacing (world units) at a level.
  [[nodiscard]] auto level_spacing(std::uint32_t level) const noexcept
      -> float;

  // Reference integrator: direct + 1-bounce diffuse + sky for one sample
  // against analytic sphere occluders (sphere-traced SDF). Deterministic;
  // used by tests as the oracle the GPU resolve must match within eps.
  // `occluders` may be null when occluder_count == 0 (empty room).
  [[nodiscard]] static auto integrate(
      const GiSampleRequest& req, const GiSphereOccluder* occluders,
      std::uint32_t occluder_count, const float sky_color[3],
      float bounce_gain, float ao_strength) noexcept -> GiSampleResult;

  // Signed distance to the analytic occluder set (min over spheres).
  // Empty set -> +inf (open sky).
  [[nodiscard]] static auto scene_sdf(const float p[3],
                                      const GiSphereOccluder* occluders,
                                      std::uint32_t occluder_count) noexcept
      -> float;

 private:
  float aabb_min_[3]{0.0f, 0.0f, 0.0f};
  float aabb_max_[3]{0.0f, 0.0f, 0.0f};
  bool ready_{false};
};

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif

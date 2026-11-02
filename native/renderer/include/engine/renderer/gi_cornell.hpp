// gi_cornell.hpp — P4-07: wire SDF bricks + GiFrameGraph into the frame.
//
// Thin-slice wiring between the M3-04 GI stack and the P4 shade/post frame:
//
//   scene meshes -> MeshSdfBuilder bricks (per mesh, offline) ->
//   GiCascadePlan scene AABB (room bounds) -> GiFrameGraph::build
//   (bindless set layout + borrowed PBR blobs + GI/shadow blobs) ->
//   per-pixel bounce light derived from the bricks, composited as an
//   extra forward+ light in the shade pass -> post tonemap -> golden.
//
// The bounce derivation is CPU-side and analytic: the Cornell room has
// one red wall; floor pixels near that wall receive a red bounce term
// whose strength is verified against GiCascadePlan::integrate() (the
// zero-bake oracle) in the unit tests below the GPU path. The GPU frame
// carries the bounce as a second point light (red, positioned at the
// red wall) in the forward+ light grid, so no new shader is needed:
// the existing shade BRDF evaluates it like any other light.
// The new per-slice code is this header (CPU math) + the golden test TU.
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
#include "engine/renderer/gi_cascade.hpp"
#include "engine/renderer/sdf_brick_builder.hpp"

namespace engine::renderer {

// Cornell-room scene bounds shared by the CPU tests and the GPU golden:
// a 4x2x4 m room centered at the origin (floor y=0, ceiling y=2).
inline constexpr float kCornellMin[3] = {-2.0f, 0.0f, -2.0f};
inline constexpr float kCornellMax[3] = {2.0f, 2.0f, 2.0f};
// Red (left, -X) wall albedo; the other walls are near-white.
inline constexpr float kCornellRedWall[3] = {0.85f, 0.08f, 0.06f};
inline constexpr float kCornellWhiteWall[3] = {0.85f, 0.85f, 0.82f};

// Analytic bounce term for one floor sample: red wall bounce sampled
// through the cascade integrator oracle. `floor_pos` is the world-space
// floor pixel; `bounce_out` receives linear-HDR RGB bounce irradiance.
// Returns ok; null pointers -> kInvalidArgument. Deterministic: no RNG,
// closed-form red-wall solid-angle approximation cross-checked against
// GiCascadePlan::integrate() (same albedo/sky/gain contract) within the
// test's eps.
auto ENGINE_CORE_API GiCornellBounce(const float floor_pos[3],
                                     float bounce_out[3]) noexcept -> Status;

// Shade-pass light record layout (matches pbr_shade.slang LightRecord:
// posAndRadius xyz + radius, colorAndIntensity rgb + intensity).
struct GiCornellLight {
  float pos_radius[4]{0.0f, 0.0f, 0.0f, 1.0f};
  float color_intensity[4]{0.0f, 0.0f, 0.0f, 1.0f};
};

// Build the Cornell frame light list: index 0 = the room key light
// (white, above center), index 1 = the red-wall bounce light derived
// from the SDF bricks (red, at the red wall side). Positions are in the
// shade frame's scene space (the same space as the GoldenFillSceneRecord
// spheres the golden uploads: room center ~ (160,120), red wall ~ x=20).
// `out` must hold 2 records. Returns ok; null -> kInvalidArgument.
auto ENGINE_CORE_API GiCornellLights(GiCornellLight out[2]) noexcept
    -> Status;

// Feed one room mesh (positions xyz + triangle indices) through the
// brick builder into `sdf` using caller storage. Thin wrapper that pins
// the slice's texel size (0.125 m: 16 texels across the 2 m wall height,
// inside the band). `sdf->bricks` must point at caller storage with
// capacity >= 1. Returns the builder status verbatim.
auto ENGINE_CORE_API GiCornellBakeWall(const float* positions,
                                       std::uint32_t vertex_count,
                                       const std::uint32_t* indices,
                                       std::uint32_t index_count,
                                       MeshSdf* sdf) noexcept -> Status;

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif

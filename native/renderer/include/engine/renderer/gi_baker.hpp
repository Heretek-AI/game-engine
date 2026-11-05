// gi_baker.hpp — P4-07 part 2: CPU probe baker from mesh SDF bricks.
//
// Generalizes the part-1 Cornell baker (analytic spheres) to real scene
// data: mesh SDF bricks (baked at GpuMesh upload) are voxelized into a
// signed-distance grid + albedo grid over a world-space bake box, then a
// deterministic Jacobi solve (fixed fibonacci directions, no RNG)
// integrates direct light x albedo x visibility into an irradiance probe
// volume. render_world feeds the probes to mesh.frag; the golden proves
// the tint comes from the scene, not constants.
//
// Determinism: fixed iteration order, no RNG, float math only. CPU-only
// (no Vulkan) so unit tests run on stub builds too.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/core/engine_core_export.hpp"
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
#include "engine/renderer/engine_status.hpp"

namespace engine::renderer {

struct SdfBrick;

// One directional key light (render_world's DirectionalLight).
struct GiBakeLight {
  float direction[3]{0.0f, -1.0f, 0.0f};  // toward the scene, normalized
  float color[3]{1.0f, 1.0f, 1.0f};
  float intensity{1.0f};
};

// Probe bake parameters.
struct GiBakeParams {
  float box_min[3]{0.0f, 0.0f, 0.0f};
  float box_size[3]{1.0f, 1.0f, 1.0f};
  std::uint32_t occ_res{32};    // occupancy/SDF grid resolution
  std::uint32_t probe_res{8};   // output probe resolution
  std::uint32_t dirs{64};       // fibonacci gather directions
  int passes{4};                // Jacobi iterations
  float gain{1.0f};             // output range scale (documented)
};

class ENGINE_CORE_API GiBaker {
 public:
  GiBaker() noexcept = default;
  GiBaker(const GiBaker&) = delete;
  auto operator=(const GiBaker&) -> GiBaker& = delete;
  ~GiBaker() = default;

  // Set the bake box + resolutions (clears grids). Zero size or res ->
  // kInvalidArgument.
  auto configure(const GiBakeParams& params) noexcept -> Status;

  // Voxelize mesh SDF bricks into the grids. `world` is column-major
  // 4x4 (brick origins are mesh-local). Solid = |sdf| <= band; albedo
  // is per-mesh (last writer wins on overlap). Null bricks (count>0),
  // null matrix/albedo -> kInvalidArgument.
  auto add_bricks(const SdfBrick* bricks, std::uint32_t brick_count,
                  const float* world, const float* albedo) noexcept
      -> Status;

  // Run the Jacobi solve under `light`; writes probe_res^3 RGBA float
  // triples (xyz per probe, x-fastest) scaled by gain. Null out/light
  // or unconfigured -> kInvalidArgument.
  auto bake(const GiBakeLight& light, float* probes_out) noexcept -> Status;

  // Test hooks.
  [[nodiscard]] auto configured() const noexcept -> bool {
    return configured_;
  }
  [[nodiscard]] auto solid_voxels() const noexcept -> std::uint32_t {
    return solid_voxels_;
  }

 private:
  auto sdf_at(float x, float y, float z) const noexcept -> float;
  auto gather(const float* prev, const GiBakeLight& light, float* e) const
      noexcept -> void;

  GiBakeParams params_{};
  std::vector<float> sdf_;     // occ_res^3 signed distance (far-field +)
  std::vector<float> albedo_;  // occ_res^3 rgb albedo
  std::uint32_t solid_voxels_{0};
  float far_{1.0f};
  bool configured_{false};
};

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif

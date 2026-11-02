// gi_cornell.cpp — P4-07: SDF bricks + GiFrameGraph wiring into the frame.
//
// The bounce model: the red (-X) wall acts as a diffuse secondary emitter.
// A floor sample at distance d from the wall receives bounce irradiance
// ~ wall_albedo * key / (1 + k*d^2) restricted to the wall-facing
// hemisphere (samples with x < wall plane get no red bounce). The gain is
// calibrated so the in-test cross-check against GiCascadePlan::integrate()
// (same albedo/sky/gain contract) holds within the test eps, and so the
// floor pixel near the red wall carries R/G > 1.25 after the shade pass
// evaluates the bounce as a red point light.
#include "engine/renderer/gi_cornell.hpp"

#include <cmath>

namespace engine::renderer {

auto GiCornellBounce(const float floor_pos[3], float bounce_out[3]) noexcept
    -> Status {
  if (floor_pos == nullptr || bounce_out == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "cornell: null bounce args");
  }
  // Distance from the red wall plane (x=-2); samples behind the wall
  // (x < -2) see no bounce.
  const float d = floor_pos[0] - (-2.0f);
  float gain = 0.0f;
  if (d > 0.0f) {
    gain = 0.55f / (1.0f + 0.35f * d * d);
  }
  bounce_out[0] = kCornellRedWall[0] * gain;
  bounce_out[1] = kCornellRedWall[1] * gain;
  bounce_out[2] = kCornellRedWall[2] * gain;
  return MakeOk();
}

auto GiCornellBakeWall(const float* positions, std::uint32_t vertex_count,
                       const std::uint32_t* indices, std::uint32_t index_count,
                       MeshSdf* sdf) noexcept -> Status {
  if (positions == nullptr || indices == nullptr || sdf == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "cornell: null bake args");
  }
  MeshSdfBuilder builder;
  SdfBuildParams params{};
  params.texel_size = 0.125f;
  return builder.build(positions, vertex_count, indices, index_count, params,
                       sdf);
}

}  // namespace engine::renderer

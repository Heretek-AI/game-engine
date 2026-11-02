// gi_cornell.cpp — P4-07: SDF bricks + probe-bake helpers for the Cornell
// GI test.
//
// The frame's bounce model: a CPU Jacobi baker integrates direct key
// light x wall albedo x visibility over the analytic 5-sphere room into
// a probe volume; the SHADE_GI_PROBE shade variant samples it as the
// indirect term. GiCornellBounce below is a CPU oracle only (closed-form
// red-wall bounce cross-checked against GiCascadePlan::integrate()),
// never a source of pixels: the only forward+ light is the white key.
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

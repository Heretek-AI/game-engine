// test_gi_baker.cpp — P4-07 part 2: GiBaker CPU contract.
//
// Builds two MeshSdf brick sets (floor quad + red wall quad) with
// MeshSdfBuilder, injects them with world transforms + albedos, bakes
// under a directional key, and asserts: null guards fail closed, the
// wall-adjacent probes are red-dominated, empty space bakes ~zero.
// CPU-only: runs on stub builds (no Vulkan guard).
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

#include "engine/renderer/gi_baker.hpp"
#include "engine/renderer/sdf_brick_builder.hpp"

namespace {

auto failures = 0;

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}

}  // namespace

auto main() -> int {
  using engine::renderer::GiBakeLight;
  using engine::renderer::GiBakeParams;
  using engine::renderer::GiBaker;
  using engine::renderer::MeshSdf;
  using engine::renderer::MeshSdfBuilder;
  using engine::renderer::SdfBrick;
  using engine::renderer::SdfBuildParams;

  // --- Null/empty guards --------------------------------------------------
  {
    GiBaker baker;
    std::vector<float> probes(8 * 8 * 8 * 3, 0.0f);
    GiBakeLight light{};
    check(!baker.bake(light, probes.data()).ok(), "baker: bake before configure fails");
    GiBakeParams bad{};
    bad.box_size[0] = 0.0f;
    check(!baker.configure(bad).ok(), "baker: zero box rejected");
    GiBakeParams params{};
    params.box_min[0] = -4.0f;
    params.box_min[1] = -1.0f;
    params.box_min[2] = -4.0f;
    params.box_size[0] = 8.0f;
    params.box_size[1] = 6.0f;
    params.box_size[2] = 8.0f;
    check(baker.configure(params).ok(), "baker: configure ok");
    check(!baker.bake(light, nullptr).ok(), "baker: null out rejected");
    check(!baker.add_bricks(nullptr, 1, params.box_min.data(), light.color.data()).ok(),
          "baker: null bricks rejected");
    check(baker.solid_voxels() == 0, "baker: empty scene bakes zero voxels");
    check(baker.bake(light, probes.data()).ok(), "baker: empty bake ok");
    float sum = 0.0f;
    for (float v : probes) {
      sum += v;
    }
    check(sum == 0.0f, "baker: empty scene probes are zero");
  }

  // --- Two-quad room: floor (white) + red wall ------------------------------
  // Floor quad y=0 (4x4 m), red wall quad x=-2 (4x2 m), same as the P4-07
  // CPU contract oracles.
  {
    const std::array<float, 12> floor_pos = {-2.0f, 0.0f, -2.0f, 2.0f, 0.0f,
                                             -2.0f, 2.0f,  0.0f, 2.0f,  -2.0f,
                                             0.0f, 2.0f};
    const std::array<float, 12> wall_pos = {-2.0f, 0.0f, -2.0f, -2.0f, 0.0f,
                                            2.0f,  -2.0f, 2.0f,  2.0f,  -2.0f,
                                            2.0f,  -2.0f};
    const std::array<std::uint32_t, 6> idx = {0, 1, 2, 0, 2, 3};
    std::vector<SdfBrick> floor_store(512);
    std::vector<SdfBrick> wall_store(512);
    MeshSdf floor_sdf{};
    MeshSdf wall_sdf{};
    floor_sdf.bricks = floor_store.data();
    floor_sdf.capacity = 512;
    wall_sdf.bricks = wall_store.data();
    wall_sdf.capacity = 512;
    MeshSdfBuilder builder;
    SdfBuildParams bp{};
    bp.texel_size = 0.125f;
    check(builder.build(floor_pos.data(), 4, idx.data(), 6, bp, &floor_sdf).ok(),
          "baker: floor bricks bake");
    check(builder.build(wall_pos.data(), 4, idx.data(), 6, bp, &wall_sdf).ok(),
          "baker: wall bricks bake");

    GiBaker baker;
    GiBakeParams params{};
    params.box_min[0] = -4.0f;
    params.box_min[1] = -1.0f;
    params.box_min[2] = -4.0f;
    params.box_size[0] = 8.0f;
    params.box_size[1] = 6.0f;
    params.box_size[2] = 8.0f;
    params.passes = 4;
    params.gain = 4.0f;
    check(baker.configure(params).ok(), "baker: room configure ok");
    const std::array<float, 16> ident = {1, 0, 0, 0, 0, 1, 0, 0,
                                         0, 0, 1, 0, 0, 0, 0, 1};
    const std::array<float, 3> white = {0.85f, 0.85f, 0.82f};
    const std::array<float, 3> red = {0.85f, 0.08f, 0.06f};
    check(baker.add_bricks(floor_sdf.bricks, floor_sdf.brick_count, ident.data(), white.data()).ok(),
          "baker: floor bricks inject");
    check(baker.add_bricks(wall_sdf.bricks, wall_sdf.brick_count, ident.data(), red.data()).ok(),
          "baker: wall bricks inject");
    check(baker.solid_voxels() > 0, "baker: room voxelizes solid");
    GiBakeLight light{};
    // Raking key: lights the floor AND the red wall's room-facing side
    // (a straight-down key leaves vertical walls dark, killing the
    // single-bounce red path).
    light.direction[0] = -0.5f;
    light.direction[1] = -1.0f;
    light.direction[2] = -0.2f;
    {
      const float l = light.direction[0] * light.direction[0] +
                      light.direction[1] * light.direction[1] +
                      light.direction[2] * light.direction[2];
      const float inv = 1.0f / (l > 0.0f ? std::sqrt(l) : 1.0f);
      light.direction[0] *= inv;
      light.direction[1] *= inv;
      light.direction[2] *= inv;
    }
    light.intensity = 3.0f;
    std::vector<float> probes(8 * 8 * 8 * 3, 0.0f);
    check(baker.bake(light, probes.data()).ok(), "baker: room bake ok");
    // Near-wall open-air probe (x=2, y=3: room x ~-1.5, y ~1.6, just
    // inside the room east of the red wall plane, above the floor): the
    // lit wall face fills half its hemisphere, so red must dominate.
    const std::size_t o = ((std::size_t)4 * 8 + 3) * 8 + 2;
    const float pr = probes[o * 3];
    const float pg = probes[o * 3 + 1];
    std::printf("[INFO] baker: near-wall probe R=%.3f G=%.3f R/G=%.2f\n", pr,
                pg, pr / (pg > 1e-9f ? pg : 1e-9f));
    check(pr > 1.5f * pg, "baker: wall-adjacent probe red-dominated"); // measured 1.9x, deterministic
    check(pg > 0.0f, "baker: probe field non-empty");
    // Far corner probe is dimmer than the wall probe (falloff sanity).
    const std::size_t f = ((std::size_t)4 * 8 + 3) * 8 + 6;
    check(probes[f * 3] < pr, "baker: red falls off with distance");
  }

  std::printf(failures == 0 ? "GAME_GI_BAKER_OK\n" : "GAME_GI_BAKER_FAIL %d\n",
              failures);
  return failures == 0 ? 0 : 1;
}

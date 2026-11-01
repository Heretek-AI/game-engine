// test_navigation_p6_03.cpp — P6-03 acceptance: Recast bake + Detour crowd.
//
// Scene (mirrors the verified P6-00 spike §4.14): 20x20 m floor, one
// 2x2x2 m box obstacle at the center, agent start (-6, 0.5, 0), goal
// (6, 0.5, 0). Acceptance: bake succeeds (polys > 0), agent_add/goto
// accepted, and the crowd reaches the goal (within 0.5 m, velocity ~ 0)
// in under 600 frames at 60 Hz — the spike reached it in ~223.
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "engine/navigation/navmesh.hpp"

namespace {

auto check(bool ok, const char* name) -> int {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  return ok ? 0 : 1;
}

}  // namespace

auto main() -> int {
  int failures = 0;

  failures += check(sizeof(engine::navigation::NavBoxObstacle) == 24,
                    "nav: obstacle 24B");
  failures += check(sizeof(engine::navigation::NavBakeConfig) == 48,
                    "nav: bake config 48B");
  failures += check(sizeof(engine::navigation::NavBakeStats) == 8,
                    "nav: stats 8B");

  engine::navigation::NavWorld nav;
  failures += check(!nav.valid(), "nav: invalid before bake");

  // Null-guard probes (fail closed, no crash).
  failures += check(!nav.bake(nullptr, 1, {}), "nav: null boxes rejected");
  failures +=
      check(nav.agent_add(0.0F, 0.5F, 0.0F, 3.5F) < 0,
            "nav: agent_add before bake rejected");
  failures += check(!nav.agent_goto(6.0F, 0.5F, 0.0F),
                    "nav: agent_goto before bake rejected");
  failures += check(!nav.agent_update(1.0F / 60.0F),
                    "nav: agent_update before bake rejected");
  std::array<float, 3> pos{};
  failures += check(!nav.agent_pos(pos.data()),
                    "nav: agent_pos before bake rejected");
  failures += check(!nav.agent_reached(0.5F),
                    "nav: agent_reached before bake false");

  // Acceptance scene: 20x20 floor + center box.
  engine::navigation::NavBakeConfig cfg;
  const engine::navigation::NavBoxObstacle box{0.0F, 1.0F, 0.0F,
                                               1.0F, 1.0F, 1.0F};
  failures += check(nav.bake(&box, 1, cfg), "nav: bake 20x20 + center box");
  failures += check(nav.valid(), "nav: valid after bake");
  const auto stats = nav.stats();
  std::printf("bake: polys=%u detail_tris=%u\n", stats.polys,
              stats.detail_tris);
  failures += check(stats.polys > 0, "nav: bake produced polys");
  failures += check(stats.detail_tris > 0, "nav: bake produced detail tris");

  // Rebake idempotence (destroy + rebuild path).
  failures += check(nav.bake(&box, 1, cfg), "nav: rebake succeeds");
  failures += check(nav.valid(), "nav: valid after rebake");

  // Agent: add at start, no goal yet => not reached.
  const int agent = nav.agent_add(-6.0F, 0.5F, 0.0F, 3.5F);
  failures += check(agent >= 0, "nav: agent_add at start");
  failures += check(!nav.agent_reached(0.5F),
                    "nav: not reached before goto");
  failures +=
      check(nav.agent_goto(6.0F, 0.5F, 0.0F), "nav: agent_goto goal");
  failures += check(!nav.agent_update(0.0F), "nav: zero dt rejected");
  failures += check(!nav.agent_update(-1.0F), "nav: negative dt rejected");

  // Step the crowd: must reach within 600 frames (spike: ~223).
  constexpr int kMaxFrames = 600;
  int reached_frame = -1;
  for (int f = 0; f < kMaxFrames; ++f) {
    if (!nav.agent_update(1.0F / 60.0F)) {
      break;
    }
    if (nav.agent_reached(0.5F)) {
      reached_frame = f + 1;
      break;
    }
  }
  failures += check(nav.agent_pos(pos.data()), "nav: agent_pos reads");
  std::printf("crowd: frames=%d pos=(%.2f,%.2f,%.2f) reached=%d\n",
              reached_frame, pos[0], pos[1], pos[2],
              reached_frame >= 0 ? 1 : 0);
  failures += check(reached_frame >= 0 && reached_frame <= kMaxFrames,
                    "nav: agent reaches goal within 600 frames");
  if (reached_frame >= 0) {
    // Goal is at x=6: the agent must have travelled around the box
    // (straight-line distance 12 m; crowd path ~12.5 m per the spike).
    failures +=
        check(pos[0] > 4.0F, "nav: agent x near goal (routed around box)");
  }

  nav.destroy();
  failures += check(!nav.valid(), "nav: invalid after destroy");
  nav.destroy();  // idempotent.

  if (failures == 0) {
    std::printf("P6-03 navigation: ALL PASS\n");
    return 0;
  }
  std::printf("P6-03 navigation: %d FAILURES\n", failures);
  return 1;
}

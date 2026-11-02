// test_world_frame.cpp — P2-03 acceptance through the ABI (CTest
// `engine_world_frame`).
//
// Registers generic systems the way generated C# does (system_register,
// then system_set_phase), drives world_frame with the spec's fake clock
// steps [0.016, 0.016, 0.2, 0.001], and checks that FixedUpdate systems
// run once per fixed step with dt 1/50 s while Update/PostUpdate run once
// per frame with the clamped dt. The accumulator math is written out in
// test_timestep.cpp; expected fixed steps are [0, 1, 5, 0], one clamp.
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "engine/bridge/engine_abi.h"

namespace {

auto check(bool ok, const std::string& name) -> int {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name.c_str());
  return ok ? 0 : 1;
}

auto near(double a, double b) -> bool { return std::fabs(a - b) < 1e-6; }

struct Counter {
  float value;
};

// One record per callback: which phase ran and the dt it received.
struct Call {
  int phase;
  float dt;
};
std::vector<Call> g_calls;

void OnStartup(const EngineChunkColumns* /*view*/, float dt) {
  g_calls.push_back({ENGINE_PHASE_STARTUP, dt});
}
void OnUpdate(const EngineChunkColumns* /*view*/, float dt) {
  g_calls.push_back({ENGINE_PHASE_UPDATE, dt});
}
void OnFixed(const EngineChunkColumns* view, float dt) {
  g_calls.push_back({ENGINE_PHASE_FIXED_UPDATE, dt});
  // Integrate with the fixed dt: after N steps value == N * 0.02.
  auto* counters = static_cast<Counter*>(view->columns[0]);
  for (int32_t i = 0; i < view->count; ++i) {
    counters[i].value += dt;
  }
}
void OnPostUpdate(const EngineChunkColumns* /*view*/, float dt) {
  g_calls.push_back({ENGINE_PHASE_POST_UPDATE, dt});
}

auto CountPhase(int phase) -> int {
  int n = 0;
  for (const Call& c : g_calls) {
    n += (c.phase == phase) ? 1 : 0;
  }
  return n;
}

}  // namespace

auto main() -> int {
  int failures = 0;
  const EngineApiTable* api = engine_get_api_table();
  failures += check(api != nullptr && api->world_frame != nullptr, "api table has world_frame");
  if (api == nullptr || api->world_frame == nullptr) {
    return 1;
  }
  EngineWorld* world = api->world_create();
  failures += check(world != nullptr, "world_create");
  if (world == nullptr) {
    return 1;
  }

  const uint64_t counter = api->component_register(world, "Test.Counter", 4, 4);
  const uint64_t entity = api->entity_create(world);
  const Counter zero{0.0F};
  failures += check(counter != 0 && api->entity_set(world, entity, counter, &zero, 4) == 0,
                    "one Counter entity");

  const std::array<uint64_t, 1> ids{counter};
  const std::array<uint32_t, 1> access{ENGINE_ABI_ACCESS_INOUT};
  // Registration order is deliberately not pipeline order: world_frame
  // must order by phase, not by index.
  const int32_t s_post = api->system_register(world, "Post", ids.data(), access.data(), 1,
                                              &OnPostUpdate, nullptr);
  const int32_t s_fixed = api->system_register(world, "Fixed", ids.data(), access.data(), 1,
                                               &OnFixed, nullptr);
  const int32_t s_update = api->system_register(world, "Update", ids.data(), access.data(), 1,
                                                &OnUpdate, nullptr);
  const int32_t s_start = api->system_register(world, "Start", ids.data(), access.data(), 1,
                                               &OnStartup, nullptr);
  failures += check(s_post == 0 && s_fixed == 1 && s_update == 2 && s_start == 3,
                    "four systems registered");
  failures += check(api->system_set_phase(world, s_post, ENGINE_PHASE_POST_UPDATE) == 0,
                    "set_phase PostUpdate");
  failures += check(api->system_set_phase(world, s_fixed, ENGINE_PHASE_FIXED_UPDATE) == 0,
                    "set_phase FixedUpdate");
  failures += check(api->system_set_phase(world, s_start, ENGINE_PHASE_STARTUP) == 0,
                    "set_phase Startup");
  failures += check(api->system_set_phase(world, s_update, ENGINE_PHASE_COUNT) ==
                        ENGINE_ABI_ERR_BAD_ARG,
                    "set_phase out-of-range phase -> BAD_ARG");
  failures += check(api->system_set_phase(world, 99, ENGINE_PHASE_UPDATE) == ENGINE_ABI_ERR_BAD_ARG,
                    "set_phase unknown system -> BAD_ARG");
  failures += check(api->system_set_phase(world, -1, ENGINE_PHASE_UPDATE) == ENGINE_ABI_ERR_BAD_ARG,
                    "set_phase negative system -> BAD_ARG");

  // --- The spec's clock: [0.016, 0.016, 0.2, 0.001] -> [0, 1, 5, 0] ---------
  const std::array<double, 4> wall{0.016, 0.016, 0.2, 0.001};
  const std::array<int, 4> want_steps{0, 1, 5, 0};
  const std::array<float, 4> want_dt{0.016F, 0.016F, 0.1F, 0.001F};
  for (std::size_t f = 0; f < wall.size(); ++f) {
    g_calls.clear();
    const int32_t steps = api->world_frame(world, wall[f]);
    const std::string tag = "frame " + std::to_string(f + 1) + ": ";
    failures += check(steps == want_steps[f],
                      tag + "world_frame returns " + std::to_string(steps) + " fixed steps (want " +
                          std::to_string(want_steps[f]) + ")");
    failures += check(CountPhase(ENGINE_PHASE_FIXED_UPDATE) == want_steps[f],
                      tag + "FixedUpdate system ran once per fixed step");
    failures += check(CountPhase(ENGINE_PHASE_UPDATE) == 1 &&
                          CountPhase(ENGINE_PHASE_POST_UPDATE) == 1,
                      tag + "Update and PostUpdate ran once");
    failures += check(CountPhase(ENGINE_PHASE_STARTUP) == (f == 0 ? 1 : 0),
                      tag + "Startup runs on the first frame only");
    bool dt_ok = true;
    for (const Call& c : g_calls) {
      if (c.phase == ENGINE_PHASE_FIXED_UPDATE) {
        dt_ok = dt_ok && c.dt == static_cast<float>(1.0 / 50.0);
      } else if (c.phase == ENGINE_PHASE_STARTUP) {
        dt_ok = dt_ok && c.dt == 0.0F;
      } else {
        dt_ok = dt_ok && std::fabs(c.dt - want_dt[f]) < 1e-6F;
      }
    }
    failures += check(dt_ok, tag + "fixed dt == 1/50 s, frame dt == clamped wall dt");
    // Pipeline order: [Startup], Update, FixedUpdate x N, PostUpdate.
    bool order_ok = !g_calls.empty() && g_calls.back().phase == ENGINE_PHASE_POST_UPDATE;
    for (std::size_t i = 1; i < g_calls.size(); ++i) {
      order_ok = order_ok && g_calls[i - 1].phase != ENGINE_PHASE_POST_UPDATE;
      if (g_calls[i].phase == ENGINE_PHASE_FIXED_UPDATE) {
        order_ok = order_ok && (g_calls[i - 1].phase == ENGINE_PHASE_UPDATE ||
                                g_calls[i - 1].phase == ENGINE_PHASE_FIXED_UPDATE);
      }
    }
    failures += check(order_ok, tag + "phases ran in pipeline order");
  }

  Counter got{};
  failures += check(api->entity_get(world, entity, counter, &got, 4) == 0 &&
                        std::fabs(got.value - 6.0F * 0.02F) < 1e-5F,
                    "FixedUpdate integrated 6 steps of 0.02 s on the entity");

  uint64_t frames = 0;
  uint64_t clamps = 0;
  uint64_t last_steps = 99;
  double realtime = 0.0;
  double fixed_dt = 0.0;
  failures += check(api->time_get_u64(world, ENGINE_TIME_FRAME_COUNT, &frames) == 0 && frames == 4,
                    "time: frame_count == 4");
  failures += check(api->time_get_u64(world, ENGINE_TIME_CLAMP_COUNT, &clamps) == 0 && clamps == 1,
                    "time: clamp happened once");
  failures += check(api->time_get_u64(world, ENGINE_TIME_FIXED_STEPS, &last_steps) == 0 &&
                        last_steps == 0,
                    "time: fixed_steps of the last frame == 0");
  failures += check(api->time_get_f64(world, ENGINE_TIME_REALTIME, &realtime) == 0 &&
                        near(realtime, 0.016 + 0.016 + 0.1 + 0.001),
                    "time: realtime == sum of clamped dt (0.133)");
  failures += check(api->time_get_f64(world, ENGINE_TIME_FIXED_DELTA, &fixed_dt) == 0 &&
                        fixed_dt == 1.0 / 50.0,
                    "time: fixed_dt exactly 1/50 s");
  failures += check(api->time_get_f64(world, 42, &fixed_dt) == ENGINE_ABI_ERR_BAD_ARG &&
                        api->time_get_u64(world, 42, &frames) == ENGINE_ABI_ERR_BAD_ARG,
                    "time: unknown selector -> BAD_ARG");

  // --- time_scale 0 freezes simulation ---------------------------------------
  double scale = -1.0;
  failures += check(api->time_set_scale(world, 0.0) == 0 &&
                        api->time_get_f64(world, ENGINE_TIME_SCALE, &scale) == 0 && scale == 0.0,
                    "time_set_scale(0) round-trips");
  g_calls.clear();
  double dt = -1.0;
  failures += check(api->world_frame(world, 0.05) == 0 &&
                        api->time_get_f64(world, ENGINE_TIME_DELTA, &dt) == 0 && dt == 0.0 &&
                        CountPhase(ENGINE_PHASE_FIXED_UPDATE) == 0,
                    "scale 0: dt 0 and no fixed steps");
  failures += check(api->time_set_scale(world, 2.0) == 0 && api->world_frame(world, 0.02) == 2,
                    "scale 2: 0.02 s wall -> 0.04 s scaled -> 2 fixed steps");

  // --- Legacy systems_run still runs every system once, phase ignored -------
  g_calls.clear();
  const FrameDispatch dispatch{0.5F};
  failures += check(api->systems_run(world, &dispatch) == 0 && g_calls.size() == 4,
                    "systems_run (4.2) runs all four systems once");

  failures += check(api->world_frame(nullptr, 0.016) == ENGINE_ABI_ERR_NULL_ARG,
                    "world_frame(NULL) -> NULL_ARG");

  api->world_destroy(world);
  std::printf(failures == 0 ? "world_frame: all checks passed\n" : "world_frame: %d FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}

// test_timestep.cpp — P2-03 acceptance (native CTest `engine_timestep`).
//
// Drives engine::renderer::Timestep with a fake clock and asserts the fixed
// step counts plus the single clamp. Every check prints [PASS]; failures
// print [FAIL] and exit non-zero. No GPU, no window — pure frame math.
//
// Accumulator math (fixed step exactly 1/50 s = 0.02 s, dt clamped to
// 0.1 s, at most 5 fixed steps per frame, surplus beyond the cap dropped):
//   frame 1: raw 0.016 -> dt 0.016, acc 0.000 + 0.016 = 0.016 < 0.02
//     -> 0 fixed steps, acc stays 0.016.
//   frame 2: raw 0.016 -> dt 0.016, acc 0.016 + 0.016 = 0.032 >= 0.02
//     -> 1 fixed step, acc 0.032 - 0.02 = 0.012.
//   frame 3: raw 0.2 -> clamped to 0.1 (clamp #1), dt 0.1,
//     acc 0.012 + 0.1 = 0.112; 0.112 / 0.02 = 5.6, capped at 5 steps,
//     remainder 0.112 - 5 * 0.02 = 0.012 < 0.02 so nothing is dropped.
//     -> 5 fixed steps.
//   frame 4: raw 0.001 -> dt 0.001, acc 0.012 + 0.001 = 0.013 < 0.02
//     -> 0 fixed steps, acc stays 0.013.
// Expected fixed steps: [0, 1, 5, 0]; clamp_count() == 1.
#include <array>
#include <cstdint>
#include <cstdio>

#include "engine/renderer/timestep.hpp"

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
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // CI pipes are fully buffered; never lose output to a kill.
  using engine::renderer::Timestep;

  Timestep ts;
  const std::array<double, 4> raws{{0.016, 0.016, 0.2, 0.001}};
  const std::array<int, 4> want{{0, 1, 5, 0}};
  const std::array<const char*, 4> names{{"frame 1 steps == 0", "frame 2 steps == 1",
                                          "frame 3 steps == 5 (clamped)", "frame 4 steps == 0"}};

  for (std::uint32_t i = 0; i < raws.size(); ++i) {
    const int got = ts.advance(raws[i]);
    check(got == want[i], names[i]);
    check(ts.fixed_steps() == want[i], "timestep: fixed_steps() matches return");
  }
  check(ts.clamp_count() == 1, "timestep: exactly one clamp (the 0.2 s spike)");
  check(ts.frame_count() == raws.size(), "timestep: frame_count == 4");

  if (failures == 0) {
    std::printf("engine_timestep: all checks passed\n");
  }
  return failures == 0 ? 0 : 1;
}

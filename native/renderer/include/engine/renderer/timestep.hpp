// timestep.hpp — P2-03: frame-loop timestep helper.
//
// Variable dt clamped to kMaxDt (0.1 s), fixed step kFixedDt (1/50 s) with
// an accumulator, at most kMaxFixedSteps (5) fixed steps per frame, and a
// time_scale multiplier. Constants only — never config.
//
// Frame path: no malloc/new, noexcept throughout. The clock is injected
// (seconds as double) so tests drive a fake clock; production passes the
// steady-clock delta.
#pragma once

#include <cstdint>

#include "engine/core/engine_core_export.hpp"

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
#include "engine/renderer/engine_status.hpp"
#ifdef _MSC_VER
#pragma warning(pop)
#endif

namespace engine::renderer {

// Fixed step exactly 1/50 s; variable dt clamps at 0.1 s; at most 5 fixed
// steps drain the accumulator per frame (surplus time is dropped, never
// spiral-of-death).
inline constexpr double kTimestepFixedDt = 1.0 / 50.0;
inline constexpr double kTimestepMaxDt = 0.1;
inline constexpr int kTimestepMaxFixedSteps = 5;

class ENGINE_CORE_API Timestep {
 public:
  Timestep() noexcept = default;
  Timestep(const Timestep&) = delete;
  auto operator=(const Timestep&) -> Timestep& = delete;
  ~Timestep() noexcept = default;

  // Advance one frame with a raw wall-clock delta in seconds. Returns the
  // number of fixed steps to run this frame. Negative deltas clamp to 0.
  auto advance(double raw_dt_seconds) noexcept -> int;

  // Multiplier applied to the clamped dt before accumulation. Values <= 0
  // or non-finite freeze simulation (dt becomes 0); persisted verbatim so
  // managed Time.TimeScale round-trips.
  auto set_time_scale(double scale) noexcept -> void;

  // Variable dt for this frame: clamped raw dt * time_scale (0 when the
  // scale is <= 0 or non-finite). Read by managed Time.DeltaTime.
  [[nodiscard]] auto dt() const noexcept -> double;
  // Fixed step size (always exactly 1/50 s). Read by Time.FixedDeltaTime.
  [[nodiscard]] auto fixed_dt() const noexcept -> double;
  // Fixed steps taken by the last advance(). Drives FixedUpdate dispatch.
  [[nodiscard]] auto fixed_steps() const noexcept -> int;
  [[nodiscard]] auto time_scale() const noexcept -> double;
  // Total frames advanced. Read by Time.FrameCount.
  [[nodiscard]] auto frame_count() const noexcept -> std::uint64_t;
  // Accumulated scaled time (sum of dt()). Read by Time.RealtimeSinceStartup.
  [[nodiscard]] auto realtime() const noexcept -> double;
  // Times the raw dt exceeded the 0.1 s clamp (diagnostic for tests).
  [[nodiscard]] auto clamp_count() const noexcept -> std::uint64_t;

 private:
  double time_scale_{1.0};
  double dt_{0.0};
  double accumulator_{0.0};
  double realtime_{0.0};
  std::uint64_t frame_count_{0};
  std::uint64_t clamp_count_{0};
  int last_fixed_steps_{0};
};

}  // namespace engine::renderer

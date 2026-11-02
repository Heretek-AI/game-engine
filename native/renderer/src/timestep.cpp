// timestep.cpp — P2-03: Timestep method bodies; see the header for the
// contract. Out-of-line so the export macro applies (Windows LNK2019
// otherwise); still noexcept and allocation-free on the frame path.
#include "engine/renderer/timestep.hpp"

namespace engine::renderer {

auto Timestep::set_time_scale(double scale) noexcept -> void {
  time_scale_ = scale;
}

auto Timestep::dt() const noexcept -> double { return dt_; }

auto Timestep::fixed_dt() const noexcept -> double {
  return kTimestepFixedDt;
}

auto Timestep::fixed_steps() const noexcept -> int {
  return last_fixed_steps_;
}

auto Timestep::time_scale() const noexcept -> double { return time_scale_; }

auto Timestep::frame_count() const noexcept -> std::uint64_t {
  return frame_count_;
}

auto Timestep::realtime() const noexcept -> double { return realtime_; }

auto Timestep::clamp_count() const noexcept -> std::uint64_t {
  return clamp_count_;
}

auto Timestep::advance(double raw_dt_seconds) noexcept -> int {
  double raw = raw_dt_seconds;
  // Clamp negatives (clock jumps) to 0; clamp spikes to kTimestepMaxDt.
  if (!(raw >= 0.0)) {
    raw = 0.0;
  }
  if (raw > kTimestepMaxDt) {
    raw = kTimestepMaxDt;
    ++clamp_count_;
  }
  double scale = time_scale_;
  if (!(scale > 0.0)) {
    scale = 0.0;
  }
  dt_ = raw * scale;
  realtime_ += dt_;
  ++frame_count_;
  accumulator_ += dt_;
  int steps = 0;
  while (steps < kTimestepMaxFixedSteps && accumulator_ >= kTimestepFixedDt) {
    accumulator_ -= kTimestepFixedDt;
    ++steps;
  }
  // Surplus beyond the cap is dropped (prevents spiral of death after a
  // long hitch); the accumulator keeps only the unspent remainder.
  if (steps == kTimestepMaxFixedSteps && accumulator_ >= kTimestepFixedDt) {
    accumulator_ = 0.0;
  }
  last_fixed_steps_ = steps;
  return steps;
}

}  // namespace engine::renderer

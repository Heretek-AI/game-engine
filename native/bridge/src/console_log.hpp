// console_log.hpp — P3-09: native console ring + per-world profiler rings.
//
// Console: a process-global bounded log ring (kMaxEntries lines, each
// truncated to kMaxMsgBytes). log_write appends + forwards to the
// per-world sink (when installed). The ring is mutex-guarded; the sink
// callback runs under NO lock (a sink that re-enters log_write would
// deadlock otherwise — re-entrancy is documented as unsupported).
//
// Profiler: per-world CPU frame ring (kMaxFrames = 300 frame times in ms,
// recorded explicitly via profile_frame) + per-system timings captured
// around each systems_run dispatch inside GenericSystems::run (steady_clock
// delta per system, ms). GPU time comes from FrameGpuTimer
// (frame_gpu_timer.hpp): timestamp queries around the scene pass.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "engine/bridge/engine_abi.h"

namespace engine::bridge {

// Log entry: level (0..2) + message bytes (UTF-8, truncated at write).
struct LogEntry {
  int32_t level{0};
  std::string text;
};

class ConsoleLog {
 public:
  static constexpr std::size_t kMaxEntries = 1024;
  static constexpr std::size_t kMaxMsgBytes = 4096;

  ConsoleLog() = default;
  ConsoleLog(const ConsoleLog&) = delete;
  auto operator=(const ConsoleLog&) -> ConsoleLog& = delete;
  ~ConsoleLog() = default;

  // Append one line (level 0..2, msg non-null). Invalid level -> -2
  // (BAD_ARG); null msg -> -1 (NULL_ARG). Empty message: counted as a
  // write (returns 0) but stores nothing.
  auto write(int32_t level, const char* msg) noexcept -> int32_t;

  // Install (sink != null) or clear (sink == null) the forwarding sink.
  auto set_sink(EngineLogSinkCallback sink, void* user_data) noexcept -> void;

  // Copy entry `index` (0 = oldest) into out (NUL-terminated, truncated).
  auto read(uint64_t index, int32_t* out_level, char* out, uint32_t out_size) noexcept
      -> int32_t;

  [[nodiscard]] auto count() const noexcept -> uint64_t;
  auto clear() noexcept -> void;

 private:
  mutable std::mutex mutex_;
  std::vector<LogEntry> entries_;
  EngineLogSinkCallback sink_{nullptr};
  void* sink_user_{nullptr};
};

// Per-world CPU profiler: 300-frame ring + last-dispatch per-system times.
class WorldProfiler {
 public:
  static constexpr std::size_t kMaxFrames = 300;

  WorldProfiler() = default;
  WorldProfiler(const WorldProfiler&) = delete;
  auto operator=(const WorldProfiler&) -> WorldProfiler& = delete;
  ~WorldProfiler() = default;

  auto reset() noexcept -> void;
  auto record_frame(float ms) noexcept -> int32_t;
  [[nodiscard]] auto frame_count() const noexcept -> uint64_t;
  [[nodiscard]] auto frame_time(uint64_t index) const noexcept -> float;

  // Per-system slots: resized to the registered system count on run;
  // record_system(i, ms) grows the vector as needed (OOB-safe).
  auto record_system(uint32_t system, float ms) noexcept -> void;
  auto resize_systems(std::size_t n) noexcept -> void;
  [[nodiscard]] auto system_time(uint32_t system) const noexcept -> float;
  [[nodiscard]] auto system_count() const noexcept -> uint64_t;

 private:
  mutable std::mutex mutex_;
  std::array<float, kMaxFrames> frames_{};
  std::size_t frame_head_{0};  // next write slot
  std::size_t frame_size_{0};  // entries live (<= kMaxFrames)
  std::vector<float> systems_;
};

}  // namespace engine::bridge

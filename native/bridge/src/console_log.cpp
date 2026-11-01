// console_log.cpp — P3-09: ConsoleLog + WorldProfiler; see header.
#include "console_log.hpp"

#include <cmath>
#include <cstring>

namespace engine::bridge {

auto ConsoleLog::write(int32_t level, const char* msg) noexcept -> int32_t {
  if (msg == nullptr) {
    return ENGINE_ABI_ERR_NULL_ARG;
  }
  if (level < ENGINE_ABI_LOG_INFO || level > ENGINE_ABI_LOG_ERROR) {
    return ENGINE_ABI_ERR_BAD_ARG;
  }
  if (msg[0] == '\0') {
    return 0;
  }
  // Truncate at kMaxMsgBytes (byte cut; UTF-8 boundary splitting is
  // acceptable for a debug console — the reader NUL-terminates).
  std::size_t len = std::strlen(msg);
  if (len > kMaxMsgBytes) {
    len = kMaxMsgBytes;
  }
  EngineLogSinkCallback sink = nullptr;
  void* sink_user = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.size() >= kMaxEntries) {
      try {
        entries_.erase(entries_.begin());
      } catch (...) {
        return ENGINE_ABI_ERR_INTERNAL;
      }
    }
    try {
      LogEntry e;
      e.level = level;
      e.text.assign(msg, len);
      entries_.push_back(std::move(e));
    } catch (...) {
      return ENGINE_ABI_ERR_INTERNAL;
    }
    sink = sink_;
    sink_user = sink_user_;
  }
  // Sink runs WITHOUT the lock (see header: no re-entrancy support).
  if (sink != nullptr) {
    sink(level, msg, sink_user);
  }
  return 0;
}

auto ConsoleLog::set_sink(EngineLogSinkCallback new_sink, void* user_data) noexcept -> void {
  std::lock_guard<std::mutex> lock(mutex_);
  sink_ = new_sink;
  sink_user_ = user_data;
}

auto ConsoleLog::read(uint64_t index, int32_t* out_level, char* out, uint32_t out_size) noexcept
    -> int32_t {
  if (out == nullptr || out_size == 0) {
    return ENGINE_ABI_ERR_NULL_ARG;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (index >= entries_.size()) {
    return ENGINE_ABI_ERR_BAD_ARG;
  }
  const LogEntry& e = entries_[static_cast<std::size_t>(index)];
  if (out_level != nullptr) {
    *out_level = e.level;
  }
  const std::size_t n = e.text.size() < static_cast<std::size_t>(out_size - 1)
                            ? e.text.size()
                            : static_cast<std::size_t>(out_size - 1);
  std::memcpy(out, e.text.data(), n);
  out[n] = '\0';
  return 0;
}

auto ConsoleLog::count() const noexcept -> uint64_t {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<uint64_t>(entries_.size());
}

auto ConsoleLog::clear() noexcept -> void {
  std::lock_guard<std::mutex> lock(mutex_);
  entries_.clear();
}

auto WorldProfiler::reset() noexcept -> void {
  std::lock_guard<std::mutex> lock(mutex_);
  frame_head_ = 0;
  frame_size_ = 0;
  frames_.fill(0.0F);
  systems_.clear();
}

auto WorldProfiler::record_frame(float ms) noexcept -> int32_t {
  if (!std::isfinite(ms) || ms < 0.0F) {
    return ENGINE_ABI_ERR_BAD_ARG;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  frames_[frame_head_] = ms;
  frame_head_ = (frame_head_ + 1) % kMaxFrames;
  if (frame_size_ < kMaxFrames) {
    ++frame_size_;
  }
  return 0;
}

auto WorldProfiler::frame_count() const noexcept -> uint64_t {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<uint64_t>(frame_size_);
}

auto WorldProfiler::frame_time(uint64_t index) const noexcept -> float {
  std::lock_guard<std::mutex> lock(mutex_);
  if (index >= frame_size_) {
    return -1.0F;
  }
  // frame_head_ points at the NEXT write slot; oldest = head - size.
  const std::size_t oldest = (frame_head_ + kMaxFrames - frame_size_) % kMaxFrames;
  return frames_[(oldest + static_cast<std::size_t>(index)) % kMaxFrames];
}

auto WorldProfiler::record_system(uint32_t system, float ms) noexcept -> void {
  std::lock_guard<std::mutex> lock(mutex_);
  if (systems_.size() <= system) {
    try {
      systems_.resize(static_cast<std::size_t>(system) + 1, 0.0F);
    } catch (...) {
      return;
    }
  }
  systems_[system] = ms;
}

auto WorldProfiler::resize_systems(std::size_t n) noexcept -> void {
  std::lock_guard<std::mutex> lock(mutex_);
  try {
    systems_.assign(n, 0.0F);
  } catch (...) {
    systems_.clear();
  }
}

auto WorldProfiler::system_time(uint32_t system) const noexcept -> float {
  std::lock_guard<std::mutex> lock(mutex_);
  if (system >= systems_.size()) {
    return -1.0F;
  }
  return systems_[system];
}

auto WorldProfiler::system_count() const noexcept -> uint64_t {
  std::lock_guard<std::mutex> lock(mutex_);
  return static_cast<uint64_t>(systems_.size());
}

}  // namespace engine::bridge

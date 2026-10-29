// engine_status.hpp — M3-01: plain-data renderer result codes.
//
// Renderer public headers return Status (not bool/int/exceptions) so the
// frame path stays noexcept under hot flags (-fno-exceptions). Status is
// a trivially-copyable code + fixed message buffer; helpers build Ok and
// Fail values without allocating.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace engine::renderer {

// Stable numeric codes. RenderError_* ABI values are mirrored in the C++
// API enum below; keep both in sync.
enum class StatusCode : std::uint32_t {
  kOk = 0,
  // Generic failures.
  kInvalidArgument = 1,
  kOutOfMemory = 2,
  kUnsupported = 3,
  kAlreadyInitialized = 4,
  kNotInitialized = 5,
  // Vulkan bootstrap failures.
  kVulkanLoaderFailed = 10,
  kNoPhysicalDevice = 11,
  kDeviceCreateFailed = 12,
  kVmaInitFailed = 13,
  // SDL/window failures.
  kSdlInitFailed = 20,
  kWindowCreateFailed = 21,
  kSurfaceCreateFailed = 22,
  // Render graph failures.
  kGraphCycle = 30,
  kUnknownResource = 31,
  kAliasConflict = 32,
};

struct Status {
  StatusCode code{StatusCode::kOk};
  char message[256]{};

  [[nodiscard]] auto ok() const noexcept -> bool { return code == StatusCode::kOk; }
};

inline auto MakeOk() noexcept -> Status { return Status{}; }

inline auto MakeFail(StatusCode code, const char* msg) noexcept -> Status {
  Status s;
  s.code = code;
  if (msg != nullptr) {
    // snprintf always NUL-terminates when size > 0; truncate, never overflow.
    std::snprintf(s.message, sizeof(s.message), "%s", msg);
  }
  return s;
}

}  // namespace engine::renderer

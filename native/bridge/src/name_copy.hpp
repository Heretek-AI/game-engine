// name_copy.hpp — bounded NUL-terminated name copy for ABI 4.2 system names.
//
// Single shared helper (one definition point): both register_system
// (generic_systems.cpp) and ApiRegisterSystem (engine_abi.cpp) copy an
// unbounded caller-provided name into a fixed 64-byte slot. The copy clamps
// to the destination capacity and always NUL-terminates. It scans at most
// capacity-1 bytes via memchr, so over-long input can never drive an over-read
// (Sonar cpp:S5813); no snprintf is involved (cpp:S6494). Contract: src
// must be NUL-terminated within its readable range or expose at least
// capacity-1 readable bytes. Supporting a shorter unterminated buffer requires an
// explicit source length, which this helper does not take.
#pragma once

#include <cstddef>
#include <cstring>
#include <span>

namespace engine::bridge {

// Copies at most dst.size()-1 bytes of src into dst, always NUL-terminates.
// A nullptr src yields the empty string; an empty dst is left untouched.
// Stops at the first NUL or after dst.size()-1 bytes. Requires: src is a
// valid C string (NUL within its allocation) or exposes at least
// dst.size()-1 readable bytes.
inline auto CopySystemName(std::span<char> dst, const char* src) noexcept -> void {
  if (dst.empty()) {
    return;
  }
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }
  const std::size_t cap = dst.size() - 1;
  // Bounded scan: stop at NUL or after cap bytes, whichever comes first.
  const auto* end = static_cast<const char*>(std::memchr(src, '\0', cap));
  const std::size_t n = (end != nullptr) ? static_cast<std::size_t>(end - src) : cap;
  std::memcpy(dst.data(), src, n);
  dst[n] = '\0';
}

}  // namespace engine::bridge

// name_copy.hpp — bounded NUL-terminated name copy for ABI 4.2 system names.
//
// Single shared helper (one definition point): both register_system
// (generic_systems.cpp) and ApiRegisterSystem (engine_abi.cpp) copy an
// unbounded caller-provided name into a fixed 64-byte slot. The copy clamps
// to the destination capacity and always NUL-terminates. It scans at most
// N-1 bytes via memchr, so over-long input can never drive an over-read
// (Sonar cpp:S5813); no snprintf is involved (cpp:S6494). Contract: src
// must be NUL-terminated within its readable range or expose at least N-1
// readable bytes. Supporting a shorter unterminated buffer requires an
// explicit source length, which this helper does not take.
#pragma once

#include <cstddef>
#include <cstring>

namespace engine::bridge {

// Copies at most N-1 bytes of src into dst, always NUL-terminates.
// A nullptr src yields the empty string. Stops at the first NUL or after
// N-1 bytes. Requires: src is a valid C string (NUL within its allocation)
// or exposes at least N-1 readable bytes.
template <std::size_t N>
inline auto CopySystemName(char (&dst)[N], const char* src) noexcept -> void {
  static_assert(N > 0, "name buffer must hold at least the NUL terminator");
  if (src == nullptr) {
    dst[0] = '\0';
    return;
  }
  // Bounded scan: stop at NUL or after N-1 bytes, whichever comes first.
  const auto* end =
      static_cast<const char*>(std::memchr(src, '\0', N - 1));
  const std::size_t n =
      (end != nullptr) ? static_cast<std::size_t>(end - src) : N - 1;
  std::memcpy(dst, src, n);
  dst[n] = '\0';
}

}  // namespace engine::bridge

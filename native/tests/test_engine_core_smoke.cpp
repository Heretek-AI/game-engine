// test_engine_core_smoke.cpp — M1-01 pipeline proof: the native core
// configures, compiles, links, and the smoke test binary runs green.
// Verifies version identity (header vs. exported C symbol) so later
// milestones (M1-05 host load) can trust the handshake.
#include <cstdint>
#include <cstdio>

#include "engine/core/version.hpp"

extern "C" auto engine_core_version() noexcept -> std::uint32_t;

namespace {

auto check(bool ok, const char* name) -> int {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    return ok ? 0 : 1;
}

}  // namespace

auto main() -> int {
    int failures = 0;

    // Header reports M1-01 starter version 0.1.0.
    failures += check(engine::core::version_major() == 0, "version_major == 0");
    failures += check(engine::core::version_minor() == 1, "version_minor == 1");
    failures += check(engine::core::version_patch() == 0, "version_patch == 0");

    // Exported C symbol agrees with the header (header <-> .so handshake).
    const std::uint32_t exported = engine_core_version();
    const std::uint32_t expected =
        static_cast<std::uint32_t>((engine::core::version_major() << 16) |
                                   (engine::core::version_minor() << 8) |
                                   engine::core::version_patch());
    failures += check(exported == expected, "engine_core_version matches header");
    failures += check(exported == 0x00000100u, "engine_core_version == 0.1.0");

    if (failures == 0) {
        std::printf("engine_core_smoke: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}

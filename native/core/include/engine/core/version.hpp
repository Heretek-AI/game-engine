// EngineCoreConfig — build-stamped version/config surface for libengine_core.
// M1-01 starter: proves the native pipeline (CMake -> OBJECT libs ->
// engine_core shared lib -> CTest) end to end. Expanded in M1-02 (arenas)
// and M1-03 (scheduler/ecs); the C99 ABI boundary is M1-04 (bridge/).
#pragma once

namespace engine::core {

// Semantic version of the native core (mirrors CMake PROJECT_VERSION).
constexpr int kVersionMajor = 0;
constexpr int kVersionMinor = 1;
constexpr int kVersionPatch = 0;

// Compile-time feature probes, useful to CTest smoke tests and later to the
// managed host (M1-05) when it validates the loaded native binary.
[[nodiscard]] constexpr auto version_major() noexcept -> int { return kVersionMajor; }
[[nodiscard]] constexpr auto version_minor() noexcept -> int { return kVersionMinor; }
[[nodiscard]] constexpr auto version_patch() noexcept -> int { return kVersionPatch; }

#ifdef ENGINE_HAS_AVX2
constexpr bool kBuiltWithAvx2 = true;
#else
constexpr bool kBuiltWithAvx2 = false;
#endif

}  // namespace engine::core

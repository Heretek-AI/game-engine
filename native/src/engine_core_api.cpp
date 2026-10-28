// engine_core_api.cpp — stable C ABI surface of the engine_core shared lib.
// M1-01: exports engine_core_version() only. M1-04 adds
// engine_get_api_table() (the single-entry-point C99 ABI) here.
#include <cstdint>

#include "engine/core/version.hpp"

#if defined(_WIN32)
#define ENGINE_CORE_EXPORT __declspec(dllexport)
#else
#define ENGINE_CORE_EXPORT __attribute__((visibility("default")))
#endif

extern "C" {

// Packed semver: (major << 16) | (minor << 8) | patch.
ENGINE_CORE_EXPORT auto engine_core_version() noexcept -> std::uint32_t {
    return static_cast<std::uint32_t>((engine::core::version_major() << 16) |
                                      (engine::core::version_minor() << 8) |
                                      engine::core::version_patch());
}

}  // extern "C"

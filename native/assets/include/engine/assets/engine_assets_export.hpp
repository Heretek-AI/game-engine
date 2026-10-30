// engine_assets_export.hpp — DLL visibility for the M5-01 asset module.
//
// Why a second macro: asset TUs compile inside the engine_core_assets
// OBJECT stash, which never sees ENGINE_CORE_BUILDING (that flag is stamped
// on each module's own target — core/ecs/... — and OBJECT stashes don't
// propagate it to the final engine_core DLL link). On Windows/MSVC, which
// exports nothing by default, the asset free functions (MintGuid,
// ImportFast, ...) would therefore build as dllimport inside the DLL that
// is supposed to export them -> LNK2019 in every consumer. ENGINE_ASSETS_API
// with its own building flag (set PRIVATE on engine_core_assets, same
// pattern as core/ecs) gives the asset API an independent, correct export
// surface. Linux/macOS: visibility-default, harmless.
#pragma once

#if defined(_WIN32)
#if defined(ENGINE_ASSETS_BUILDING)
#define ENGINE_ASSETS_API __declspec(dllexport)
#elif defined(ENGINE_ASSETS_STATIC)
// Static Windows lib: no import table exists, so consumers must not see
// dllimport (CodeRabbit #4420064453 — propagated PUBLIC from CMake when
// ENGINE_BUILD_SHARED=OFF; same pattern as the ENGINE_ASSETS_HAS_* flags).
#define ENGINE_ASSETS_API
#else
#define ENGINE_ASSETS_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define ENGINE_ASSETS_API __attribute__((visibility("default")))
#else
#define ENGINE_ASSETS_API
#endif

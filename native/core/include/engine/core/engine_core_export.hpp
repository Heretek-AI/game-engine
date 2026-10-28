// engine_core_export.hpp — shared-library symbol visibility for engine_core.
//
// Windows exports nothing by default: every symbol the .NET host (M1-05)
// or native tests consume from engine_core.dll must be explicitly marked.
// Linux/macOS export everything; the macro is a no-op there (except with
// -fvisibility=hidden builds, where it does the right thing).
//
// Usage: `class ENGINE_CORE_API Foo { ... };`
// The engine_core target defines ENGINE_CORE_BUILDING (see native/CMakeLists.txt).
#pragma once

#if defined(_WIN32)
#if defined(ENGINE_CORE_BUILDING)
#define ENGINE_CORE_API __declspec(dllexport)
#else
#define ENGINE_CORE_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define ENGINE_CORE_API __attribute__((visibility("default")))
#else
#define ENGINE_CORE_API
#endif

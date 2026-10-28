cmake_minimum_required(VERSION 3.28)

# ---------------------------------------------------------------------------
# GameEngine — root build.
# M1-01: unified CMake C++20 pipeline (Ninja + Clang/MSVC) for libengine_core.
#
# Layout:
#   native/   libengine_core (core, ecs, renderer, physics, audio, bridge)
#   deps/     vendored third-party (flecs v4.x, enkiTS)
#   managed/  .NET 10 solution (Engine.slnx) — built by `dotnet`, not CMake
#   tools/    build-time utilities (abi_codegen, M1-04)
# ---------------------------------------------------------------------------
project(GameEngine
    VERSION 0.1.0
    DESCRIPTION "Native C++20 + .NET 10 zero-copy game engine"
    LANGUAGES C CXX
)

# --- Global options ---------------------------------------------------------
option(ENGINE_BUILD_TESTS   "Build native smoke tests (CTest)" ON)
option(ENGINE_ENABLE_AVX2   "Enable AVX2 codegen for x86-64 hot modules" ON)
option(ENGINE_WERROR        "Treat warnings as errors in engine targets" ON)
option(ENGINE_BUILD_SHARED  "Build engine_core as a shared library" ON)

# --- Module path (strict hot-path flags live here) --------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(EngineHotFlags)

# --- Language defaults ------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)          # strict ISO C++20, no GNU extensions
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # clangd / tooling support

# Position-independent code everywhere (managed host loads us as a .so/.dll).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# --- Vendored dependencies --------------------------------------------------
# flecs v4.x (pinned tag v4.1.6, see deps/README.md): static lib only, no
# tests/examples — keeps configure fast and warnings out of our build.
set(FLECS_STATIC ON  CACHE BOOL "" FORCE)
set(FLECS_SHARED OFF CACHE BOOL "" FORCE)
set(FLECS_TESTS  OFF CACHE BOOL "" FORCE)
set(FLECS_PIC    ON  CACHE BOOL "" FORCE)
add_subdirectory(deps/flecs)

# M1-02: flecs lives INSIDE libengine_core as the single native runtime
# universe (InstallAlignedFlecsHooks patches the .so's ecs_os_api; tests
# and tools must bind to that same copy, not a second static one).
# Flecs' own CMake forces C_VISIBILITY_PRESET hidden — reset it here so
# the shared lib re-exports the ecs_* API (deliberate engine decision,
# documented in native/CMakeLists.txt at the WHOLE_ARCHIVE link).
set_target_properties(flecs_static PROPERTIES C_VISIBILITY_PRESET default)

# enkiTS (pinned HEAD 404a3bf8, see deps/README.md): static lib, C interface
# on (M1-03 binds it to Flecs task hooks), examples off.
set(ENKITS_BUILD_C_INTERFACE ON  CACHE BOOL "" FORCE)
set(ENKITS_BUILD_EXAMPLES    OFF CACHE BOOL "" FORCE)
set(ENKITS_BUILD_SHARED      OFF CACHE BOOL "" FORCE)
set(ENKITS_INSTALL           OFF CACHE BOOL "" FORCE)
add_subdirectory(deps/enkiTS)

# --- Engine core ------------------------------------------------------------
add_subdirectory(native)

# --- Tests ------------------------------------------------------------------
if(ENGINE_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(native/tests)
endif()

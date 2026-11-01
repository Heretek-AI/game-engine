# native/navigation — P6-03: Recast/Detour navmesh bake + NavAgent crowd.
#
# Recast/Detour v1.6.0 arrives via FetchContent (pinned tag + SHA256 below;
# Zlib; build-tree only, never vendored — same pattern as Jolt in
# native/physics/CMakeLists.txt). First-slice records: URL + SHA256 here, a
# row in deps/README.md, and THIRD_PARTY_NOTICES.md (per-issue notes).
#
# Targets: Recast, Detour, DetourCrowd (upstream names; no re-targeting).
# The adapter (src/*.cpp, here only navmesh.cpp) compiles with engine hot
# flags (no-exceptions/RTTI, AVX2); the fetched kernel never inherits our
# -Werror (its own warning set). Only the adapter TU textually includes
# <Recast.h>/<Detour...>: headers reach it ONLY via the wrapper interface
# below (SYSTEM include + -Wno-conversion suppression). Never add a plain
# include of the Recast source dir — -Wconversion -Werror would fire
# inside upstream headers.
include(FetchContent)

set(RECAST_TAG "v1.6.0")
set(RECAST_SHA256 "d48ca0121962fa0639502c0f56c4e3ae72f98e55d88727225444f500775c0074")

FetchContent_Declare(
    RecastNavigation
    URL "https://github.com/recastnavigation/recastnavigation/archive/refs/tags/${RECAST_TAG}.tar.gz"
    URL_HASH SHA256=${RECAST_SHA256}
)
set(FETCHCONTENT_QUIET OFF)

# Upstream builds the demo/tests/examples by default: slice them off
# (engine needs only the three runtime libs).
set(RECASTNAVIGATION_DEMO OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_TESTS OFF CACHE BOOL "" FORCE)
set(RECASTNAVIGATION_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_GetProperties(RecastNavigation)
if(NOT recastnavigation_POPULATED)
    FetchContent_Populate(RecastNavigation)
endif()

# Recast v1.6.0 declares cmake_minimum_required < 3.5, which CMake >= 4
# (macos-15 / windows-2025 runners) hard-rejects. Scope a policy floor to
# this add_subdirectory only; the engine top-level minimum (3.28) is
# untouched outside these lines.
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.5")
    if(DEFINED CMAKE_POLICY_VERSION_MINIMUM)
        set(_ENGINE_SAVED_POLICY_VERSION_MINIMUM "${CMAKE_POLICY_VERSION_MINIMUM}")
    endif()
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()
add_subdirectory("${recastnavigation_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/_recastnavigation")
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.5")
    if(DEFINED _ENGINE_SAVED_POLICY_VERSION_MINIMUM)
        set(CMAKE_POLICY_VERSION_MINIMUM "${_ENGINE_SAVED_POLICY_VERSION_MINIMUM}")
    else()
        unset(CMAKE_POLICY_VERSION_MINIMUM)
    endif()
    unset(_ENGINE_SAVED_POLICY_VERSION_MINIMUM)
endif()

# Fetched kernel: its own warning set, never our -Werror.
foreach(_recast_tgt Recast Detour DetourCrowd)
    if(TARGET ${_recast_tgt} AND NOT MSVC)
        target_compile_options(${_recast_tgt} PRIVATE -w -Wno-error)
    endif()
endforeach()

# Header-only adapter interface lib: carries only warning-safe wrappers.
# Only the adapter TU ever textually includes <Recast.h>/<Detour...>;
# everything else in the engine includes adapter headers only.
add_library(engine_navigation_recast_wrapper INTERFACE)
target_include_directories(engine_navigation_recast_wrapper SYSTEM INTERFACE
    $<BUILD_INTERFACE:${recastnavigation_SOURCE_DIR}/Recast/Include>
    $<BUILD_INTERFACE:${recastnavigation_SOURCE_DIR}/Detour/Include>
    $<BUILD_INTERFACE:${recastnavigation_SOURCE_DIR}/DetourCrowd/Include>
)
if(NOT MSVC)
    target_compile_options(engine_navigation_recast_wrapper INTERFACE
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-conversion -Wno-sign-conversion>
    )
endif()

file(GLOB NAVIGATION_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
add_library(engine_core_navigation OBJECT ${NAVIGATION_SOURCES})
add_library(engine::navigation ALIAS engine_core_navigation)

target_include_directories(engine_core_navigation PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# Building-side of ENGINE_CORE_API (same model as engine_core_core):
# the adapter class is compiled here, so this TU set must see
# dllexport on Windows. PRIVATE so it never leaks to dependents.
target_compile_definitions(engine_core_navigation PRIVATE ENGINE_CORE_BUILDING)

# Recast headers reach the adapter ONLY via the wrapper interface above
# (SYSTEM include + conversion-warning suppression). The fetched static
# libs link here (their symbols resolve into engine_core at link time).
target_link_libraries(engine_core_navigation PUBLIC
    engine_navigation_recast_wrapper
    Recast
    Detour
    DetourCrowd
    engine_core_core
    engine_core_ecs
    flecs
)

engine_apply_hot_flags(engine_core_navigation)

# native/bridge — C99 ABI + hostfxr embedding. Owner: M1-04/M1-05.
# M1-04: engine_abi.cpp implements engine_get_api_table() on EcsWorld.
# NOTE: base flags (not hot flags) — this module hosts the ABI boundary
# (vector registry may throw on OOM -> errno codes) and later hostfxr.
file(GLOB BRIDGE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
# nethost discovery: the Microsoft.NETCore.App.Host pack ships
# libnethost (static + shared) and its headers. Prefer the hermetic
# SDK layout via DOTNET_ROOT (falls back to $HOME/.dotnet):
#   ${DOTNET_ROOT}/packs/Microsoft.NETCore.App.Host.<rid>/<ver>/
# Lookup tolerates a missing pack (offline CI): dotnet_host.cpp falls back
# to DOTNET_ROOT/host/fxr at runtime, so configure never hard-fails.
# NOTE: the link below is guarded — when the pack is absent the bridge
# builds WITHOUT nethost (dotnet_host.cpp declares get_hostfxr_path
# locally and resolves hostfxr purely via DOTNET_ROOT/host/fxr).
set(DOTNET_ROOT_SDK "$ENV{DOTNET_ROOT}" CACHE PATH "dotnet install root")
if(DOTNET_ROOT_SDK STREQUAL "" OR NOT EXISTS "${DOTNET_ROOT_SDK}")
    if(DEFINED ENV{HOME})
        set(DOTNET_ROOT_SDK "$ENV{HOME}/.dotnet")
    endif()
endif()
if(WIN32)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64")
        set(NETHOST_RID "win-arm64")
    else()
        set(NETHOST_RID "win-x64")
    endif()
    set(NETHOST_LIBNAME "nethost.lib")
elseif(APPLE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
        set(NETHOST_RID "osx-arm64")
    else()
        set(NETHOST_RID "osx-x64")
    endif()
    set(NETHOST_LIBNAME "libnethost.a")
else()
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64|ARM64")
        set(NETHOST_RID "linux-arm64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm")
        set(NETHOST_RID "linux-arm")
    else()
        set(NETHOST_RID "linux-x64")
    endif()
    set(NETHOST_LIBNAME "libnethost.a")
endif()
file(GLOB NETHOST_PACK_DIRS
    "${DOTNET_ROOT_SDK}/packs/Microsoft.NETCore.App.Host.${NETHOST_RID}/*")
if(NETHOST_PACK_DIRS)
    # Natural version ordering (10.0.14 > 10.0.9): plain lexicographic
    # sort misorders multi-digit components ("10.0.9" > "10.0.14").
    # GLOB returns full paths; sort by version with a padded key.
    set(_NETHOST_SORTED "")
    foreach(_P IN LISTS NETHOST_PACK_DIRS)
        get_filename_component(_V "${_P}" NAME)
        string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _M "${_V}")
        if(_M)
            # zero-pad each component to 8 digits for correct ordering
            # (SUBSTRING with a negative begin is an error on short
            # strings, so pad first via a fixed 8-zero prefix lane).
            set(_PAD1 "00000000${CMAKE_MATCH_1}")
            set(_PAD2 "00000000${CMAKE_MATCH_2}")
            set(_PAD3 "00000000${CMAKE_MATCH_3}")
            string(LENGTH "${_PAD1}" _LEN1)
            string(LENGTH "${_PAD2}" _LEN2)
            string(LENGTH "${_PAD3}" _LEN3)
            math(EXPR _S1 "${_LEN1} - 8")
            math(EXPR _S2 "${_LEN2} - 8")
            math(EXPR _S3 "${_LEN3} - 8")
            string(SUBSTRING "${_PAD1}" ${_S1} 8 _P1)
            string(SUBSTRING "${_PAD2}" ${_S2} 8 _P2)
            string(SUBSTRING "${_PAD3}" ${_S3} 8 _P3)
            list(APPEND _NETHOST_SORTED "${_P1}.${_P2}.${_P3}|${_P}")
        endif()
    endforeach()
    if(_NETHOST_SORTED)
        list(SORT _NETHOST_SORTED)
        list(GET _NETHOST_SORTED -1 _NETHOST_BEST)
        string(REGEX REPLACE "^[^|]*\\|" "" NETHOST_PACK "${_NETHOST_BEST}")
    else()
        # No parseable versions: fall back to newest-by-name.
        list(SORT NETHOST_PACK_DIRS)
        list(GET NETHOST_PACK_DIRS -1 NETHOST_PACK)
    endif()
    set(NETHOST_NATIVE_DIR "${NETHOST_PACK}/runtimes/${NETHOST_RID}/native")
    message(STATUS "nethost pack: ${NETHOST_PACK} (rid ${NETHOST_RID})")
    if(NOT EXISTS "${NETHOST_NATIVE_DIR}/${NETHOST_LIBNAME}")
        message(STATUS "nethost lib missing: ${NETHOST_NATIVE_DIR}/${NETHOST_LIBNAME} (bridge builds without nethost)")
        unset(NETHOST_NATIVE_DIR)
    endif()
endif()

add_library(engine_core_bridge OBJECT ${BRIDGE_SOURCES})
add_library(engine::bridge ALIAS engine_core_bridge)

target_include_directories(engine_core_bridge PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# Building-side of ENGINE_CORE_API for engine_get_api_table (see
# engine_abi.h): this TU defines the export, so it must see dllexport on
# Windows. PRIVATE so it never leaks to dependents.
target_compile_definitions(engine_core_bridge PRIVATE ENGINE_CORE_BUILDING)

# The ABI implementation drives EcsWorld directly (ecs_* chunk query),
# compares layouts against transform.hpp, owns the M3-03 RenderPass
# registry (Compositor), and serves M4-03 spatial queries + M5-02 editor
# sessions + the P3-08 project-drop entry (ImportFast over staged asset
# files) + the P6-03 navigation tail: needs ecs/core + renderer + physics +
# assets + navigation headers and the shared flecs target transitively.
target_link_libraries(engine_core_bridge PUBLIC engine_core_ecs engine_core_renderer engine_core_physics engine_core_assets engine_core_navigation engine_core_audio)

# nethost: get_hostfxr_path for hostfxr discovery (M1-05 DotNetHost).
# Static libnethost.a keeps engine_core dependency-free at load time;
# NETHOST_USE_AS_STATIC avoids dllimport decoration on Linux/macOS.
# Guarded: CI images without the pack (or without the RID subdir) build
# the bridge WITHOUT nethost — ScanFxDir (DOTNET_ROOT/host/fxr) still
# resolves hostfxr at runtime, so the smoke test keeps working.
# WINDOWS CARVE-OUT (2026-10-28): linking nethost.lib statically into
# engine_core.dll hangs every test that loads the DLL on windows-2025
# CI (pre-main stall; pristine main without the link passes in 55s).
# Suspect: CRT flavor of the pack's static lib vs our /MD build, or an
# extra load-time dependency it pulls in. Until root-caused, Windows
# resolves hostfxr purely via ScanFxDir (DOTNET_ROOT/host/fxr + well-known
# prefixes in dotnet_host.cpp) — the smoke test's DOTNET_ROOT env makes
# this hermetic, and TryNethost is a stub there by construction.
if(NOT WIN32 AND DEFINED NETHOST_NATIVE_DIR AND EXISTS "${NETHOST_NATIVE_DIR}/nethost.h")
    set(NETHOST_LIB "${NETHOST_NATIVE_DIR}/${NETHOST_LIBNAME}")
    target_include_directories(engine_core_bridge PRIVATE "${NETHOST_NATIVE_DIR}")
    target_compile_definitions(engine_core_bridge PRIVATE NETHOST_USE_AS_STATIC)
    # Presence marker: dotnet_host.cpp compiles the TryNethost path only
    # when the pack was found; without it TryNethost is a stub and
    # ScanFxDir (DOTNET_ROOT/host/fxr) resolves hostfxr at runtime.
    target_compile_definitions(engine_core_bridge PRIVATE ENGINE_HAS_NETHOST=1)
    if(WIN32)
        target_link_libraries(engine_core_bridge PRIVATE "${NETHOST_LIB}")
    else()
        target_link_libraries(engine_core_bridge PRIVATE "${NETHOST_LIB}" ${CMAKE_DL_LIBS})
    endif()
else()
    message(STATUS "nethost unavailable for rid ${NETHOST_RID} under ${DOTNET_ROOT_SDK}; bridge builds without nethost (runtime DOTNET_ROOT/host/fxr fallback still applies)")
endif()

engine_apply_base_flags(engine_core_bridge)

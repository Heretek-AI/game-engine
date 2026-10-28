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
#   ${DOTNET_ROOT}/packs/Microsoft.NETCore.App.Host.linux-x64/<ver>/
# Lookup tolerates a missing pack (offline CI): dotnet_host.cpp falls back
# to DOTNET_ROOT/host/fxr at runtime, so configure never hard-fails.
set(DOTNET_ROOT_SDK "$ENV{DOTNET_ROOT}" CACHE PATH "dotnet install root")
if(DOTNET_ROOT_SDK STREQUAL "" OR NOT EXISTS "${DOTNET_ROOT_SDK}")
    if(DEFINED ENV{HOME})
        set(DOTNET_ROOT_SDK "$ENV{HOME}/.dotnet")
    endif()
endif()
file(GLOB NETHOST_PACK_DIRS
    "${DOTNET_ROOT_SDK}/packs/Microsoft.NETCore.App.Host.linux-x64/*")
if(NETHOST_PACK_DIRS)
    list(SORT NETHOST_PACK_DIRS)  # ascending: last entry = newest version
    list(GET NETHOST_PACK_DIRS -1 NETHOST_PACK)
    set(NETHOST_NATIVE_DIR "${NETHOST_PACK}/runtimes/linux-x64/native")
    message(STATUS "nethost pack: ${NETHOST_PACK}")
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

# The ABI implementation drives EcsWorld directly (ecs_* chunk query) and
# compares layouts against transform.hpp: needs ecs/core headers + the
# shared flecs target transitively.
target_link_libraries(engine_core_bridge PUBLIC engine_core_ecs)

# nethost: get_hostfxr_path for hostfxr discovery (M1-05 DotNetHost).
# Static libnethost.a keeps engine_core dependency-free at load time;
# NETHOST_USE_AS_STATIC avoids dllimport decoration on Linux/macOS.
if(DEFINED NETHOST_NATIVE_DIR AND EXISTS "${NETHOST_NATIVE_DIR}/nethost.h")
    if(WIN32)
        set(NETHOST_LIB "${NETHOST_NATIVE_DIR}/nethost.lib")
    else()
        set(NETHOST_LIB "${NETHOST_NATIVE_DIR}/libnethost.a")
    endif()
    target_include_directories(engine_core_bridge PRIVATE "${NETHOST_NATIVE_DIR}")
    target_compile_definitions(engine_core_bridge PRIVATE NETHOST_USE_AS_STATIC)
    target_link_libraries(engine_core_bridge PRIVATE "${NETHOST_LIB}" ${CMAKE_DL_LIBS})
else()
    message(STATUS "nethost headers not found under ${DOTNET_ROOT_SDK}; DotNetHost compiles without nethost include path (runtime DOTNET_ROOT/host/fxr fallback still applies)")
endif()

engine_apply_base_flags(engine_core_bridge)

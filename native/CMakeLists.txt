# ---------------------------------------------------------------------------
# native/ — libengine_core: 100% of frame state lives here (AGENTS.md).
# Managed (managed/) holds no gameplay state; it views this memory zero-copy.
#
# Module owners:
#   core     M1-02 arenas/allocators, SIMD math, EngineCoreConfig (version)
#   ecs      M1-03 Flecs world wrapper, LocalTransform/WorldTransform
#   renderer M3    Vulkan 1.3 bindless renderer + FrameGraph
#   physics  M4    Jolt <-> Flecs sync
#   audio    M4    miniaudio spatial graph
#   bridge   M1-04/M1-05 C99 ABI (engine_abi.h) + .NET hostfxr embedding
# ---------------------------------------------------------------------------

add_subdirectory(core)
add_subdirectory(ecs)
# renderer before input: it fetches SDL3 (P1-02), which input links.
add_subdirectory(renderer)
add_subdirectory(input)
add_subdirectory(physics)
add_subdirectory(audio)
add_subdirectory(assets)
add_subdirectory(navigation)

add_subdirectory(terrain)
add_subdirectory(bridge)

# Thin shared-library TU: gives `engine_core` a stable binary identity
# (engine_core.so / engine_core.dll) that the .NET host loads. All real
# implementation lives in the OBJECT modules above; see M1-05 (hostfxr).
if(ENGINE_BUILD_SHARED)
    add_library(engine_core SHARED src/engine_core_api.cpp)
    set_target_properties(engine_core PROPERTIES
        OUTPUT_NAME "engine_core"
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
    )
else()
    add_library(engine_core STATIC src/engine_core_api.cpp)
endif()

target_link_libraries(engine_core PUBLIC
    engine_core_core
    engine_core_ecs
    engine_core_input
    engine_core_renderer
    engine_core_physics
    engine_core_audio
    engine_core_assets
    engine_core_navigation

    engine_core_terrain
    engine_core_bridge
)

# Single Flecs universe: engine_core and all test/tool executables link the
# shared `flecs` target (see root CMakeLists.txt), so there is exactly one
# ecs_os_api per process on every OS. No whole-archive trick needed.

engine_apply_base_flags(engine_core)

# Windows exports nothing by default: consumers see engine_core.dll
# symbols only via ENGINE_CORE_API (dllimport). Define the building-side
# macro so the same headers expand to dllexport inside the DLL.
target_compile_definitions(engine_core PRIVATE ENGINE_CORE_BUILDING)

# M5-01 asset export surface: DLL consumers (tests, host) link the asset
# symbols through engine_core.lib. On Windows the import lib only carries
# symbols the DLL actually exports; OBJECT-stash TUs compile without the
# building macro, so force-export the asset API with an explicit .def-style
# WINDOWS_EXPORT_ALL_SYMBOLS on the engine_core target is too broad (would
# export flecs/enkiTS internals) — instead the asset headers use
# ENGINE_ASSETS_API (own macro, own building flag, header-private).
# See engine_assets_export.hpp.

target_include_directories(engine_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

# Keep the shared symbol surface minimal: only the C99 ABI entry point
# (engine_get_api_table, M1-04) is exported. Until M1-04 lands, the version
# accessor below is the sole export (see src/engine_core_api.cpp).
if(BUILD_SHARED_LIBS OR ENGINE_BUILD_SHARED)
    include(GenerateExportHeader)
endif()

install(TARGETS engine_core
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

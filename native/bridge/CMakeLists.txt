# native/bridge — C99 ABI + hostfxr embedding. Owner: M1-04/M1-05.
# M1-04: engine_abi.cpp implements engine_get_api_table() on EcsWorld.
# NOTE: base flags (not hot flags) — this module hosts the ABI boundary
# (vector registry may throw on OOM -> errno codes) and later hostfxr.
file(GLOB BRIDGE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
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

engine_apply_base_flags(engine_core_bridge)

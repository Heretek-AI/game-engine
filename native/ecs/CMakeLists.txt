# native/ecs — Flecs world wrapper, hierarchy pipeline. Owner: M1-03.
# Hot module: strict flags (no exceptions/RTTI, AVX2 on x86-64).
file(GLOB ECS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
add_library(engine_core_ecs OBJECT ${ECS_SOURCES})
add_library(engine::ecs ALIAS engine_core_ecs)

target_include_directories(engine_core_ecs PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../core/include>
)

# flecs (shared) exposes flecs::flecs; link PUBLIC so consumers
# (bridge, tests) inherit its include dirs transitively.
# P3-08: drop_asset stages files through the M5-01 fast lane
# (ImportFast/GUID/.meta), so the ecs TU links the assets module too.
target_link_libraries(engine_core_ecs PUBLIC flecs enkiTS engine_core_assets)

# Building-side of ENGINE_CORE_API (same model as engine_core_core):
# EcsWorld/transform functions are compiled here, so this TU set must
# see dllexport on Windows. Consumers (tests, host) see dllimport.
# PRIVATE so it never leaks to dependents.
target_compile_definitions(engine_core_ecs PRIVATE ENGINE_CORE_BUILDING)

engine_apply_hot_flags(engine_core_ecs)

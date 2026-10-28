# native/ecs — Flecs world wrapper, hierarchy pipeline. Owner: M1-03.
# Hot module: strict flags (no exceptions/RTTI, AVX2 on x86-64).
file(GLOB ECS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
add_library(engine_core_ecs OBJECT ${ECS_SOURCES})
add_library(engine::ecs ALIAS engine_core_ecs)

target_include_directories(engine_core_ecs PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# flecs_static exposes flecs::flecs_static; link PUBLIC so consumers
# (bridge, tests) inherit its include dirs transitively.
target_link_libraries(engine_core_ecs PUBLIC flecs_static enkiTS)

engine_apply_hot_flags(engine_core_ecs)

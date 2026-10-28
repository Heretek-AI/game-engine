# native/physics — Jolt <-> Flecs transform sync. Owner: M4.
# Stub until M4-01: builds an empty OBJECT lib so the solution graph,
# CI, and IDE project enumeration stay green.
file(GLOB PHYSICS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
add_library(engine_core_physics OBJECT ${PHYSICS_SOURCES})
add_library(engine::physics ALIAS engine_core_physics)

target_include_directories(engine_core_physics PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

engine_apply_hot_flags(engine_core_physics)

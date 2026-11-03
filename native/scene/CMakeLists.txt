# native/scene — P2-06: scene files (JSON v1) save and load.
#
# Plain OBJECT module (base flags, not hot flags — file IO serializes, and
# the bridge boundary owns all allocation). Links ecs (LocalTransform +
# render component layouts) + flecs (raw world walk).
file(GLOB SCENE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
add_library(engine_core_scene OBJECT ${SCENE_SOURCES})
add_library(engine::scene ALIAS engine_core_scene)

target_include_directories(engine_core_scene PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../core/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../ecs/include>
)

# Building-side of ENGINE_CORE_API (same model as engine_core_ecs):
# save/load + registry functions are compiled here, so this TU set must
# see dllexport on Windows. PRIVATE so it never leaks to dependents.
target_compile_definitions(engine_core_scene PRIVATE ENGINE_CORE_BUILDING)

target_link_libraries(engine_core_scene PUBLIC flecs engine_core_ecs)

engine_apply_base_flags(engine_core_scene)

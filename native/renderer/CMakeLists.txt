# native/renderer — Vulkan 1.3 bindless renderer + FrameGraph. Owner: M3.
# Stub until M3-01: builds an empty OBJECT lib so the solution graph,
# CI, and IDE project enumeration stay green.
file(GLOB RENDERER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
add_library(engine_core_renderer OBJECT ${RENDERER_SOURCES})
add_library(engine::renderer ALIAS engine_core_renderer)

target_include_directories(engine_core_renderer PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

engine_apply_hot_flags(engine_core_renderer)

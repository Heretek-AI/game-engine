# native/audio — miniaudio spatial graph. Owner: M4.
# Stub until M4-04: builds an empty OBJECT lib so the solution graph,
# CI, and IDE project enumeration stay green.
file(GLOB AUDIO_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
add_library(engine_core_audio OBJECT ${AUDIO_SOURCES})
add_library(engine::audio ALIAS engine_core_audio)

target_include_directories(engine_core_audio PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

engine_apply_hot_flags(engine_core_audio)

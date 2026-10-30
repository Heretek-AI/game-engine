# native/audio — miniaudio spatial graph (M4-04).
#
# engine_miniaudio_impl: the single MINIAUDIO_IMPLEMENTATION TU (plain C).
# miniaudio's implementation trips -Wconversion/-Wsign-conversion under our
# strict flags, so this TU compiles with base (non-hot) flags + targeted
# -Wno-* suppressions. SYSTEM include keeps the vendored header's own
# warnings out of every consumer. No exceptions/RTTI concerns (pure C).
#
# engine_core_audio: the engine adapter (components, AudioWorld spatializer
# system) — hot module: strict flags, AVX2, no exceptions/RTTI.
add_library(engine_miniaudio_impl STATIC src/miniaudio_impl.c)
target_include_directories(engine_miniaudio_impl SYSTEM PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../deps/miniaudio>
)
if(MSVC)
    target_compile_options(engine_miniaudio_impl PRIVATE /W1)
else()
    target_compile_options(engine_miniaudio_impl PRIVATE
        -Wno-conversion -Wno-sign-conversion -Wno-unused-parameter
        -Wno-unused-function -Wno-unused-variable -Wno-missing-field-initializers
        -Wno-extra -Wno-pedantic
    )
endif()

file(GLOB AUDIO_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
list(FILTER AUDIO_SOURCES EXCLUDE REGEX ".*miniaudio_impl.*")
add_library(engine_core_audio OBJECT ${AUDIO_SOURCES})
add_library(engine::audio ALIAS engine_core_audio)

# Building-side of ENGINE_CORE_API (same model as ecs/core): AudioWorld is
# compiled here, so this TU set must see dllexport on Windows. PRIVATE.
target_compile_definitions(engine_core_audio PRIVATE ENGINE_CORE_BUILDING)

target_include_directories(engine_core_audio PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# Adapter needs miniaudio declarations (SYSTEM: no warning leak), the ECS
# world for WorldTransform registration order, and shared flecs.
target_link_libraries(engine_core_audio PUBLIC
    engine_miniaudio_impl
    engine_core_ecs
    flecs
)

engine_apply_hot_flags(engine_core_audio)

# The implementation TU must not inherit -Werror/hot flags: strip the
# directory-level strictness back to a quiet C compile.
if(NOT MSVC)
    target_compile_options(engine_miniaudio_impl PRIVATE -Wno-error)
endif()

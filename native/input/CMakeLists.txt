# native/input — P2-01: keyboard/mouse/gamepad state fed from SDL events.
# Hot module: strict flags (no exceptions/RTTI, AVX2 on x86-64).
file(GLOB INPUT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
add_library(engine_core_input OBJECT ${INPUT_SOURCES})
add_library(engine::input ALIAS engine_core_input)

target_include_directories(engine_core_input PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../core/include>
)

# Building-side of ENGINE_CORE_API (same model as engine_core_core):
# InputState member functions are compiled here, so this TU set must
# see dllexport on Windows. Consumers (tests, host) see dllimport.
target_compile_definitions(engine_core_input PRIVATE ENGINE_CORE_BUILDING)

# SDL3: the renderer module fetches and statically links SDL3 3.4.18
# (P1-02, ENGINE_WITH_SDL3=ON by default) and is configured first, so link
# the same static target. With ENGINE_WITH_SDL3=OFF the input TU compiles
# to a stub that ignores events, and the test skips the SDL-injection
# checks (ENGINE_INPUT_HAS_SDL3 undefined).
if(TARGET SDL3::SDL3-static)
    target_link_libraries(engine_core_input PUBLIC SDL3::SDL3-static)
    target_compile_definitions(engine_core_input PUBLIC
        ENGINE_INPUT_HAS_SDL3=1)
    message(STATUS "input: SDL3 (static, from renderer fetch) — real event path")
else()
    message(STATUS "input: no SDL3 — stub InputState (ignores events)")
endif()

engine_apply_hot_flags(engine_core_input)

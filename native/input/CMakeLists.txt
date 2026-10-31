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

# SDL3 (same probe shape as the renderer module): real SDL3 when found,
# otherwise the input TU compiles to a stub that ignores events. The test
# then skips the SDL-injection checks (ENGINE_INPUT_HAS_SDL3 undefined).
find_package(SDL3 QUIET)
if(SDL3_FOUND)
    target_link_libraries(engine_core_input PUBLIC SDL3::SDL3)
    target_compile_definitions(engine_core_input PUBLIC
        ENGINE_INPUT_HAS_SDL3=1)
    message(STATUS "input: SDL3 found — real event path")
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL3_PC QUIET sdl3)
    endif()
    if(SDL3_PC_FOUND)
        target_include_directories(engine_core_input PUBLIC
            ${SDL3_PC_INCLUDE_DIRS})
        target_link_libraries(engine_core_input PUBLIC ${SDL3_PC_LIBRARIES})
        target_compile_definitions(engine_core_input PUBLIC
            ENGINE_INPUT_HAS_SDL3=1)
        message(STATUS "input: SDL3 via pkg-config — real event path")
    else()
        message(STATUS "input: no SDL3 — stub InputState (ignores events)")
    endif()
endif()

engine_apply_hot_flags(engine_core_input)

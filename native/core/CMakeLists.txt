# native/core — arenas, SIMD math, EngineCoreConfig. Owner: M1-02.
# Hot module: strict flags (no exceptions/RTTI, AVX2 on x86-64).
file(GLOB CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
add_library(engine_core_core OBJECT ${CORE_SOURCES})
add_library(engine::core ALIAS engine_core_core)

# Building-side of ENGINE_CORE_API: the allocator/hook member functions are
# compiled here, so this TU set must see dllexport. Consumers (tests, host)
# see dllimport. PRIVATE so it never leaks to dependents.
target_compile_definitions(engine_core_core PRIVATE ENGINE_CORE_BUILDING)

target_include_directories(engine_core_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# aligned_flecs_hooks.cpp includes flecs.h (OS hook table); page_arena uses
# std::mutex. Both come transitively via flecs (shared) / Threads.
find_package(Threads REQUIRED)
target_link_libraries(engine_core_core PUBLIC flecs Threads::Threads)

engine_apply_hot_flags(engine_core_core)

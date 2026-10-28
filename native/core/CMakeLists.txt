# native/core — arenas, SIMD math, EngineCoreConfig. Owner: M1-02.
# Hot module: strict flags (no exceptions/RTTI, AVX2 on x86-64).
file(GLOB CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
add_library(engine_core_core OBJECT ${CORE_SOURCES})
add_library(engine::core ALIAS engine_core_core)

target_include_directories(engine_core_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

engine_apply_hot_flags(engine_core_core)

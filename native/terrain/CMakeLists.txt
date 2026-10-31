# native/terrain — P6-01: CPU terrain (heightmap, chunked LOD, splat).
#
# Owns the elevation grid (R16 PNG via stb_image), the chunked mesh builder
# with LOD index decimation, the 4-layer splat blend contract, and the Jolt
# HeightFieldShape static collider. The collider TU is the ONLY terrain TU
# that textually includes <Jolt/...>; it links the
# engine_physics_jolt_wrapper interface (SYSTEM + -Wno-conversion), so
# -Wconversion -Werror never fires inside Jolt headers.
file(GLOB TERRAIN_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
add_library(engine_core_terrain OBJECT ${TERRAIN_SOURCES})
add_library(engine::terrain ALIAS engine_core_terrain)

target_include_directories(engine_core_terrain PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../core/include>
)

# Building-side of ENGINE_CORE_API (same model as engine_core_core):
# the terrain classes are compiled here, so this TU set must see
# dllexport on Windows. PRIVATE so it never leaks to dependents.
target_compile_definitions(engine_core_terrain PRIVATE ENGINE_CORE_BUILDING)

# stb_image declarations for the heightmap TU: reuse the asset module's
# fetch (never a second fetch — offline configure would diverge). The
# implementation TU lives in engine_core_assets; this module only needs
# the header dir + feature define when the fetch populated. NOTE: the
# assets module must configure first (native/ lists assets before terrain).
get_target_property(_terrain_stb_dir engine_stb_image INTERFACE_INCLUDE_DIRECTORIES)
if(_terrain_stb_dir)
    target_compile_definitions(engine_core_terrain PUBLIC ENGINE_TERRAIN_HAS_STB=1)
    target_include_directories(engine_core_terrain PUBLIC ${_terrain_stb_dir})
    target_link_libraries(engine_core_terrain PUBLIC engine_stb_image)
endif()

# Jolt headers reach the collider TU ONLY via the wrapper interface
# (SYSTEM include + conversion-warning suppression). Never add
# JOLTPHYSICS_ROOT as a plain include here: -Wconversion -Werror would
# fire inside Jolt headers (verified prior failure — wrapper exists for it).
target_link_libraries(engine_core_terrain PUBLIC
    engine_physics_jolt_wrapper
    Jolt
    engine_core_core
    engine_core_physics
)

engine_apply_hot_flags(engine_core_terrain)

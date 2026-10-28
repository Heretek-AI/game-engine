# native/bridge — C99 ABI + hostfxr embedding. Owner: M1-04/M1-05.
# Stub until M1-04 (engine_abi.h): builds an empty OBJECT lib so the
# solution graph, CI, and IDE project enumeration stay green.
# NOTE: base flags (not hot flags) — this module will host OS loader calls
# and hostfxr interop, which need exceptions/RTTI at the boundary.
file(GLOB BRIDGE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
add_library(engine_core_bridge OBJECT ${BRIDGE_SOURCES})
add_library(engine::bridge ALIAS engine_core_bridge)

target_include_directories(engine_core_bridge PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

engine_apply_base_flags(engine_core_bridge)

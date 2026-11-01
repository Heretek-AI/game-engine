# native/net — ENet transport + snapshot replication + RPCs. Owner: P7-04.
#
# ENet v1.3.18 arrives via FetchContent (pinned tag v1.3.18 + SHA256 below,
# MIT; §3.4/§4.15.1). Fetched source lands in the build tree, never in the
# git tree. First slice adding it: URL + hash here, row in deps/README.md,
# license text in THIRD_PARTY_NOTICES.md (per-issue notes).
#
# Build: ENet compiles as its own static lib with upstream flags (plain C).
# The engine adapter (src/*.cpp) compiles with engine base flags
# (no-exceptions/RTTI) but NOT hot flags: sessions do socket I/O and may
# block in enet_host_service, so they never run on the frame path.
# No exceptions cross the boundary: all entry points are noexcept and
# return null/0/errno-style codes on failure.
#
# Target review surface: `enet` is the fetched static lib (no `add_subdirectory`
# — ENet's CMakeLists installs targets unconditionally; we build only the
# sources we need via an explicit source list from the fetched dir).
include(FetchContent)

set(ENET_TAG "v1.3.18")
set(ENET_SHA256 "28603c895f9ed24a846478180ee72c7376b39b4bb1287b73877e5eae7d96b0dd")

FetchContent_Declare(
    ENet
    URL "https://github.com/lsalzman/enet/archive/refs/tags/${ENET_TAG}.tar.gz"
    URL_HASH SHA256=${ENET_SHA256}
)

FetchContent_GetProperties(ENet)
if(NOT enet_POPULATED)
    FetchContent_Populate(ENet)
endif()
if(NOT EXISTS "${enet_SOURCE_DIR}/include/enet/enet.h")
    message(FATAL_ERROR "ENet source missing enet.h under ${enet_SOURCE_DIR}")
endif()

set(ENET_SOURCES
    "${enet_SOURCE_DIR}/callbacks.c"
    "${enet_SOURCE_DIR}/compress.c"
    "${enet_SOURCE_DIR}/host.c"
    "${enet_SOURCE_DIR}/list.c"
    "${enet_SOURCE_DIR}/packet.c"
    "${enet_SOURCE_DIR}/peer.c"
    "${enet_SOURCE_DIR}/protocol.c"
    "${enet_SOURCE_DIR}/unix.c"
    "${enet_SOURCE_DIR}/win32.c"
)
set(ENET_EXISTING_SOURCES "")
foreach(f IN LISTS ENET_SOURCES)
    if(EXISTS "${f}")
        list(APPEND ENET_EXISTING_SOURCES "${f}")
    endif()
endforeach()

add_library(enet STATIC ${ENET_EXISTING_SOURCES})
target_include_directories(enet PUBLIC
    $<BUILD_INTERFACE:${enet_SOURCE_DIR}/include>
)
if(NOT MSVC)
    target_compile_options(enet PRIVATE -w -Wno-error)
else()
    target_compile_options(enet PRIVATE /W1)
endif()
set_target_properties(enet PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

file(GLOB NET_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
add_library(engine_core_net OBJECT ${NET_SOURCES})
add_library(engine::net ALIAS engine_core_net)

target_include_directories(engine_core_net PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# Building-side of ENGINE_CORE_API: the session class is compiled here.
target_compile_definitions(engine_core_net PRIVATE ENGINE_CORE_BUILDING)

# ENet headers reach the adapter ONLY as SYSTEM includes (fetched C
# kernel: never our -Wconversion -Werror). Link enet + core (export
# macros) + ecs (LocalTransform layout for snapshot rows).
target_include_directories(engine_core_net SYSTEM PRIVATE
    ${enet_SOURCE_DIR}/include
)
target_link_libraries(engine_core_net PUBLIC
    enet
    engine_core_core
    engine_core_ecs
)

engine_apply_base_flags(engine_core_net)

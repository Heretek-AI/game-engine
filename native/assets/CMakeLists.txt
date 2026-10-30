# native/assets — M5-01: non-blocking background asset pipeline.
#
# Owns GUIDs, .meta sidecars, mmap fast path, header preview parsers,
# the Assets/ watcher, and the background import worker (meshopt + bc7enc).
# Hot module: strict flags (no-exceptions/RTTI, AVX2 on x86-64) — the fast
# lane runs on the editor's frame thread.
#
# Third-party import codecs arrive via FetchContent into the BUILD tree
# (never vendored — review surface stays engine-only):
#   meshoptimizer v0.24  (vertex cache + overdraw + simplify LOD)
#   bc7enc        @f66c2e4 (BC7 texture compression; rgbcxbulit in)
#   cgltf         v1.15   (single-header glTF parse)
#   stb           master@6/2026 (stb_image decode)
# Offline configure degrades gracefully: the adapter compiles with
# ENGINE_ASSETS_HAS_* unset and ProcessOne reports "pipeline unavailable"
# (fast lane + watcher + GUID/.meta still fully testable).
include(FetchContent)

set(ENGINE_ASSETS_HAS_MESHOPT OFF)
set(ENGINE_ASSETS_HAS_BC7 OFF)
set(ENGINE_ASSETS_HAS_CGLTF OFF)
set(ENGINE_ASSETS_HAS_STB OFF)

# --- meshoptimizer -----------------------------------------------------------
FetchContent_Declare(
    _engine_meshopt
    URL "https://github.com/zeux/meshoptimizer/archive/refs/tags/v0.24.tar.gz"
)
FetchContent_GetProperties(_engine_meshopt)
if(NOT _engine_meshopt_POPULATED)
    FetchContent_Populate(_engine_meshopt)
endif()
set(MESHOPT_SRC "${_engine_meshopt_SOURCE_DIR}/src/meshoptimizer.h"
                "${_engine_meshopt_SOURCE_DIR}/src/allocator.cpp"
                "${_engine_meshopt_SOURCE_DIR}/src/indexgenerator.cpp"
                "${_engine_meshopt_SOURCE_DIR}/src/indexcodec.cpp"
                "${_engine_meshopt_SOURCE_DIR}/src/overdrawoptimizer.cpp"
                "${_engine_meshopt_SOURCE_DIR}/src/vcacheoptimizer.cpp"
                "${_engine_meshopt_SOURCE_DIR}/src/vertexcacheoptimizer.cpp"
                "${_engine_meshopt_SOURCE_DIR}/src/simplifier.cpp"
                "${_engine_meshopt_SOURCE_DIR}/src/partition.cpp"
                "${_engine_meshopt_SOURCE_DIR}/src/spatialorder.cpp"
                "${_engine_meshopt_SOURCE_DIR}/src/stripifier.cpp"
                "${_engine_meshopt_SOURCE_DIR}/src/quantization.cpp"
                "${_engine_meshopt_SOURCE_DIR}/src/clusterizer.cpp")
# Prune to files that exist (upstream renames TUs between tags; missing
# entries would break configure on every bump).
set(MESHOPT_EXISTING_SRCS "")
foreach(f IN LISTS MESHOPT_SRC)
    if(EXISTS "${f}")
        list(APPEND MESHOPT_EXISTING_SRCS "${f}")
    endif()
endforeach()
# meshopt needs at least its public header + a few TUs; the API set we use
# (remap/optimize/simplify) lives in indexgenerator/vcacheoptimizer/
# overdrawoptimizer/simplifier — require those four or degrade.
# NOTE: vcacheoptimizer.cpp is the filename at v0.24+ (renamed from
# vertexcacheoptimizer.cpp); accept either.
set(_MESHOPT_NEED "meshoptimizer.h;indexgenerator.cpp;overdrawoptimizer.cpp;simplifier.cpp")
set(_MESHOPT_OK TRUE)
foreach(need IN LISTS _MESHOPT_NEED)
    set(_found FALSE)
    foreach(f IN LISTS MESHOPT_EXISTING_SRCS)
        if(f MATCHES "${need}$")
            set(_found TRUE)
        endif()
    endforeach()
    if(NOT _found)
        set(_MESHOPT_OK FALSE)
    endif()
endforeach()
# vcacheoptimizer OR vertexcacheoptimizer (rename across tags).
set(_vcache_ok FALSE)
foreach(f IN LISTS MESHOPT_EXISTING_SRCS)
    if(f MATCHES "(vcacheoptimizer|vertexcacheoptimizer)\\.cpp$")
        set(_vcache_ok TRUE)
    endif()
endforeach()
if(NOT _vcache_ok)
    set(_MESHOPT_OK FALSE)
endif()
if(_MESHOPT_OK)
    set(ENGINE_ASSETS_HAS_MESHOPT ON)
    message(STATUS "assets: meshoptimizer v0.24 populated (${_engine_meshopt_SOURCE_DIR})")
else()
    message(STATUS "assets: meshoptimizer incomplete (offline?) — model jobs degrade")
endif()

# --- bc7enc ------------------------------------------------------------------
FetchContent_Declare(
    _engine_bc7enc
    URL "https://github.com/richgel999/bc7enc/archive/f66c2e489b07138f2673a2fb3d27c1aa1d565c48.tar.gz"
)
FetchContent_GetProperties(_engine_bc7enc)
if(NOT _engine_bc7enc_POPULATED)
    FetchContent_Populate(_engine_bc7enc)
endif()
set(_BC7_SRC "${_engine_bc7enc_SOURCE_DIR}/bc7enc.c")
set(_BC7_HDR "${_engine_bc7enc_SOURCE_DIR}/bc7enc.h")
if(EXISTS "${_BC7_SRC}" AND EXISTS "${_BC7_HDR}")
    set(ENGINE_ASSETS_HAS_BC7 ON)
    message(STATUS "assets: bc7enc populated (${_engine_bc7enc_SOURCE_DIR})")
else()
    message(STATUS "assets: bc7enc missing (offline?) — texture jobs degrade")
endif()

# --- cgltf -------------------------------------------------------------------
FetchContent_Declare(
    _engine_cgltf
    URL "https://github.com/jkuhlmann/cgltf/archive/refs/tags/v1.15.tar.gz"
)
FetchContent_GetProperties(_engine_cgltf)
if(NOT _engine_cgltf_POPULATED)
    FetchContent_Populate(_engine_cgltf)
endif()
set(_CGLTF_HDR "${_engine_cgltf_SOURCE_DIR}/cgltf.h")
if(EXISTS "${_CGLTF_HDR}")
    set(ENGINE_ASSETS_HAS_CGLTF ON)
    message(STATUS "assets: cgltf v1.15 populated")
else()
    message(STATUS "assets: cgltf missing (offline?) — model jobs degrade")
endif()

# --- stb (image decode) ------------------------------------------------------
FetchContent_Declare(
    _engine_stb
    URL "https://github.com/nothings/stb/archive/master.tar.gz"
)
FetchContent_GetProperties(_engine_stb)
if(NOT _engine_stb_POPULATED)
    FetchContent_Populate(_engine_stb)
endif()
set(_STB_HDR "${_engine_stb_SOURCE_DIR}/stb_image.h")
if(EXISTS "${_STB_HDR}")
    set(ENGINE_ASSETS_HAS_STB ON)
    message(STATUS "assets: stb_image populated")
else()
    message(STATUS "assets: stb_image missing (offline?) — texture jobs degrade")
endif()

# --- engine adapter ----------------------------------------------------------
file(GLOB ASSETS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
add_library(engine_core_assets OBJECT ${ASSETS_SOURCES})
add_library(engine::assets ALIAS engine_core_assets)

target_include_directories(engine_core_assets PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../core/include>
)

target_compile_definitions(engine_core_assets PRIVATE ENGINE_CORE_BUILDING)
if(ENGINE_ASSETS_HAS_MESHOPT)
    target_compile_definitions(engine_core_assets PUBLIC ENGINE_ASSETS_HAS_MESHOPT=1)
    target_include_directories(engine_core_assets PUBLIC "${_engine_meshopt_SOURCE_DIR}/src")
    # meshopt TUs compiled into the adapter OBJECT lib (upstream-compatible:
    # C++11, exceptions/RTTI ON — meshopt is batch math, not frame state, so
    # engine hot flags never apply; -w silences upstream warnings under
    # -Werror).
    add_library(engine_meshopt STATIC ${MESHOPT_EXISTING_SRCS})
    target_include_directories(engine_meshopt PUBLIC "${_engine_meshopt_SOURCE_DIR}/src")
    if(NOT MSVC)
        target_compile_options(engine_meshopt PRIVATE -w)
    endif()
    target_link_libraries(engine_core_assets PUBLIC engine_meshopt)
endif()
if(ENGINE_ASSETS_HAS_BC7)
    target_compile_definitions(engine_core_assets PUBLIC ENGINE_ASSETS_HAS_BC7=1)
    target_include_directories(engine_core_assets PUBLIC "${_engine_bc7enc_SOURCE_DIR}")
    add_library(engine_bc7enc STATIC "${_BC7_SRC}")
    target_include_directories(engine_bc7enc PUBLIC "${_engine_bc7enc_SOURCE_DIR}")
    if(NOT MSVC)
        target_compile_options(engine_bc7enc PRIVATE -w)
    endif()
    target_link_libraries(engine_core_assets PUBLIC engine_bc7enc)
endif()
if(ENGINE_ASSETS_HAS_CGLTF)
    target_compile_definitions(engine_core_assets PUBLIC ENGINE_ASSETS_HAS_CGLTF=1)
    target_include_directories(engine_core_assets PUBLIC "${_engine_cgltf_SOURCE_DIR}")
endif()
if(ENGINE_ASSETS_HAS_STB)
    target_compile_definitions(engine_core_assets PUBLIC ENGINE_ASSETS_HAS_STB=1)
    target_include_directories(engine_core_assets PUBLIC "${_engine_stb_SOURCE_DIR}")
    # stb_image implementation TU (single place — the adapter only #includes
    # the header for declarations).
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/stb_image_impl.c"
        "#define STB_IMAGE_IMPLEMENTATION\n#include \"stb_image.h\"\n")
    add_library(engine_stb_image STATIC "${CMAKE_CURRENT_BINARY_DIR}/stb_image_impl.c")
    target_include_directories(engine_stb_image PUBLIC "${_engine_stb_SOURCE_DIR}")
    if(NOT MSVC)
        target_compile_options(engine_stb_image PRIVATE -w)
    endif()
    target_link_libraries(engine_core_assets PUBLIC engine_stb_image)
endif()

find_package(Threads REQUIRED)
target_link_libraries(engine_core_assets PUBLIC flecs enkiTS Threads::Threads)

# Base flags (not hot flags): this module is editor host tooling — file I/O,
# std::thread worker, third-party parse entry points — not the frame path.
# The frame-path fast parsers inside are noexcept/alloc-free by construction.
engine_apply_base_flags(engine_core_assets)

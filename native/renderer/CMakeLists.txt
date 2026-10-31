# native/renderer — Vulkan 1.3 bootstrap + RenderGraph. Owner: M3-01.
#
# Sources:
#   render_graph.cpp    CPU-side DAG planner (always built, no SDK needed).
#   renderer_window.cpp SDL3 window (real SDL3 when found, stub otherwise).
#   vulkan_device.cpp   volk/VMA bootstrap (real when Vulkan headers found,
#                       stub reporting kVulkanLoaderFailed otherwise).
#
# Dependencies are wired WITHOUT CMake find_package / FetchContent so
# offline CI stays green:
#   * Vulkan headers: ENGINE_RENDERER_HAS_VULKAN is defined when
#     <vulkan/vulkan.h> is resolvable. CI installs libvulkan-dev; dev
#     machines may set VULKAN_SDK. volk/ and VMA live in deps/ (vendored).
#   * SDL3: SDL3::SDL3 target when installed (libsdl3-dev on Debian,
#     SDL3 framework via pkg-config/Homebrew/FetchContent elsewhere).
#     Absent -> SDL3 stub: window reports kSdlInitFailed, Vulkan headless
#     path still works.
file(GLOB RENDERER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
add_library(engine_core_renderer OBJECT ${RENDERER_SOURCES})
add_library(engine::renderer ALIAS engine_core_renderer)

target_include_directories(engine_core_renderer PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    # engine_core_export.hpp lives in core's include dir (ecs pattern).
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../core/include>
    # P2-01: the input-feeding pump_events overload needs InputState.
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../input/include>
)

# Building-side of ENGINE_CORE_API (mirrors core/ecs: the renderer member
# functions are compiled here, so this TU set must see dllexport while
# test consumers see dllimport; without it Windows links fail LNK2019).
target_compile_definitions(engine_core_renderer PRIVATE ENGINE_CORE_BUILDING)

# --- Vulkan headers ---------------------------------------------------------
# Prefer CMake's FindVulkan (VULKAN_SDK env or system install); fall back
# to a raw header probe so minimal images with only the loader headers
# still enable the real path.
find_package(Vulkan QUIET)
# Local + parent scope: this directory's `if()` below needs the value AND
# native/CMakeLists.txt gates the volk --dynamic-list export on it (M3-02).
if(Vulkan_FOUND AND DEFINED Vulkan_INCLUDE_DIRS)
    set(ENGINE_HAS_VULKAN_HEADERS TRUE)
    set(ENGINE_HAS_VULKAN_HEADERS TRUE PARENT_SCOPE)
elseif(DEFINED ENV{VULKAN_SDK} AND EXISTS "$ENV{VULKAN_SDK}/include/vulkan/vulkan.h")
    set(ENGINE_HAS_VULKAN_HEADERS TRUE)
    set(ENGINE_HAS_VULKAN_HEADERS TRUE PARENT_SCOPE)
elseif(EXISTS "/usr/include/vulkan/vulkan.h")
    set(ENGINE_HAS_VULKAN_HEADERS TRUE)
    set(ENGINE_HAS_VULKAN_HEADERS TRUE PARENT_SCOPE)
else()
    set(ENGINE_HAS_VULKAN_HEADERS FALSE)
    set(ENGINE_HAS_VULKAN_HEADERS FALSE PARENT_SCOPE)
endif()

if(ENGINE_HAS_VULKAN_HEADERS)
    target_compile_definitions(engine_core_renderer PUBLIC
        ENGINE_RENDERER_HAS_VULKAN=1)
    if(Vulkan_FOUND AND DEFINED Vulkan_INCLUDE_DIRS)
        target_include_directories(engine_core_renderer PUBLIC
            ${Vulkan_INCLUDE_DIRS})
    elseif(DEFINED ENV{VULKAN_SDK})
        target_include_directories(engine_core_renderer PUBLIC
            "$ENV{VULKAN_SDK}/include")
    endif()
    # volk: meta-loader (links dl only; loads libvulkan.so.1 at runtime).
    # Compiled STRAIGHT INTO the renderer OBJECT lib (not a separate static
    # archive): a separate engine_volk archive would propagate through the
    # OBJECT->SHARED->exe link closure (CMake splices OBJECT link deps into
    # every final link), defining a SECOND copy of volk's globals in each
    # test exe — a copy that is never volkInitialize()d (init runs inside
    # libengine_core.so on ITS copy), leaving every vk* the .so reads null
    # while the exe's own copy works. Separate .bss per DSO means no
    # visibility flag or -Bsymbolic can reunite them; single compilation is
    # the only correct shape (M3-02 root-cause: engine_gpu_indirect device
    # init failed exactly this way).
    target_sources(engine_core_renderer PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../deps/volk/volk.c")
    target_include_directories(engine_core_renderer PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/../../deps/volk")
    if(NOT WIN32)
        find_package(Threads REQUIRED)
        target_link_libraries(engine_core_renderer PUBLIC ${CMAKE_DL_LIBS})
    endif()
    # VMA: header-only allocator. It cannot survive our -Werror hot flags
    # (nullability extensions, unused params in third-party code), so
    # vma_impl.cpp gets base flags instead of hot flags: it keeps
    # exceptions/RTTI (VMA internals need them) and carries -Wno-*
    # relaxations. COMPILE_OPTIONS ordering beats target options, so the
    # -Wno-* suppression reliably wins over the inherited -Werror.
    target_include_directories(engine_core_renderer PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../deps/vma/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src")  # vma_bridge.hpp (internal)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        set_source_files_properties(
            "${CMAKE_CURRENT_SOURCE_DIR}/src/vma_impl.cpp"
            PROPERTIES COMPILE_OPTIONS
                "-Wno-nullability-extension;-Wno-nullability-completeness;-Wno-unused-parameter;-Wno-unused-function;-Wno-missing-field-initializers;-Wno-conversion;-Wno-sign-conversion;-Wno-shorten-64-to-32;-Wno-tautological-unsigned-zero-compare;-Wno-unused-variable;-fno-strict-aliasing")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set_source_files_properties(
            "${CMAKE_CURRENT_SOURCE_DIR}/src/vma_impl.cpp"
            PROPERTIES COMPILE_OPTIONS
                "-Wno-unused-parameter;-Wno-unused-function;-Wno-missing-field-initializers;-Wno-conversion;-Wno-sign-conversion;-Wno-unused-variable;-fno-strict-aliasing")
    elseif(MSVC)
        set_source_files_properties(
            "${CMAKE_CURRENT_SOURCE_DIR}/src/vma_impl.cpp"
            PROPERTIES COMPILE_OPTIONS "/wd4189;/wd4100;/wd4324")
    endif()
    message(STATUS "renderer: Vulkan SDK headers found — real volk/VMA path")
else()
    message(STATUS
        "renderer: no Vulkan headers — stub VulkanDevice (M3-02 enables)")
endif()

# --- SDL3 (FetchContent, pinned tarball) ---------------------------------------
# P1-02: SDL3 release-3.4.18 fetched at configure time and linked STATIC
# into engine_core_renderer, so the real window path works on machines
# without a system SDL3 package (Ubuntu 24.04 has none). Pinned by
# tarball URL + SHA256 (dependency rule §3.4); row in deps/README.md +
# entry in THIRD_PARTY_NOTICES.md.
option(ENGINE_WITH_SDL3 "Fetch and statically link SDL3 for real windows" ON)
if(ENGINE_WITH_SDL3)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        URL "https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.18.tar.gz"
        URL_HASH SHA256=c4b08b950bd29d83caae0ab8d884298d27046a9d30700fb42841c50992665c90)
    FetchContent_MakeAvailable(SDL3)
    target_link_libraries(engine_core_renderer PUBLIC SDL3::SDL3-static)
    target_compile_definitions(engine_core_renderer PUBLIC ENGINE_RENDERER_HAS_SDL3=1)
    message(STATUS "renderer: SDL3 release-3.4.18 (static) — real window path")
else()
    message(STATUS "renderer: ENGINE_WITH_SDL3=OFF — stub RendererWindow (headless Vulkan OK)")
endif()

# P4-01 material import needs the cgltf include dir + flag owned by the
# assets module (its FetchContent populates _engine_cgltf). The assets
# OBJECT lib already carries both PUBLIC, so link it: the material_import
# TU resolves cgltf.h through that usage requirement on every platform.
# No new FetchContent here (single population point stays in assets).
target_link_libraries(engine_core_renderer PUBLIC engine_core_assets)

engine_apply_hot_flags(engine_core_renderer)

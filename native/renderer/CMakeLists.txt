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
)

# --- Vulkan headers ---------------------------------------------------------
# Prefer CMake's FindVulkan (VULKAN_SDK env or system install); fall back
# to a raw header probe so minimal images with only the loader headers
# still enable the real path.
find_package(Vulkan QUIET)
set(ENGINE_HAS_VULKAN_HEADERS FALSE)
if(Vulkan_FOUND AND DEFINED Vulkan_INCLUDE_DIRS)
    set(ENGINE_HAS_VULKAN_HEADERS TRUE)
elseif(DEFINED ENV{VULKAN_SDK} AND EXISTS "$ENV{VULKAN_SDK}/include/vulkan/vulkan.h")
    set(ENGINE_HAS_VULKAN_HEADERS TRUE)
elseif(EXISTS "/usr/include/vulkan/vulkan.h")
    set(ENGINE_HAS_VULKAN_HEADERS TRUE)
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
    add_library(engine_volk STATIC
        "${CMAKE_CURRENT_SOURCE_DIR}/../../deps/volk/volk.c")
    target_include_directories(engine_volk PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/../../deps/volk")
    if(NOT WIN32)
        find_package(Threads REQUIRED)
        target_link_libraries(engine_volk PUBLIC ${CMAKE_DL_LIBS})
    endif()
    target_link_libraries(engine_core_renderer PUBLIC engine_volk)
    # VMA: header-only allocator. It cannot survive our -Werror hot flags
    # (nullability extensions, unused params in third-party code), so
    # vma_impl.cpp gets base flags instead of hot flags: it keeps
    # exceptions/RTTI (VMA internals need them) and carries -Wno-*
    # relaxations. COMPILE_OPTIONS ordering beats target options, so the
    # -Wno-* suppression reliably wins over the inherited -Werror.
    target_include_directories(engine_core_renderer PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../deps/vma/include")
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        set_source_files_properties(
            "${CMAKE_CURRENT_SOURCE_DIR}/src/vma_impl.cpp"
            PROPERTIES COMPILE_OPTIONS
                "-Wno-nullability-extension;-Wno-nullability-completeness;-Wno-unused-parameter;-Wno-unused-function;-Wno-missing-field-initializers;-Wno-conversion;-Wno-sign-conversion;-Wno-shorten-64-to-32;-Wno-tautological-unsigned-zero-compare;-Wno-unused-variable;-fno-strict-aliasing")
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

# --- SDL3 (optional) ----------------------------------------------------------
find_package(SDL3 QUIET)
if(SDL3_FOUND)
    target_link_libraries(engine_core_renderer PUBLIC SDL3::SDL3)
    target_compile_definitions(engine_core_renderer PUBLIC
        ENGINE_RENDERER_HAS_SDL3=1)
    message(STATUS "renderer: SDL3 found — real window path")
else()
    # pkg-config fallback (Debian libsdl3-dev ships sdl3.pc without a
    # CMake config package in some releases).
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL3_PC QUIET sdl3)
    endif()
    if(SDL3_PC_FOUND)
        target_include_directories(engine_core_renderer PUBLIC
            ${SDL3_PC_INCLUDE_DIRS})
        target_link_libraries(engine_core_renderer PUBLIC ${SDL3_PC_LIBRARIES})
        target_compile_definitions(engine_core_renderer PUBLIC
            ENGINE_RENDERER_HAS_SDL3=1)
        message(STATUS "renderer: SDL3 via pkg-config — real window path")
    else()
        message(STATUS
            "renderer: no SDL3 — stub RendererWindow (headless Vulkan OK)")
    endif()
endif()

engine_apply_hot_flags(engine_core_renderer)

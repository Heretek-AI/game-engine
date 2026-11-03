# native/ui — game UI (P5-04: RmlUi 6.3 + FreeType CPU HUD).
#
# engine_core_ui: OBJECT lib with two third-party deps, both pinned by
# tarball URL + SHA256 (dependency rule §3.4):
#   * FreeType VER-2-14-3 — static, all optional backends OFF (same flags
#     as the P5-00 spike, §4.13.2). Produces libfreetype.a.
#   * RmlUi 6.3 — RMLUI_BACKEND=native (core only, no platform backend),
#     samples/tests OFF. The engine never uses RmlUi's own VK renderer
#     (verdict §4.13.2: it owns instance/device/swapchain); pixels come
#     from the CPU rasteriser in src/game_ui.cpp.
#
# FreeType include layout: RmlUi's FreeTypeInterface.cpp includes
# <ft2build.h> + <freetype/...> (needs the `include/` dir) while the
# engine code includes <ft2build.h> + FT_FREETYPE_H (needs the dir that
# contains ft2build.h). Both come from the same fetch dir. The RmlUi
# core target needs the freetype lib + both include dirs PUBLIC.
set(ENGINE_UI_FREETYPE_URL
    "https://github.com/freetype/freetype/archive/refs/tags/VER-2-14-3.tar.gz")
set(ENGINE_UI_FREETYPE_SHA256
    dc49de6b01a266eef4876a4dd34d9842c475d3e28ff2eff63bd2fb760ab56261)
set(ENGINE_UI_RMLUI_URL
    "https://github.com/mikke89/RmlUi/archive/refs/tags/6.3.tar.gz")
set(ENGINE_UI_RMLUI_SHA256
    d977298bb6147610e5984d5db85ddf284020d655a8713913f6982074f1dbdede)

include(FetchContent)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
FetchContent_Declare(engine_freetype
    URL "${ENGINE_UI_FREETYPE_URL}"
    URL_HASH SHA256=${ENGINE_UI_FREETYPE_SHA256})
FetchContent_MakeAvailable(engine_freetype)

if(NOT TARGET Freetype::Freetype AND TARGET freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()

set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(RMLUI_TESTS OFF CACHE BOOL "" FORCE)
set(RMLUI_BACKEND "native" CACHE STRING "" FORCE)
set(RMLUI_LUA_BINDINGS OFF CACHE BOOL "" FORCE)
set(RMLUI_SVG_PLUGIN OFF CACHE BOOL "" FORCE)
set(RMLUI_LOTTIE_PLUGIN OFF CACHE BOOL "" FORCE)
set(RMLUI_HARFBUZZ_SAMPLE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(engine_rmlui
    URL "${ENGINE_UI_RMLUI_URL}"
    URL_HASH SHA256=${ENGINE_UI_RMLUI_SHA256})
FetchContent_MakeAvailable(engine_rmlui)

# LatoLatin face: the golden's deterministic font. Copied from the RmlUi
# tree into the build staging dir (tests read ENGINE_UI_FONT_DIR).
set(ENGINE_UI_FONT_SRC
    "${engine_rmlui_SOURCE_DIR}/Samples/assets/LatoLatin-Regular.ttf")
set(ENGINE_UI_FONT_DIR "${CMAKE_BINARY_DIR}/ui_fonts")
file(MAKE_DIRECTORY "${ENGINE_UI_FONT_DIR}")
file(COPY "${ENGINE_UI_FONT_SRC}" DESTINATION "${ENGINE_UI_FONT_DIR}")

add_library(engine_core_ui OBJECT src/game_ui.cpp)
add_library(engine::ui ALIAS engine_core_ui)

target_compile_definitions(engine_core_ui PRIVATE ENGINE_CORE_BUILDING)

target_include_directories(engine_core_ui PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../core/include>
)

# RmlUi core include dir (SYSTEM: upstream headers trip -Wconversion /
# -Wsign-conversion under our -Werror wall; never fix third-party code).
# PUBLIC: the link closure carries the usage requirement to dependents.
target_include_directories(engine_core_ui SYSTEM PUBLIC
    "${engine_rmlui_SOURCE_DIR}/Include")

target_link_libraries(engine_core_ui PUBLIC
    rmlui_core
    Freetype::Freetype
)

# Third-party TUs trip -Werror: RmlUi core builds with its own flags
# (RMLUI_COMPILER_OPTIONS stays ON but RMLUI_WARNINGS_AS_ERRORS is OFF
# upstream, so the lib itself is warning-clean without our hot flags).
engine_apply_base_flags(engine_core_ui)

# Deterministic font dir for tests (compile definition, same pattern as
# ENGINE_GOLDEN_DIR in native/tests).
target_compile_definitions(engine_core_ui PUBLIC
    ENGINE_UI_FONT_DIR="${ENGINE_UI_FONT_DIR}")

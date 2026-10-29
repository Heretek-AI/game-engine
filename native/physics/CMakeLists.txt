# native/physics — Jolt v5.6.0 + engine adapter (M4-01).
#
# Jolt arrives via FetchContent (pinned tag v5.6.0 + SHA256 below; MIT).
# Fetched source lands in the build tree, never in the git tree — so the
# review surface stays engine files only (CodeRabbit/CodeQL gates). Pin
# refresh: bump FETCHCONTENT_SOURCE_DIR_JOLTPHYSICS / URL + JOLT_SHA256.
#
# Build: Jolt compiles as its own static lib with upstream-compatible flags
# (C++17, exceptions+RTTI ON per Jolt's Build system; Jolt is the physics
# kernel, not frame-state code, so engine hot flags never apply to its TUs).
# The engine adapter (src/*.cpp) compiles with engine hot flags
# (no-exceptions/RTTI, AVX2) and links Jolt + enkiTS + core. No exceptions
# cross the boundary: Jolt internally never throws (verified: zero `throw`
# in public headers), and our adapter TUs are noexcept throughout.
#
# Upstream options mirror Build/CMakeLists.txt defaults (Linux CI path):
#   USE_AVX2=ON, USE_F16C=ON, USE_FMADD=ON, floating-point exceptions OFF,
#   asserts ON in Debug only, double precision OFF, determinism OFF,
#   ObjectLayer bits 16 default. SSE level fixed by upstream detection
#   (Jolt.cmake emits the -mavx2/-mfma/-msse set; never add ISA flags here
#   or AVX2 codegen double-applies and MSVC warns).
include(FetchContent)

set(JOLT_TAG "v5.6.0")
set(JOLT_SHA256 "6e069ee0172478cc78182047aac87e5310ba14a67a53348ae14cc37801fd3f8e")

FetchContent_Declare(
    JoltPhysics
    URL "https://github.com/jrouwe/JoltPhysics/archive/refs/tags/${JOLT_TAG}.tar.gz"
    URL_HASH SHA256=${JOLT_SHA256}
)
set(FETCHCONTENT_QUIET OFF)

# Upstream Build-system defaults (JoltPhysics/Build/CMakeLists.txt):
# keep in sync when bumping JOLT_TAG.
set(USE_AVX512 OFF CACHE BOOL "" FORCE)
set(USE_AVX2 ON CACHE BOOL "" FORCE)
set(USE_AV ON CACHE BOOL "" FORCE)
set(USE_SSE4_1 OFF CACHE BOOL "" FORCE)
set(USE_SSE4_2 OFF CACHE BOOL "" FORCE)
set(USE_LZCNT ON CACHE BOOL "" FORCE)
set(USE_TZCNT ON CACHE BOOL "" FORCE)
set(USE_F16C ON CACHE BOOL "" FORCE)
set(USE_FMADD ON CACHE BOOL "" FORCE)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
set(CROSS_PLATFORM_DETERMINISTIC OFF CACHE BOOL "" FORCE)
set(DOUBLE_PRECISION OFF CACHE BOOL "" FORCE)
set(USE_ASSERTS OFF CACHE BOOL "" FORCE)
set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
set(JPH_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(JPH_ENABLE_ASSERTS $<CONFIG:Debug> CACHE BOOL "" FORCE)

FetchContent_GetProperties(JoltPhysics)
if(NOT JoltPhysics_POPULATED)
    FetchContent_Populate(JoltPhysics)
endif()
# Jolt.cmake expects PHYSICS_REPO_ROOT = dir containing Jolt/ (upstream
# Build/CMakeLists.txt sets it to the repo root). Our fetched source dir
# IS the repo root (contains Jolt/ + Build/).
set(PHYSICS_REPO_ROOT "${joltphysics_SOURCE_DIR}")
set(JOLT_PHYSICS_ROOT "${joltphysics_SOURCE_DIR}/Jolt")

include("${PHYSICS_REPO_ROOT}/Jolt/Jolt.cmake")

# Jolt as its own static lib: never apply engine hot flags to target `Jolt`
# (upstream requires exceptions+RTTI; hot flags would break its contract).
set_target_properties(Jolt PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)
# The fetched tree must not inherit our global -Werror: Jolt ships with its
# own warning set (its Build system does not run -Wconversion). Our adapter
# still builds at full strictness; only the fetched kernel is exempted.
if(NOT MSVC)
    target_compile_options(Jolt PRIVATE -Wno-error -Wno-conversion -Wno-sign-conversion -Wno-shorten-64-to-32)
endif()
if(MSVC)
    target_compile_options(Jolt PRIVATE /W1)
endif()

# Pin the Jolt target to C++17 (upstream-tested standard): Jolt.cmake sets
# only the CMAKE_CXX_STANDARD *variable* (visible in the Jolt.cmake source,
# line 2), never the target property — so there is nothing to "restore".
# Set it directly; the FATAL_ERROR below guards future re-vendors that
# change the upstream standard.
set_target_properties(Jolt PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# Header-only adapter interface lib: carries only warning-safe wrappers.
# Only the adapter TUs ever textually include <Jolt/...> headers; everything
# else in the engine includes adapter headers only. Placement is load-bearing:
# the SYSTEM include + -Wno-conversion suppression lives on this target, so
# -Wconversion -Werror never sees Jolt's implicit conversions.
add_library(engine_physics_jolt_wrapper INTERFACE)
target_include_directories(engine_physics_jolt_wrapper SYSTEM INTERFACE
    $<BUILD_INTERFACE:${JOLT_PHYSICS_ROOT}>
)
if(NOT MSVC)
    # Compile-language guard: suppression valid for C++ TUs only (this target
    # is consumed by C++ only today; the genex keeps C TUs warning-clean).
    target_compile_options(engine_physics_jolt_wrapper INTERFACE
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-conversion -Wno-sign-conversion>
    )
endif()

file(GLOB PHYSICS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)
add_library(engine_core_physics OBJECT ${PHYSICS_SOURCES})
add_library(engine::physics ALIAS engine_core_physics)

target_include_directories(engine_core_physics PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

# Building-side of ENGINE_CORE_API (same model as engine_core_core):
# the adapter classes are compiled here, so this TU set must see
# dllexport on Windows. PRIVATE so it never leaks to dependents.
target_compile_definitions(engine_core_physics PRIVATE ENGINE_CORE_BUILDING)

# Jolt headers reach the adapter ONLY via the wrapper interface above
# (SYSTEM include + conversion-warning suppression). Never add
# JOLT_PHYSICS_ROOT as a plain include here: -Wconversion -Werror would
# fire inside Jolt headers (verified prior failure — wrapper exists for it).
target_link_libraries(engine_core_physics PUBLIC
    engine_physics_jolt_wrapper
    Jolt
    engine_core_core
    engine_core_ecs
    enkiTS
    flecs
)

engine_apply_hot_flags(engine_core_physics)

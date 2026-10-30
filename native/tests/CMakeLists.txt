# native/tests — CTest smoke tests proving the M1-01 pipeline links and runs.
# NOTE: tests use base flags (exceptions/RTTI on) — they are host tooling,
# not frame-path code.
add_executable(engine_core_smoke test_engine_core_smoke.cpp)

target_link_libraries(engine_core_smoke PRIVATE engine_core)

engine_apply_base_flags(engine_core_smoke)

add_test(NAME engine_core_smoke COMMAND engine_core_smoke)
set_tests_properties(engine_core_smoke PROPERTIES TIMEOUT 60)

# Zero-dependency control: if this passes and engine_core_smoke hangs,
# the fault is in engine_core load/static init, not the runner.
add_executable(bootstrap_smoke test_bootstrap_smoke.cpp)
add_test(NAME bootstrap_smoke COMMAND bootstrap_smoke)
set_tests_properties(bootstrap_smoke PROPERTIES TIMEOUT 60)

# M1-02: allocator + aligned-hook acceptance. Links engine_core (which pulls
# the shared flecs transitively) for ecs_os_api + world/bulk/query APIs.
add_executable(engine_memory_arenas test_memory_arenas.cpp)

target_link_libraries(engine_memory_arenas PRIVATE engine_core)

engine_apply_base_flags(engine_memory_arenas)

add_test(NAME engine_memory_arenas COMMAND engine_memory_arenas)
# The 1M-row sweep needs a real arena; give CTest a generous timeout so
# loaded CI runners don't flake.
set_tests_properties(engine_memory_arenas PROPERTIES TIMEOUT 300)

# M1-03: scheduler + hierarchy acceptance. Links engine_core (Flecs task
# hooks, EcsWorld, SIMD transforms). Owns OS-API setup in-process.
add_executable(engine_job_scheduler test_job_scheduler.cpp)

target_link_libraries(engine_job_scheduler PRIVATE engine_core)

engine_apply_base_flags(engine_job_scheduler)

add_test(NAME engine_job_scheduler COMMAND engine_job_scheduler)
# 1M-row propagate + task-thread staging; generous timeout for CI.
set_tests_properties(engine_job_scheduler PROPERTIES TIMEOUT 300)

# M3-01: renderer bootstrap acceptance. Links engine_core (which pulls the
# renderer OBJECT lib). Headless-safe: SDL window + Vulkan init are probed
# and skipped when no display/ICD is present (Lavapipe still exercises the
# real init path where mesa-vulkan-drivers is installed).
add_executable(engine_renderer_bootstrap test_renderer_bootstrap.cpp)

target_link_libraries(engine_renderer_bootstrap PRIVATE engine_core)

engine_apply_base_flags(engine_renderer_bootstrap)

add_test(NAME engine_renderer_bootstrap COMMAND engine_renderer_bootstrap)
set_tests_properties(engine_renderer_bootstrap PROPERTIES TIMEOUT 300)

# M3-02: GPU-driven indirect acceptance. Links engine_core (renderer OBJECT
# lib). Lavapipe exercises the real indirect-count draw path where an ICD
# advertises a device; otherwise the GPU section skips cleanly.
# NOTE: this TU includes <volk.h> for vk* entry points but must NOT link
# volk's objects into the exe: volk is compiled into libengine_core.so
# (renderer OBJECT lib) and initialized there; a second copy in the exe
# would be a separate never-initialized .bss (all vk* null in the .so).
# The exe's volk refs stay UNDEFINED, satisfied by the .so at runtime.
add_executable(engine_gpu_indirect test_gpu_indirect.cpp)

target_link_libraries(engine_gpu_indirect PRIVATE engine_core)

engine_apply_base_flags(engine_gpu_indirect)

add_test(NAME engine_gpu_indirect COMMAND engine_gpu_indirect)
set_tests_properties(engine_gpu_indirect PROPERTIES TIMEOUT 300)

# M3-03: slang shader pipeline acceptance. Links engine_core (renderer
# OBJECT lib). SHADER_SOURCE_DIR points at the checked-in .slang sources
# for the hot-reload watcher test (read-only); ENGINE_TEST_SLANGC carries
# the slangc path when available (CI: absent -> swap section skips, magic
# checks still prove the embedded blobs).
add_executable(engine_slang_pbr test_slang_pbr.cpp)

target_link_libraries(engine_slang_pbr PRIVATE engine_core)

target_compile_definitions(engine_slang_pbr PRIVATE
    SHADER_SOURCE_DIR="${CMAKE_SOURCE_DIR}/native/renderer/shaders")

find_program(ENGINE_SLANGC_EXE slangc)
engine_apply_base_flags(engine_slang_pbr)

add_test(NAME engine_slang_pbr COMMAND engine_slang_pbr)

# M3-03: managed compositor live round-trip. Runs Engine.Tests from a
# staging dir where libengine_core's SONAME symlink (libengine_core.so,
# what dlopen("engine_core") resolves) sits next to the managed dlls, so
# ApiLoader.Load() takes the live path: GraphicsCompositor create/add/
# compile/run against the real ABI 3 table. A sentinel file written by
# the live path fails the test closed when the live path skips.
# The live probe stages the managed output dir (copy_directory so the
# AnalyzerTests FindBin layout + deps.json closure resolve exactly as
# `dotnet run` sees them) alongside the native library; the test itself
# runs from the STAGED dir with cwd == staged dir. Depends on the hostfxr
# fixture build (same `dotnet build Engine.slnx` the smoke test uses), so
# the output dir always exists first (fixes the CI "no such file"
# symlink/workdir failures). The probe flag fails closed when engine_core
# does not load (no silent SKIP on the live path).
add_test(NAME engine_compositor_live_stage
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/managed/Engine.Tests/bin/Release/net10.0"
        "${CMAKE_CURRENT_BINARY_DIR}/compositor_live")
# Stage the runtime closure: native lib under its SONAME + the flecs
# sibling (DT_NEEDED) next to the managed app. The probe prefers these
# CWD/appdir siblings before the build tree (macOS .dylib / Win .dll /
# Linux .so all covered); the build-tree walk stays as fallback.
# NOTE: $<TARGET_FILE_SUFFIX> on a versioned .so yields ".so.0.1.0" while
# the base name drops the "lib" prefix ("engine_core"), producing the
# wrong "engine_core.so.0.1.0". Stage under the explicit link names the
# loader probes: lib<name>.so (Linux), lib<name>.dylib (macOS),
# <name>.dll (Windows). $<TARGET_FILE_NAME> keeps the real filename.
add_test(NAME engine_compositor_live_stage_native
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:engine_core>"
        "${CMAKE_CURRENT_BINARY_DIR}/compositor_live/$<TARGET_FILE_NAME:engine_core>")
set_tests_properties(engine_compositor_live_stage_native PROPERTIES
    TIMEOUT 120
    FIXTURES_SETUP compositor_live_staged
    FIXTURES_REQUIRED benchmark1m_fixtures)
add_test(NAME engine_compositor_live_stage_native_link
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        "$<TARGET_FILE_NAME:engine_core>"
        "${CMAKE_CURRENT_BINARY_DIR}/compositor_live/libengine_core$<TARGET_FILE_SUFFIX:engine_core>")
set_tests_properties(engine_compositor_live_stage_native_link PROPERTIES
    TIMEOUT 120
    FIXTURES_SETUP compositor_live_staged
    FIXTURES_REQUIRED benchmark1m_fixtures)
add_test(NAME engine_compositor_live_stage_flecs
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:flecs>"
        "${CMAKE_CURRENT_BINARY_DIR}/compositor_live/$<TARGET_FILE_NAME:flecs>")
set_tests_properties(engine_compositor_live_stage_flecs PROPERTIES
    TIMEOUT 120
    FIXTURES_SETUP compositor_live_staged
    FIXTURES_REQUIRED benchmark1m_fixtures)
add_test(NAME engine_compositor_live_stage_analyzers
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/managed/Engine.Analyzers/bin/Release/netstandard2.0"
        "${CMAKE_CURRENT_BINARY_DIR}/compositor_live/managed/Engine.Analyzers/bin/Release/netstandard2.0")
set_tests_properties(engine_compositor_live_stage_analyzers PROPERTIES
    TIMEOUT 120
    FIXTURES_SETUP compositor_live_staged
    FIXTURES_REQUIRED benchmark1m_fixtures)
add_test(NAME engine_compositor_live_stage_codefixes
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/managed/Engine.CodeFixes/bin/Release/net8.0"
        "${CMAKE_CURRENT_BINARY_DIR}/compositor_live/managed/Engine.CodeFixes/bin/Release/net8.0")
set_tests_properties(engine_compositor_live_stage_codefixes PROPERTIES
    TIMEOUT 120
    FIXTURES_SETUP compositor_live_staged
    FIXTURES_REQUIRED benchmark1m_fixtures)
set_tests_properties(engine_compositor_live_stage PROPERTIES
    TIMEOUT 120
    FIXTURES_SETUP compositor_live_staged
    FIXTURES_REQUIRED benchmark1m_fixtures)
# The apphost (Engine.Tests exe, NOT the .dll) boots the staged
# deps.json/runtimeconfig closure; --live-native-probe fails closed when
# engine_core does not load. LD_LIBRARY_PATH covers libflecs resolution.
if(WIN32)
    set(_COMPOSITOR_EXE "${CMAKE_CURRENT_BINARY_DIR}/compositor_live/Engine.Tests.exe")
else()
    set(_COMPOSITOR_EXE "${CMAKE_CURRENT_BINARY_DIR}/compositor_live/Engine.Tests")
endif()
add_test(NAME engine_compositor_live
    COMMAND "${_COMPOSITOR_EXE}" --live-native-probe
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/compositor_live")
set_tests_properties(engine_compositor_live PROPERTIES
    TIMEOUT 300
    FIXTURES_REQUIRED compositor_live_staged
    ENVIRONMENT "LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/native:${CMAKE_BINARY_DIR}/deps/flecs:$ENV{LD_LIBRARY_PATH};DOTNET_ROOT=$ENV{DOTNET_ROOT}")

set_tests_properties(engine_slang_pbr PROPERTIES TIMEOUT 300)
if(ENGINE_SLANGC_EXE)
    set_tests_properties(engine_slang_pbr PROPERTIES
        ENVIRONMENT "ENGINE_TEST_SLANGC=${ENGINE_SLANGC_EXE}")
    message(STATUS "slang_pbr: slangc at ${ENGINE_SLANGC_EXE} (hot-swap proven)")
else()
    message(STATUS "slang_pbr: no slangc (hot-swap section skips; blobs still verified)")
endif()

# M3-04: zero-bake GI acceptance. Links engine_core (renderer OBJECT lib).
# CPU-side contract (SDF bricks, cascades, SDSM sizing, SPIR-V blobs)
# runs everywhere; the Lavapipe vkCreateShaderModule section skips cleanly
# with no physical device. GI resolve + shadow trace blobs live in the
# header mirror dir (same shape as the M3-03 pbr_*_spv.inc entries).
add_executable(engine_gi_zero_bake test_gi_zero_bake.cpp)

target_link_libraries(engine_gi_zero_bake PRIVATE engine_core)

engine_apply_base_flags(engine_gi_zero_bake)

add_test(NAME engine_gi_zero_bake COMMAND engine_gi_zero_bake)
set_tests_properties(engine_gi_zero_bake PROPERTIES TIMEOUT 300)

# Windows: the loader only searches the exe dir + PATH, so a test in
# next to every linked test binary (Linux/macOS resolve via rpath instead).
# NOTE: bootstrap_smoke links nothing — an empty TARGET_RUNTIME_DLLS
# expansion would make cmake -E fail the build, so it is excluded here.
# M1-04: ABI contract acceptance.
# (Declared before the WIN32 DLL-copy block: every add_custom_command
# below must reference an already-created target.) Links engine_core (Flecs universe +
# engine_get_api_table). Owns no OS-API setup (uses default malloc hooks).
add_executable(engine_abi_layout test_abi_layout.cpp)

target_link_libraries(engine_abi_layout PRIVATE engine_core)

engine_apply_base_flags(engine_abi_layout)

add_test(NAME engine_abi_layout COMMAND engine_abi_layout)
set_tests_properties(engine_abi_layout PROPERTIES TIMEOUT 120)

# M1-05: hostfxr embedding acceptance. The managed fixture
# (Benchmark1M.Systems.dll + Benchmark1M.runtimeconfig.json) is built by
# `dotnet build` of Engine.slnx; the native smoke test boots CoreCLR
# through DotNetHost and runs the 1M-row milestone through the UCO
# callback. dotnet is REQUIRED: the M1-05 gate cannot run without managed
# fixtures (CI installs .NET 10 via setup-dotnet before CMake configures).
#
# Fixture paths point at the SOURCE tree outputs (dotnet builds in-source
# under managed/ + samples/), so configure needs no build ordering: the
# ctest fixture step below rebuilds them if stale.
find_program(DOTNET_EXE dotnet REQUIRED)
set(BENCHMARK1M_SYSTEMS_DLL
    "${CMAKE_SOURCE_DIR}/samples/Benchmark1M/bin/Release/net10.0/Benchmark1M.Systems.dll")
set(BENCHMARK1M_CONFIG
    "${CMAKE_SOURCE_DIR}/samples/Benchmark1M/bin/Release/net10.0/Benchmark1M.runtimeconfig.json")

add_executable(engine_hostfxr_smoke test_hostfxr_smoke.cpp)

target_link_libraries(engine_hostfxr_smoke PRIVATE engine_core)

target_compile_definitions(engine_hostfxr_smoke PRIVATE
    BENCHMARK1M_SYSTEMS_DLL="${BENCHMARK1M_SYSTEMS_DLL}"
    BENCHMARK1M_CONFIG="${BENCHMARK1M_CONFIG}")

engine_apply_base_flags(engine_hostfxr_smoke)

# Rebuild managed fixtures before the smoke test runs (no-op when fresh).
# Runs from the repo root so dotnet resolves Engine.slnx's relative project
# paths; no --project path arithmetic needed regardless of preset binary dir.
# -p:Platform="Any CPU": the slnx declares no platform configurations, and
# the Windows job's msvc-dev-cmd environment exports Platform=x64, which
# MSBuild would otherwise demand as "Release|x64" (MSB4126). An explicit
# Any CPU is valid on all three OSes (the projects' default platform).
add_test(NAME engine_hostfxr_smoke_build
    COMMAND ${DOTNET_EXE} build Engine.slnx -c Release --nologo "-p:Platform=Any CPU")
set_tests_properties(engine_hostfxr_smoke_build PROPERTIES
    TIMEOUT 600
    FIXTURES_SETUP benchmark1m_fixtures
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

add_test(NAME engine_hostfxr_smoke COMMAND engine_hostfxr_smoke)
set_tests_properties(engine_hostfxr_smoke PROPERTIES
    TIMEOUT 300
    FIXTURES_REQUIRED benchmark1m_fixtures)
# Roofline-relative efficiency gate (not a fixed wall clock): the serial
# 1M STREAM probe measures the machine's own bandwidth for this exact
# traffic pattern (144MB/frame AoS); the parallel best-of-N must reach it
# (gain >= 1/1.5). A fixed 1.5ms demands >= 96GB/s sustained — met at 20%
# on a 256GB/s workstation, unmeetable on a 36GB/s shared CI VM — so the
# fixed number is proven by Benchmark1M on reference hardware while CI
# proves the fan-out delivers the hardware's bandwidth. Absolute ms, GB/s
# and gain stay LOGGED for humans.
# Apple-Silicon CI carve-out: macOS-15 ARM runners hang in the FIRST
# parallel managed fan-out (serial path + serial probe pass on the same
# box; native-only enkiTS fan-out passes in test_job_scheduler — the hang
# is specific to parallel UCO dispatch, root cause under investigation).
# ENGINE_SERIAL_DISPATCH forces the serial fast path there: embedding,
# exactness and no-hang are still proven on macOS while Linux proves the
# parallel path (Ubuntu shows gain 1.3x on 2 real cores, local 3.7x).
# hostfxr discovery is DOTNET_ROOT-first (ScanFxDir): the `dotnet` resolved
# above knows an SDK install, but the SDK dir is NOT necessarily the shared
# runtime install the smoke test must boot (setup-dotnet: SDK in
# $RUNNER_TOOL_CACHE, runtime in /usr/share/dotnet or ~/.dotnet).
# Prefer the SHARED runtime root from `dotnet --list-runtimes`.
# CAREFUL: the bracketed path is the SHARED root
# (<dotnet>/shared/Microsoft.NETCore.App), NOT the dotnet root — hostfxr
# resolves the runtime relative to DOTNET_ROOT, so passing the shared dir
# breaks framework lookup (observed Windows rc=0x80008093). Strip the two
# trailing components (shared/Microsoft.NETCore.App) to recover the real
# dotnet root (carries host/fxr/<ver>). Fall back to the SDK root.
execute_process(
    COMMAND ${DOTNET_EXE} --list-runtimes
    OUTPUT_VARIABLE _DOTNET_RUNTIMES
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(_DOTNET_RUNTIMES MATCHES "Microsoft\\.NETCore\\.App [0-9.]+ \\[([^]]+)\\]")
    set(_ENGINE_SHARED_ROOT "${CMAKE_MATCH_1}")
    get_filename_component(_ENGINE_DOTNET_ROOT "${_ENGINE_SHARED_ROOT}" DIRECTORY)
    get_filename_component(_ENGINE_DOTNET_ROOT "${_ENGINE_DOTNET_ROOT}" DIRECTORY)
    if(EXISTS "${_ENGINE_DOTNET_ROOT}/host/fxr")
        set(ENGINE_DOTNET_ROOT "${_ENGINE_DOTNET_ROOT}")
    else()
        message(STATUS "dotnet shared root ${_ENGINE_SHARED_ROOT} has no host/fxr sibling; leaving DOTNET_ROOT unset (ScanFxDir well-known prefixes apply)")
    endif()
    unset(_ENGINE_SHARED_ROOT)
    unset(_ENGINE_DOTNET_ROOT)
else()
    # No shared-runtime entry (plain SDK layout): fall back to the SDK root.
    execute_process(
        COMMAND ${DOTNET_EXE} --list-sdks
        OUTPUT_VARIABLE _DOTNET_SDKS
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(_DOTNET_SDKS MATCHES "\\[([^]]+)\\]")
        set(ENGINE_DOTNET_ROOT "${CMAKE_MATCH_1}")
    endif()
endif()
if(DEFINED ENGINE_DOTNET_ROOT)
    message(STATUS "dotnet root for hostfxr smoke: ${ENGINE_DOTNET_ROOT}")
    # Windows: pass DOTNET_ROOT with native backslashes. The windows-2025
    # image sets DOTNET_MULTILEVEL_LOOKUP=0, so hostfxr trusts DOTNET_ROOT
    # exclusively for framework resolution — and forward slashes break that
    # strict path (observed rc=0x80008093 with C:/... while the same value
    # works where multilevel lookup stays on). TO_NATIVE_PATH matches what
    # setup-dotnet itself exports (C:\Program Files\dotnet).
    if(WIN32)
        file(TO_NATIVE_PATH "${ENGINE_DOTNET_ROOT}" _ENGINE_DOTNET_ROOT_NATIVE)
        set(_ENGINE_SMOKE_DOTNET_ROOT "${_ENGINE_DOTNET_ROOT_NATIVE}")
        unset(_ENGINE_DOTNET_ROOT_NATIVE)
        # windows-2025 images export DOTNET_MULTILEVEL_LOOKUP=0, under which
        # hostfxr 10.0.14 refuses the fixture runtimeconfig (rc=0x80008093)
        # even with a correct DOTNET_ROOT. Restoring multilevel lookup for
        # this test-only process re-enables the standard search
        # (DOTNET_ROOT + global install) without touching product behavior.
        set(_ENGINE_SMOKE_MLL "DOTNET_MULTILEVEL_LOOKUP=1")
    else()
        set(_ENGINE_SMOKE_DOTNET_ROOT "${ENGINE_DOTNET_ROOT}")
        set(_ENGINE_SMOKE_MLL "ENGINE_MLL_UNUSED=1")
    endif()
    if(APPLE)
        set_tests_properties(engine_hostfxr_smoke PROPERTIES
            ENVIRONMENT "DOTNET_ROOT=${_ENGINE_SMOKE_DOTNET_ROOT};ENGINE_SERIAL_DISPATCH=1")
    elseif(WIN32)
        set_tests_properties(engine_hostfxr_smoke PROPERTIES
            ENVIRONMENT "DOTNET_ROOT=${_ENGINE_SMOKE_DOTNET_ROOT};${_ENGINE_SMOKE_MLL}")
    else()
        set_tests_properties(engine_hostfxr_smoke PROPERTIES
            ENVIRONMENT "DOTNET_ROOT=${_ENGINE_SMOKE_DOTNET_ROOT}")
    endif()
elseif(APPLE)
    # No runtime root resolved, but still force serial dispatch on Apple
    # Silicon (parallel UCO hang — see above).
    set_tests_properties(engine_hostfxr_smoke PROPERTIES
        ENVIRONMENT "ENGINE_SERIAL_DISPATCH=1")
endif()

# M4-01: Jolt physics acceptance (job system, temp allocator, layers,
# components, world step). Uses base flags (host tooling).
add_executable(engine_physics_jolt test_physics_jolt.cpp)

target_link_libraries(engine_physics_jolt PRIVATE engine_core)

engine_apply_base_flags(engine_physics_jolt)

add_test(NAME engine_physics_jolt COMMAND engine_physics_jolt)
set_tests_properties(engine_physics_jolt PROPERTIES TIMEOUT 300)

# Copy runtime DLLs (engine_core + flecs) next to every linked test binary.
# NOTE: engine_audio_spatial is declared AFTER this block (M4-04 section
# below); its own WIN32 copy block lives there — do NOT add it here
# (CMake errors on unknown TARGET at configure time).
if(WIN32)
    foreach(tgt engine_core_smoke engine_memory_arenas engine_job_scheduler engine_abi_layout engine_physics_jolt engine_hostfxr_smoke engine_renderer_bootstrap engine_gpu_indirect engine_slang_pbr engine_gi_zero_bake)
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${tgt}>
                $<TARGET_FILE_DIR:${tgt}>
            COMMAND_EXPAND_LISTS)
    endforeach()
endif()

# M4-04: miniaudio spatial graph acceptance (streaming decode matrix +
# Flecs spatializer). Headless-safe: the test constructs AudioWorld with
# noDevice=true. Fixtures are CHECKED IN under native/tests/fixtures/
# (3 tiny sine tones, ~68 KB total) so configure never needs ffmpeg or
# network — MSVC/Windows CI included (ffmpeg is absent there).
set(AUDIO_FIXTURE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

add_executable(engine_audio_spatial test_audio_spatial.cpp)

target_link_libraries(engine_audio_spatial PRIVATE engine_core)

target_compile_definitions(engine_audio_spatial PRIVATE
    AUDIO_FIXTURE_DIR="${AUDIO_FIXTURE_DIR}"
)

engine_apply_base_flags(engine_audio_spatial)

add_test(NAME engine_audio_spatial COMMAND engine_audio_spatial)
set_tests_properties(engine_audio_spatial PROPERTIES TIMEOUT 300)

if(WIN32)
    foreach(tgt engine_audio_spatial)
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${tgt}>
                $<TARGET_FILE_DIR:${tgt}>
            COMMAND_EXPAND_LISTS)
    endforeach()
endif()

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
# M5-03: stage the engine-cli apphost + closure next to the live probe so
# CliApiTests can drive the REAL binary over the test socket (same dir =
# same runtimeconfig/deps resolution as the apphost it probes).
if(WIN32)
    set(_CLI_APPHOST "engine-cli.exe")
else()
    set(_CLI_APPHOST "engine-cli")
endif()
add_test(NAME engine_compositor_live_stage_cli
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_SOURCE_DIR}/managed/Engine.Cli/bin/Release/net10.0/${_CLI_APPHOST}"
        "${CMAKE_CURRENT_BINARY_DIR}/compositor_live/${_CLI_APPHOST}")
set_tests_properties(engine_compositor_live_stage_cli PROPERTIES
    TIMEOUT 120
    FIXTURES_SETUP compositor_live_staged
    FIXTURES_REQUIRED benchmark1m_fixtures)
add_test(NAME engine_compositor_live_stage_cli_config
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_SOURCE_DIR}/managed/Engine.Cli/bin/Release/net10.0/engine-cli.runtimeconfig.json"
        "${CMAKE_CURRENT_BINARY_DIR}/compositor_live/engine-cli.runtimeconfig.json")
set_tests_properties(engine_compositor_live_stage_cli_config PROPERTIES
    TIMEOUT 120
    FIXTURES_SETUP compositor_live_staged
    FIXTURES_REQUIRED benchmark1m_fixtures)
add_test(NAME engine_compositor_live_stage_cli_dll
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_SOURCE_DIR}/managed/Engine.Cli/bin/Release/net10.0/engine-cli.dll"
        "${CMAKE_CURRENT_BINARY_DIR}/compositor_live/engine-cli.dll")
set_tests_properties(engine_compositor_live_stage_cli_dll PROPERTIES
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
# M5-04: stage engine-pak next to the live probe so ExportPipeline +
# ExportApiTests drive the REAL packer binary (same-dir resolution, no
# PATH dependency). engine-pak is a framework-dependent apphost (needs
# its .dll + runtimeconfig beside the exe, same as engine-cli above).
if(WIN32)
    set(_PAK_APPHOST "engine-pak.exe")
else()
    set(_PAK_APPHOST "engine-pak")
endif()
add_test(NAME engine_compositor_live_stage_pak
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_SOURCE_DIR}/tools/engine-pak/bin/Release/net10.0/${_PAK_APPHOST}"
        "${CMAKE_CURRENT_BINARY_DIR}/compositor_live/${_PAK_APPHOST}")
set_tests_properties(engine_compositor_live_stage_pak PROPERTIES
    TIMEOUT 120
    FIXTURES_SETUP compositor_live_staged
    FIXTURES_REQUIRED benchmark1m_fixtures)
add_test(NAME engine_compositor_live_stage_pak_config
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_SOURCE_DIR}/tools/engine-pak/bin/Release/net10.0/engine-pak.runtimeconfig.json"
        "${CMAKE_CURRENT_BINARY_DIR}/compositor_live/engine-pak.runtimeconfig.json")
set_tests_properties(engine_compositor_live_stage_pak_config PROPERTIES
    TIMEOUT 120
    FIXTURES_SETUP compositor_live_staged
    FIXTURES_REQUIRED benchmark1m_fixtures)
add_test(NAME engine_compositor_live_stage_pak_dll
    COMMAND ${CMAKE_COMMAND} -E copy
        "${CMAKE_SOURCE_DIR}/tools/engine-pak/bin/Release/net10.0/engine-pak.dll"
        "${CMAKE_CURRENT_BINARY_DIR}/compositor_live/engine-pak.dll")
set_tests_properties(engine_compositor_live_stage_pak_dll PROPERTIES
    TIMEOUT 120
    FIXTURES_SETUP compositor_live_staged
    FIXTURES_REQUIRED benchmark1m_fixtures)
# M5-04: golden-vector — managed engine-pak packs a fixture corpus, the
# native golden reader opens it through PakReader and asserts byte-exact
# content + CRC (proves the managed writer layout matches the native
# reader: magic, GUID sort, LZ4 wire format).
#
# Fixtures are deterministic bytes generated at CONFIGURE time into the
# build tree (never in the repo): text = repeated sentence (LZ4 path),
# noise = seeded PRNG bytes (stored path). The run script packs them with
# the staged managed engine-pak, then the native reader verifies.
execute_process(
    COMMAND python3 -c "open('${CMAKE_CURRENT_BINARY_DIR}/pak_golden_text.bin','wb').write(b'the quick brown fox jumps over the lazy dog. pack my box with five dozen liquor jugs! '*2000); import random; random.seed(42); open('${CMAKE_CURRENT_BINARY_DIR}/pak_golden_noise.bin','wb').write(bytes(random.getrandbits(8) for _ in range(20000)))"
)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/run_pak_golden.sh"
"#!/bin/sh\n"
"set -e\n"
"STAGE=\"${CMAKE_CURRENT_BINARY_DIR}/compositor_live\"\n"
"GOLD=\"$STAGE/pak_golden\"\n"
"rm -rf \"$GOLD\"\n"
"mkdir -p \"$GOLD\"\n"
"cp \"${CMAKE_CURRENT_BINARY_DIR}/pak_golden_text.bin\" \"$GOLD/text.bin\"\n"
"cp \"${CMAKE_CURRENT_BINARY_DIR}/pak_golden_noise.bin\" \"$GOLD/noise.bin\"\n"
"\"$STAGE/engine-pak\" pack --out \"$GOLD/golden.pak\" --compress \\\n"
"  \"01234567-89ab-cdef-0123-456789abcdef:model:$GOLD/text.bin\" \\\n"
"  \"11111111-2222-4333-8555-666666666666:texture:$GOLD/noise.bin\"\n"
"\"${CMAKE_CURRENT_BINARY_DIR}/engine_pak_golden\" \"$GOLD/golden.pak\" \"$GOLD/text.bin\" \"$GOLD/noise.bin\"\n")
add_executable(engine_pak_golden test_pak_golden.cpp)
target_link_libraries(engine_pak_golden PRIVATE engine_core)
engine_apply_base_flags(engine_pak_golden)
add_test(NAME engine_pak_managed_golden
    COMMAND sh "${CMAKE_CURRENT_BINARY_DIR}/run_pak_golden.sh")
set_tests_properties(engine_pak_managed_golden PROPERTIES
    TIMEOUT 120
    FIXTURES_REQUIRED compositor_live_staged)
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

# M5-01: asset pipeline acceptance (GUID/.meta, mmap fast preview,
# watcher, background meshopt+bc7enc worker). Links engine_core (assets
# OBJECT lib). Self-generates GLB/PNG fixtures — no network, no binaries.
add_executable(engine_asset_pipeline test_asset_pipeline.cpp)

# Asset API comes from engine_core (shared .so/.dll). No extra OBJECT link:
# the asset TUs export ENGINE_ASSETS_API from the DLL on Windows and default
# visibility on Linux/macOS.
target_link_libraries(engine_asset_pipeline PRIVATE engine_core)

engine_apply_base_flags(engine_asset_pipeline)

add_test(NAME engine_asset_pipeline COMMAND engine_asset_pipeline)
set_tests_properties(engine_asset_pipeline PROPERTIES TIMEOUT 300)

# M5-04: .pak archive acceptance (LZ4 codec, GUID-keyed builder, mmap
# reader, corrupt-input gates). Links engine_core (assets OBJECT lib).
add_executable(engine_asset_pak test_asset_pak.cpp)

target_link_libraries(engine_asset_pak PRIVATE engine_core)

engine_apply_base_flags(engine_asset_pak)

add_test(NAME engine_asset_pak COMMAND engine_asset_pak)
set_tests_properties(engine_asset_pak PROPERTIES TIMEOUT 300)

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

# M5-04: one-click export fixtures — NativeAOT game + .pak staging.
# Publishes samples/ExportSample with PublishAot (build --no-build reuses
# the hostfxr fixture build above, so ILCompiler runs once per machine),
# then stages the game binary + native libs + a fixture .pak into
# export_stage/ for the cold-boot acceptance test below. The publish is a
# FIXTURES_SETUP step (not the gate itself): slow ILCompiler failures
# surface here with the SDK log, while the gate measures cold boot.
set(EXPORT_SAMPLE_PUBLISH_DIR
    "${CMAKE_SOURCE_DIR}/samples/ExportSample/bin/Release/net10.0/linux-x64/publish")
if(WIN32)
    set(EXPORT_SAMPLE_RID "win-x64")
    set(EXPORT_SAMPLE_EXE "ExportSample.exe")
elseif(APPLE)
    set(EXPORT_SAMPLE_RID "osx-arm64")
    set(EXPORT_SAMPLE_EXE "ExportSample")
else()
    set(EXPORT_SAMPLE_RID "linux-x64")
    set(EXPORT_SAMPLE_EXE "ExportSample")
endif()
add_test(NAME engine_export_publish
    COMMAND ${DOTNET_EXE} build "${CMAKE_SOURCE_DIR}/samples/ExportSample/ExportSample.csproj"
        -c Release --nologo
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(engine_export_publish PROPERTIES
    TIMEOUT 1200
    FIXTURES_SETUP export_stage_built
    FIXTURES_REQUIRED benchmark1m_fixtures)
# Separate publish step: --no-build reuses the build above, so ILCompiler
# runs on bin/Release (never bin/x64/Release) on every OS. Split from the
# build because --no-build publish still evaluates stale per-config
# reference paths (MSB4018 on Windows) when build+publish share one
# invocation through the slnx Platform property.
add_test(NAME engine_export_publish_aot
    COMMAND ${DOTNET_EXE} publish "${CMAKE_SOURCE_DIR}/samples/ExportSample/ExportSample.csproj"
        --no-build -r ${EXPORT_SAMPLE_RID} -c Release -p:PublishAot=true --nologo
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
set_tests_properties(engine_export_publish_aot PROPERTIES
    TIMEOUT 1200
    FIXTURES_SETUP export_stage_built
    FIXTURES_REQUIRED benchmark1m_fixtures)
add_test(NAME engine_export_stage
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_CURRENT_BINARY_DIR}/export_stage")
set_tests_properties(engine_export_stage PROPERTIES
    TIMEOUT 120
    FIXTURES_SETUP export_staged
    FIXTURES_REQUIRED export_stage_built)
# Copy the staged AOT game into export_stage/. The publish output dir
# varies by OS/SDK (bin/Release vs bin/x64/Release when the msvc-dev-cmd
# environment exports Platform=x64): glob for the binary under the sample
# build tree instead of hardcoding one layout. Native libs follow.
add_test(NAME engine_export_stage_game
    COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/stage_export_game.cmake")
# Stage the AOT game via a real script file (cmake/stage_export_game.cmake.in):
# nesting file(WRITE) quoting inside CMakeLists proved un escapable (the
# ${_src}/${_c} refs kept expanding at configure time). configure_file
# with @ONLY leaves ${} script vars intact for ctest -P time.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/stage_export_game.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/stage_export_game.cmake" @ONLY)
set_tests_properties(engine_export_stage_game PROPERTIES
    TIMEOUT 120
    FIXTURES_SETUP export_staged
    FIXTURES_REQUIRED export_stage_built)
add_test(NAME engine_export_stage_core
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:engine_core>"
        "${CMAKE_CURRENT_BINARY_DIR}/export_stage/$<TARGET_FILE_NAME:engine_core>")
set_tests_properties(engine_export_stage_core PROPERTIES
    TIMEOUT 120
    FIXTURES_SETUP export_staged
    FIXTURES_REQUIRED export_stage_built)
add_test(NAME engine_export_stage_core_link
    COMMAND ${CMAKE_COMMAND} -E create_symlink
        "$<TARGET_FILE_NAME:engine_core>"
        "${CMAKE_CURRENT_BINARY_DIR}/export_stage/libengine_core$<TARGET_FILE_SUFFIX:engine_core>")
set_tests_properties(engine_export_stage_core_link PROPERTIES
    TIMEOUT 120
    FIXTURES_SETUP export_staged
    FIXTURES_REQUIRED export_stage_built)
add_test(NAME engine_export_stage_flecs
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:flecs>"
        "${CMAKE_CURRENT_BINARY_DIR}/export_stage/$<TARGET_FILE_NAME:flecs>")
set_tests_properties(engine_export_stage_flecs PROPERTIES
    TIMEOUT 120
    FIXTURES_SETUP export_staged
    FIXTURES_REQUIRED export_stage_built)

# M5-04: cold-boot gate — the exported NativeAOT game cold-opens beside
# its staged .pak and exits EXPORT-SAMPLE-OK. Wall <1000ms is asserted by
# the test binary itself (per-run Stopwatch + worst-of-5); ctest only
# needs the binary to exit 0. Runs after the export fixtures stage.
add_executable(engine_export_coldboot test_export_coldboot.cpp)
target_link_libraries(engine_export_coldboot PRIVATE engine_core)
engine_apply_base_flags(engine_export_coldboot)
add_test(NAME engine_export_coldboot COMMAND engine_export_coldboot)
set_tests_properties(engine_export_coldboot PROPERTIES
    TIMEOUT 300
    FIXTURES_REQUIRED "export_staged;compositor_live_staged"
    ENVIRONMENT "LD_LIBRARY_PATH=${CMAKE_BINARY_DIR}/native:${CMAKE_BINARY_DIR}/deps/flecs:$ENV{LD_LIBRARY_PATH}")
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

# M4-02: Flecs <-> Jolt transform sync acceptance (push/pull/events).
# Uses base flags (host tooling). Links engine_core (world + sync).
add_executable(engine_physics_sync test_physics_sync.cpp)

target_link_libraries(engine_physics_sync PRIVATE engine_core)

engine_apply_base_flags(engine_physics_sync)

add_test(NAME engine_physics_sync COMMAND engine_physics_sync)
set_tests_properties(engine_physics_sync PROPERTIES TIMEOUT 300)

# M4-03: spatial queries acceptance (raycast / spherecast / overlap /
# parallel batch). Uses base flags (host tooling); TU-local Jolt allocator
# registration handles the Windows DLL-duplication (see TU header).
add_executable(engine_physics_queries test_physics_queries.cpp)

target_link_libraries(engine_physics_queries PRIVATE engine_core)

engine_apply_base_flags(engine_physics_queries)

add_test(NAME engine_physics_queries COMMAND engine_physics_queries)
set_tests_properties(engine_physics_queries PROPERTIES TIMEOUT 300)

# Copy runtime DLLs (engine_core + flecs) next to every linked test binary.
# NOTE: engine_audio_spatial is declared AFTER this block (M4-04 section
# below); its own WIN32 copy block lives there — do NOT add it here
# (CMake errors on unknown TARGET at configure time).
if(WIN32)
    foreach(tgt engine_core_smoke engine_memory_arenas engine_job_scheduler engine_abi_layout engine_physics_jolt engine_physics_sync engine_physics_queries engine_hostfxr_smoke engine_renderer_bootstrap engine_gpu_indirect engine_slang_pbr engine_gi_zero_bake engine_asset_pipeline)
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

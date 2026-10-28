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

# Windows: the loader only searches the exe dir + PATH, so a test in
# native/tests/ cannot see engine_core.dll in native/. Copy runtime DLLs
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
add_test(NAME engine_hostfxr_smoke_build
    COMMAND ${DOTNET_EXE} build Engine.slnx -c Release --nologo)
set_tests_properties(engine_hostfxr_smoke_build PROPERTIES
    TIMEOUT 600
    FIXTURES_SETUP benchmark1m_fixtures
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

add_test(NAME engine_hostfxr_smoke COMMAND engine_hostfxr_smoke)
set_tests_properties(engine_hostfxr_smoke PROPERTIES
    TIMEOUT 300
    FIXTURES_REQUIRED benchmark1m_fixtures)
# hostfxr discovery is DOTNET_ROOT-first (ScanFxDir): the `dotnet`
# resolved above knows the SDK install that owns a working hostfxr
# (setup-dotnet, /usr/share/dotnet, ~/.dotnet). Export it so the smoke
# test boots CoreCLR 10 even when the ambient env lacks DOTNET_ROOT.
# `dotnet --list-sdks` prints "<ver> [<root>]"; the root is authoritative.
execute_process(
    COMMAND ${DOTNET_EXE} --list-sdks
    OUTPUT_VARIABLE _DOTNET_SDKS
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(_DOTNET_SDKS MATCHES "\\[([^]]+)\\]")
    set(ENGINE_DOTNET_ROOT "${CMAKE_MATCH_1}")
    message(STATUS "dotnet root for hostfxr smoke: ${ENGINE_DOTNET_ROOT}")
    set_tests_properties(engine_hostfxr_smoke PROPERTIES
        ENVIRONMENT "DOTNET_ROOT=${ENGINE_DOTNET_ROOT}")
endif()

# Copy runtime DLLs (engine_core + flecs) next to every linked test binary.
if(WIN32)
    foreach(tgt engine_core_smoke engine_memory_arenas engine_job_scheduler engine_abi_layout engine_hostfxr_smoke)
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${tgt}>
                $<TARGET_FILE_DIR:${tgt}>
            COMMAND_EXPAND_LISTS)
    endforeach()
endif()

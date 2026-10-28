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

# Windows: the loader only searches the exe dir + PATH, so a test in
# native/tests/ cannot see engine_core.dll in native/. Copy runtime DLLs
# next to every linked test binary (Linux/macOS resolve via rpath instead).
# NOTE: bootstrap_smoke links nothing — an empty TARGET_RUNTIME_DLLS
# expansion would make cmake -E fail the build, so it is excluded here.
if(WIN32)
    foreach(tgt engine_core_smoke engine_memory_arenas)
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${tgt}>
                $<TARGET_FILE_DIR:${tgt}>
            COMMAND_EXPAND_LISTS)
    endforeach()
endif()

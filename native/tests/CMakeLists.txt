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

# Windows: the loader only searches the exe dir + PATH, so a test in
# native/tests/ cannot see engine_core.dll in native/. Copy runtime DLLs
# next to every test binary (Linux/macOS resolve via rpath instead).
if(WIN32)
    foreach(tgt engine_core_smoke bootstrap_smoke)
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:${tgt}>
                $<TARGET_FILE_DIR:${tgt}>
            COMMAND_EXPAND_LISTS)
    endforeach()
endif()

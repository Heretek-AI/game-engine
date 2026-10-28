# native/tests — CTest smoke tests proving the M1-01 pipeline links and runs.
# NOTE: tests use base flags (exceptions/RTTI on) — they are host tooling,
# not frame-path code.
add_executable(engine_core_smoke test_engine_core_smoke.cpp)

target_link_libraries(engine_core_smoke PRIVATE engine_core)

engine_apply_base_flags(engine_core_smoke)

add_test(NAME engine_core_smoke COMMAND engine_core_smoke)

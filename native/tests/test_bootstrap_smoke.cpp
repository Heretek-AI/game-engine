// test_bootstrap_smoke.cpp — links nothing: proves the runner can execute
// a trivial binary. If this passes and engine_core_smoke hangs, the fault
// is in engine_core DLL load / static init, not the environment.
#include <cstdio>

auto main() -> int {
    std::printf("[PASS] bootstrap alive\n");
    return 0;
}

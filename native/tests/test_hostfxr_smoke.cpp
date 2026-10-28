// test_hostfxr_smoke.cpp — M1-05 acceptance: CoreCLR boots from
// libengine_core via hostfxr (DotNetHost), binds an [UnmanagedCallersOnly]
// entry point, and the bound pointer executes.
//
// This is the embedding path the AC requires (dotnet_host_create ->
// hostfxr_initialize_for_runtime_config -> hostfxr_get_runtime_delegate ->
// load_assembly_and_get_function_pointer), exercised from a NATIVE process
// so hostfxr owns the one CoreCLR instance (a second init in a `dotnet`
// process would be refused with rc=1).
//
// Layout: the test needs the managed build outputs. CMake drives them:
// BENCHMARK1M_DLL / BENCHMARK1M_CONFIG are configured at build time from
// `dotnet build` of Engine.slnx (see native/tests/CMakeLists.txt).
// The fixture assembly (Benchmark1M.Systems.dll) exposes:
//   Engine.Systems.MovementSystem.SetDt(float)         — publish frame dt
//   Engine.Systems.MovementSystem.MoveUnrolled(chunk*) — integrate rows
// and the test asserts correct results + the milestone timing.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "engine/bridge/dotnet_host.hpp"
#include "engine/bridge/engine_abi.h"

// Fixture paths, baked in by CMake (absolute paths to the dotnet outputs).
#ifndef BENCHMARK1M_SYSTEMS_DLL
#define BENCHMARK1M_SYSTEMS_DLL ""
#endif
#ifndef BENCHMARK1M_CONFIG
#define BENCHMARK1M_CONFIG ""
#endif

namespace {

auto check(bool ok, const char* name) -> int {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  return ok ? 0 : 1;
}

// Managed callback signature: static void MoveUnrolled(EcsChunkView*).
using MoveFn = void (*)(const EcsChunkView*);
using SetDtFn = void (*)(float);

}  // namespace

auto main() -> int {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  int failures = 0;

  failures +=
      check(BENCHMARK1M_SYSTEMS_DLL[0] != '\0', "smoke: systems dll configured");
  failures +=
      check(BENCHMARK1M_CONFIG[0] != '\0', "smoke: runtime config configured");
  if (failures != 0) {
    return 1;
  }
  std::printf("[info] systems: %s\n", BENCHMARK1M_SYSTEMS_DLL);
  std::printf("[info] config:  %s\n", BENCHMARK1M_CONFIG);

  // --- 1. Boot CoreCLR through the native DotNetHost -------------------------
  engine::bridge::DotNetHost* host = engine::bridge::dotnet_host_create(
      BENCHMARK1M_CONFIG, nullptr);
  failures += check(host != nullptr, "smoke: hostfxr init (CoreCLR booted)");
  if (host == nullptr) {
    return 1;
  }
  failures += check(engine::bridge::dotnet_host_load_fn(host) != nullptr,
                    "smoke: load_assembly delegate resolved");

  // --- 2. Bind the [UnmanagedCallersOnly] entry points -----------------------
  auto* set_dt = reinterpret_cast<SetDtFn>(  // NOLINT
      engine::bridge::dotnet_host_get_system_pointer(
          host, BENCHMARK1M_SYSTEMS_DLL,
          "Engine.Systems.MovementSystem, Benchmark1M.Systems", "SetDt"));
  failures += check(set_dt != nullptr, "smoke: SetDt bound via hostfxr");
  auto* move_fn = reinterpret_cast<MoveFn>(  // NOLINT
      engine::bridge::dotnet_host_get_system_pointer(
          host, BENCHMARK1M_SYSTEMS_DLL,
          "Engine.Systems.MovementSystem, Benchmark1M.Systems",
          "MoveUnrolled"));
  failures += check(move_fn != nullptr, "smoke: MoveUnrolled bound via hostfxr");
  if (set_dt == nullptr || move_fn == nullptr) {
    engine::bridge::dotnet_host_destroy(host);
    return 1;
  }

  // --- 3. Correctness: SetDt + MoveUnrolled over one synthetic chunk --------
  // Nonzero velocity pattern (1,2,3) so a wrong-column / wrong-stride
  // wiring cannot hide behind zeros; exact float equality: 0.016/0.032/
  // 0.048 are the products the kernel computes (dt=0.016 * vel).
  {
    constexpr int kN = 4096;
    float* local =
        static_cast<float*>(std::aligned_alloc(64, sizeof(float) * 4096 * 16));
    float* vel =
        static_cast<float*>(std::aligned_alloc(16, sizeof(float) * 4096 * 4));
    if (local == nullptr || vel == nullptr) {
      std::free(local);
      std::free(vel);
      failures += check(false, "smoke: probe buffers allocated");
      engine::bridge::dotnet_host_destroy(host);
      return 1;
    }
    std::memset(local, 0, sizeof(float) * 4096 * 16);
    for (int i = 0; i < kN; ++i) {
      vel[i * 4] = 1.0F;
      vel[i * 4 + 1] = 2.0F;
      vel[i * 4 + 2] = 3.0F;
      vel[i * 4 + 3] = 0.0F;
    }
    EcsChunkView view{};
    view.local = reinterpret_cast<const EngineLocalTransform*>(local);  // NOLINT
    view.world = nullptr;
    view.entities = nullptr;
    view.count = kN;
    view.stride_local = 64;
    view.stride_world = 64;
    view.user_data = vel;
    set_dt(0.016F);
    move_fn(&view);
    // 1 frame at dt=0.016: px=0.016, py=0.032, pz=0.048.
    bool ok = true;
    for (int i = 0; i < kN && ok; ++i) {
      ok &= (local[i * 16] == 0.016F);
      ok &= (local[i * 16 + 1] == 0.032F);
      ok &= (local[i * 16 + 2] == 0.048F);
    }
    failures += check(ok, "smoke: MoveUnrolled integrates 4096 rows");
    std::free(local);
    std::free(vel);
  }

  // --- 4. Milestone timing: 1M rows through the ABI world --------------------
  // Full path: bulk_create 1M (Local, World, Velocity) -> register the
  // hostfxr-bound MoveUnrolled -> run_systems dispatches serially (M1-05).
  // NOTE: no nonzero-velocity seeding here — bulk_create zeroes both
  // columns and zeros integrate to zeros through any wiring bug. Exact
  // numerical correctness is judged by the synthetic-chunk probe above
  // (nonzero pattern, exact equality) and by the managed runner's probe.
  const EngineApiTable* api = engine_get_api_table();
  failures += check(api != nullptr, "smoke: api table live");
  EngineWorld* world = (api != nullptr) ? api->world_create() : nullptr;
  failures += check(world != nullptr, "smoke: world_create");
  if (world == nullptr) {
    engine::bridge::dotnet_host_destroy(host);
    return 1;
  }
  constexpr std::uint64_t kRows = 1000000;
  const std::uint64_t made = api->world_bulk_create(world, kRows);
  failures += check(made == kRows, "smoke: bulk_create 1M");
  failures += check(api->register_system(
                        world, "move",
                        reinterpret_cast<ManagedSystemCallback>(move_fn),  // NOLINT
                        nullptr) == 0,
                    "smoke: register hostfxr-bound MoveUnrolled");
  const float dt_frame = 1.0F / 60.0F;
  set_dt(dt_frame);
  failures += check(api->run_systems(world) == 0, "smoke: warm run_systems");
  double best_ms = 1e9;
  for (int k = 0; k < 11; ++k) {
    const auto t0 = std::chrono::steady_clock::now();
    const int rc = api->run_systems(world);
    const auto t1 = std::chrono::steady_clock::now();
    if (rc != 0) {
      failures += check(false, "smoke: timed run_systems green");
      break;
    }
    const double ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (ms < best_ms) {
      best_ms = ms;
    }
    std::printf("[info] frame %2d = %8.3f ms\n", k, ms);
  }
  std::printf("[info] Benchmark1M-native-hostfxr: 1M best-of-11 = %.3f ms\n",
              best_ms);
  failures += check(best_ms < 1.5, "smoke: 1M update < 1.5ms via UCO callback");

  api->world_destroy(world);

  engine::bridge::dotnet_host_destroy(host);
  engine::bridge::dotnet_host_destroy(nullptr);  // idempotent on NULL
  std::printf(failures == 0 ? "test_hostfxr_smoke: all checks passed\n"
                            : "test_hostfxr_smoke: %d FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}

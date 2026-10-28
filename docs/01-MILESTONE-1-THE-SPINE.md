# Milestone 1: The Spine (Native ECS + Zero-Copy .NET 10 Host)

* **Timeline:** Months 1–2
* **Verification Gate:** A managed C# script iterates and mutates $1,000,000$ `LocalTransform` structs stored in native C++ Flecs tables in $< 1.5\text{ ms}$ across worker threads with $0\text{ bytes}$ of GC allocation.

---

## GitHub Issues Breakdown

### Issue M1-01: Initialize Monorepo, CMake C++20 Build, and `.slnx` Workspace
* **Labels:** `area:native-core`, `priority:critical`
* **Description:** Set up the unified build pipeline compiling `libengine_core` (`native/`) via CMake + Ninja + Clang/MSVC (C++20) alongside the `.NET 10` (`Engine.slnx`) managed solution.
* **Acceptance Criteria:**
  - [ ] Configure root `CMakeLists.txt` with strict C++20 flags (`-fno-exceptions`, `-fno-rtti` in hot modules, `-mavx2`).
  - [ ] Vendor `flecs` v4.x and `enkiTS` in `deps/`.
  - [ ] Create `Engine.slnx` and `Directory.Build.props` targeting `net10.0` with `<AllowUnsafeBlocks>true</AllowUnsafeBlocks>` and `<TieredPGO>true</TieredPGO>`.
  - [ ] Add GitHub Actions CI workflow verifying clean compilation on Windows, Linux, and macOS.

---

### Issue M1-02: Native Memory Arenas & 64-Byte Cache-Aligned Allocators
* **Labels:** `area:native-core`
* **Description:** Eliminate general-purpose `malloc`/`new` from frame execution by implementing custom native allocators in `native/core/memory/`.
* **Acceptance Criteria:**
  - [ ] Implement `LinearFrameAllocator` with atomic bump pointer that resets at every frame boundary.
  - [ ] Implement `PageArenaAllocator` backed by OS virtual memory (`VirtualAlloc` / `mmap`).
  - [ ] Override Flecs `ecs_os_api_t` memory hooks (`malloc_`, `calloc_`, `realloc_`, `free_`) to enforce 64-byte cache-line alignment (`alignas(64)`) on all component table columns.

---

### Issue M1-03: Work-Stealing Job Scheduler & Flecs Hierarchy Pipeline
* **Labels:** `area:native-core`
* **Description:** Integrate a lock-free work-stealing thread pool (`enkiTS`) with $N - 1$ worker threads pinned to physical CPU cores and wire it into Flecs' staging pipeline.
* **Acceptance Criteria:**
  - [ ] Bind `enkiTS` worker threads to Flecs' multi-threaded task API (`ecs_os_api.task_new_`).
  - [ ] Implement core built-in native components: `LocalTransform` (`vec3` pos, `quat` rot, `vec3` scale) and `WorldTransform` (`mat4x4`).
  - [ ] Implement breadth-first hierarchical transform propagation (`EcsCascade` over `EcsChildOf`) using SIMD `AVX2`/`NEON` matrix multiplication.

---

### Issue M1-04: C99 ABI Contract (`engine_abi.h`) & Automated C# Binding Generator
* **Labels:** `area:abi-bridge`, `priority:critical`
* **Description:** Create the single-entry-point C99 ABI header (`engine_get_api_table()`) and a build tool (`tools/abi_codegen`) that emits zero-overhead C# function pointers.
* **Acceptance Criteria:**
  - [ ] Define `EcsChunkView`, `ManagedSystemCallback`, and `EngineApiTable` in `native/bridge/engine_abi.h`.
  - [ ] Build `tools/abi_codegen` to parse `engine_abi.h` and emit `managed/Engine.Bindings/NativeApi.g.cs`.
  - [ ] Ensure all generated C# function pointers use `delegate* unmanaged[Cdecl, SuppressGCTransition]<...>` so P/Invoke calls compile to a single raw `call` instruction.

---

### Issue M1-05: Embed `.NET 10` (`hostfxr`) & 1M Entity Zero-Copy Benchmark
* **Labels:** `area:abi-bridge`, `priority:critical`
* **Description:** Embed the CoreCLR runtime inside `libengine_core` via `hostfxr` and execute the Milestone 1 verification benchmark.
* **Acceptance Criteria:**
  - [ ] Implement `DotNetHost` in `native/bridge/` using `hostfxr_initialize_for_runtime_config` and `load_assembly_and_get_function_pointer`.
  - [ ] Pass raw Flecs table pointers (`EcsChunkView*`) from C++ worker threads into an `[UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]` C# callback.
  - [ ] Create `samples/Benchmark1M/` spawning $1,000,000$ entities with `LocalTransform` and `Velocity`.
  - [ ] Verify frame update completes in $< 1.5\text{ ms}$ with $0\text{ bytes}$ allocated on the managed heap (`GC.GetAllocatedBytesForCurrentThread() == 0`).
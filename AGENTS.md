# AGENTS.md — game-engine

## Invariants
- Native owns all frame state (`native/`). Managed (`managed/`) holds no gameplay state; zero-copy only via `EcsChunkView*` + `[UnmanagedCallersOnly]` + `SuppressGCTransition`.
- Every change to `native/bridge/engine_abi.h` must regenerate `managed/Engine.Bindings/NativeApi.g.cs` via `tools/abi_codegen` and run layout assertions.
- No `malloc`/`new` on frame path: `LinearFrameAllocator` (reset/frame) + `PageArenaAllocator` (virtual memory). Flecs columns `alignas(64)`.
- `IComponent` structs are blittable only (no `string`/`class`/`interface`/arrays/LINQ/closures in hot path). Analyzers ENG0001–ENG0003 enforce.
- Hot-reload is stateless: component data stays in Flecs; `GameScripts.dll` loads from memory stream via collectible ALC; schema evolution migrates tables.

## Workflow (Hermes orchestration)
- Board: `game-engine`. One task per issue `M1-01`…`M5-04`. Worker claims atomically, works in `hermes -w` worktree, opens PR, `qa-gate` reviews.
- Profiles: `chief-architect` (routes + gates), `cpp-ecs-core`, `dotnet-roslyn-bridge`, `vulkan-renderer`, `editor-cli-ux`, `qa-gate` (adversarial verify, must pass CMake + `dotnet test` + Lavapipe headless before merge).
- Memory: record ABI breaks, Flecs/Jolt gotchas, failed-build postmortems. Prefer `flecs-jolt-patterns`, `zero-copy-abi-guard`, `roslyn-incremental-gen` skills.

## Verification gates (definition of done per milestone)
- M1: Benchmark1M <1.5ms, `GC.GetAllocatedBytesForCurrentThread()==0`.
- M2: edit-save → 100k entities change behavior <100ms, no state loss, exceptions fault-isolate.
- M3: `vulkaninfo` + Lavapipe headless pass; 250k draws via `vkCmdDrawIndexedIndirectCount`, <0.5ms CPU render thread.
- M4: 30k bodies locked 60 FPS, `Physics.RaycastBatch` parallel, collision ring-buffer → `ReadOnlySpan<CollisionEvent>`.
- M5: drop `.glb`/`.png` → visible <16ms/1 frame; `engine-cli scene tree|entity set|snapshot capture|profile --frames 60`; `.pak` + `dotnet publish -r <rid> /p:PublishAot=true`, cold boot <1.0s.

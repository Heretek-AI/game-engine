# Game Engine — Native C++20 + .NET 10

100% world state in native C++20 (Flecs / Jolt / Vulkan 1.3 / miniaudio), 0% gameplay state on the .NET GC heap. `.NET 10` (`hostfxr`) is a stateless logic runner mutating native memory zero-copy via `Span<T>` + `delegate* unmanaged[SuppressGCTransition]`.

End users write Unity-like `IComponent` scripts. Roslyn Incremental Source Generators compile them to burst-vectorized native chunk loops. Compile-time analyzers forbid hot-path allocations.

## Layout

```text
engine-root/
├── docs/            # Architecture roadmap + M1–M5 milestones + orchestration plan
├── scripts/         # bootstrap-github-issues.sh (labels, milestones, issues)
├── native/          # libengine_core (C++20): core, ecs, renderer, physics, audio, bridge
├── managed/         # Engine.Bindings / Runtime / Analyzers / Editor / Cli
├── tools/           # abi_codegen (bridge JSON -> C# bindings)
└── samples/         # Benchmark1M, stress scenes, FPS template
```

## Milestones & Gates

| Milestone | Gate |
|---|---|
| M1 Spine (native ECS + zero-copy host) | 1M `LocalTransform` from C# in <1.5ms, 0 GC bytes |
| M2 Illusion compiler (Roslyn + hot-reload) | <100ms state-preserving C# hot-reload |
| M3 GPU-driven Vulkan 1.3 renderer | 250k PBR meshes + SDF GI @120+ FPS, <0.5ms CPU |
| M4 Jolt physics + spatial audio | 30k rigidbodies + zero-alloc C# callbacks @60+ FPS |
| M5 Editor UX + asset pipeline + CLI | <16ms asset streaming, `engine-cli` IPC, 1-click NativeAOT export |

## Orchestration (Hermes-native)

- 6 profiles: `chief-architect`, `cpp-ecs-core`, `dotnet-roslyn-bridge`, `vulkan-renderer`, `editor-cli-ux`, `qa-gate`
- Work queue: `hermes kanban` board `game-engine` (tasks `M1-01`…`M5-04`)
- Memory: `lore` active; `mem0` OSS optional on `https://llm.heretek.one/v1` (`lemonade/embeddinggemma-2`, 768 dims)
- Bootstrap GitHub: `./scripts/bootstrap-github-issues.sh`

## Start (M1-01)

```bash
hermes kanban list --board game-engine
hermes -p cpp-ecs-core -w  # isolated worktree mode for code tasks
```

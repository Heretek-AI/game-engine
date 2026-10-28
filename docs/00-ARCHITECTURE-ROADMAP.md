# Architecture & Master Roadmap: Native Core + Zero-Copy .NET 10 Engine

## 1. Executive Summary & Core Thesis
This engine is architected to surpass **Unreal Engine 5** in deterministic CPU/GPU frame times and entity scale while matching or exceeding **Unity** in C# developer ergonomics and iteration speed.

### The Core Architectural Split
1. **100% of World State in Native C++20 (`native/`):** All ECS component arrays (Flecs v4), physics islands (Jolt), GPU command buffers (Vulkan 1.3), and audio streams (`miniaudio`) reside in 64-byte cache-line-aligned native memory.
2. **0% of Gameplay State on the .NET GC Heap (`managed/`):** The embedded `.NET 10` (`hostfxr`) runtime acts as a stateless logic runner and Editor UI host. It mutates native C++ memory directly in-place via zero-copy `Span<T>` views and `delegate* unmanaged[SuppressGCTransition]` function pointer tables.
3. **The Illusion Layer (`Engine.Analyzers`):** End users write intuitive, Unity-like `IComponent` scripts. Roslyn Incremental Source Generators secretly compile those methods into burst-vectorized, multithreaded native chunk loops at build time while compile-time analyzers forbid heap allocations in hot paths.

---

## 2. Monorepo Topology

```text
engine-root/
├── CMakeLists.txt                      # Master C++20 build (Ninja + Clang/MSVC)
├── Engine.slnx                         # .NET 10 Solution for Host, Editor, Analyzers, CLI
├── Directory.Build.props               # Strict C# flags, AllowUnsafeBlocks, Zero-Warn
├── deps/                               # Vendored/Submodule Native Dependencies
│   ├── flecs/                          # v4.x Archetype/Sparse ECS (C99/C++)
│   ├── JoltPhysics/                    # Multi-core deterministic physics
│   ├── volk/ & VulkanMemoryAllocator/  # Vulkan 1.3 loader + VMA
│   ├── slang/                          # Shader compiler (SPIR-V / DXIL / MSL)
│   └── miniaudio/                      # Low-latency native audio
├── native/                             # THE NATIVE CORE (libengine_core.so / .dll)
│   ├── core/                           # Arenas, SIMD math, SDL3 windowing, JobScheduler
│   ├── ecs/                            # Flecs world wrapper, hierarchy/prefab bridges
│   ├── renderer/                       # Vulkan 1.3 GPU-driven bindless renderer & FrameGraph
│   ├── physics/                        # Jolt <-> Flecs transform sync & BroadPhase layers
│   ├── audio/                          # miniaudio spatial graph
│   └── bridge/                         # C99 ABI boundary + JSON schema dumper
├── tools/
│   └── abi_codegen/                    # Parses bridge JSON -> emits C# unmanaged structs/ptrs
├── managed/                            # THE .NET 10 HOST & ILLUSION LAYER
│   ├── Engine.Bindings/                # Auto-generated SuppressGCTransition P/Invoke tables
│   ├── Engine.Runtime/                 # Stateless ALC host, Component base, Query<T>, Time
│   ├── Engine.Analyzers/               # Roslyn Source Generators (OOP -> ECS) & GC Guardrails
│   ├── Engine.Editor/                  # ImGui/Docking Viewport, AssetDB, Prefabs, Undo/Redo
│   │   ├── AssetsDatabase/             # Async background BC7/glTF streaming + .meta files
│   │   └── Cli/                        # Built-in IPC/JSON-RPC server for AI/CI automation
│   └── Engine.Cli/                     # Standalone terminal client
└── samples/                            # Validation scenes (1M Entity Stress Test, FPS Template)
```

---

## 3. Milestone Overview & Verification Gates

| Milestone | Document | Target Timeline | Verification Gate ("Definition of Done") |
| :--- | :--- | :--- | :--- |
| **M1: The Spine** | `docs/01-MILESTONE-1-THE-SPINE.md` | Months 1–2 | C# script mutates 1,000,000 `LocalTransform` structs in native Flecs memory in $< 1.5\text{ ms}$ with $0\text{ bytes}$ of GC allocation. |
| **M2: The Illusion Compiler** | `docs/02-MILESTONE-2-ILLUSION-COMPILER.md` | Months 3–4 | Edit a C# movement script, hit save, and see 100,000 moving entities change direction in $< 100\text{ ms}$ without losing velocity state. |
| **M3: GPU-Driven Renderer** | `docs/03-MILESTONE-3-VULKAN-RENDERER.md` | Months 5–7 | Render 250,000 unique dynamic meshes with PBR materials and real-time GI at $120+\text{ FPS}$ with $< 0.5\text{ ms}$ CPU render thread time. |
| **M4: Physics & Audio** | `docs/04-MILESTONE-4-PHYSICS-AND-AUDIO.md` | Months 8–9 | Spawn 30,000 colliding rigidbodies in a pit with active C# collision callbacks running at a locked $60+\text{ FPS}$. |
| **M5: Editor UX & CLI** | `docs/05-MILESTONE-5-EDITOR-AND-CLI.md` | Months 10–12 | Build and export a standalone, NativeAOT-compiled 3D FPS playable demo from the Editor in one click, fully controllable via `engine-cli`. |

---

## 4. Standard GitHub Labels
Use the accompanying `scripts/bootstrap-github-issues.sh` script to automatically create these labels in your repository:
* `area:native-core` (`#d73a4a`) — C++20 memory, job system, Flecs ECS, OS abstraction
* `area:abi-bridge` (`#0075ca`) — `hostfxr`, C99 ABI headers, `abi_codegen`, zero-copy marshaling
* `area:roslyn-illusion` (`#a2eeef`) — Roslyn Incremental Source Generators & GC Diagnostic Analyzers
* `area:renderer` (`#7057ff`) — Vulkan 1.3 bindless, Slang shaders, GPU culling, SDF GI
* `area:physics-audio` (`#008672`) — Jolt Physics & `miniaudio` integration
* `area:editor-cli` (`#e4e669`) — Asset pipeline, Editor UI, Prefabs, IPC/CLI protocol
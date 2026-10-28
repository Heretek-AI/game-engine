#!/usr/bin/env bash
# ==============================================================================
# GitHub Roadmap, Labels, Milestones & Issues Bootstrapper
# Prerequisite: Install GitHub CLI (https://cli.github.com/) and run `gh auth login`
# Usage: chmod +x scripts/bootstrap-github-issues.sh && ./scripts/bootstrap-github-issues.sh
# ==============================================================================

set -euo pipefail

echo "==> 1. Creating Architecture Labels..."
gh label create "area:native-core"     --color "d73a4a" --description "C++20 memory, job system, Flecs ECS, OS abstraction" --force
gh label create "area:abi-bridge"      --color "0075ca" --description "hostfxr, C99 ABI headers, abi_codegen, zero-copy marshaling" --force
gh label create "area:roslyn-illusion" --color "a2eeef" --description "Roslyn Incremental Source Generators & GC Analyzers" --force
gh label create "area:renderer"        --color "7057ff" --description "Vulkan 1.3 bindless, Slang shaders, GPU culling, SDF GI" --force
gh label create "area:physics-audio"   --color "008672" --description "Jolt Physics & miniaudio integration" --force
gh label create "area:editor-cli"      --color "e4e669" --description "Asset pipeline, Editor UI, Prefabs, IPC/CLI protocol" --force
gh label create "priority:critical"    --color "b60205" --description "Core architectural blocker for milestone gate" --force

echo "==> 2. Creating 5 Roadmap Milestones..."
REPO=$(gh repo view --json nameWithOwner -q .nameWithOwner)

create_milestone() {
  local title="$1"
  local desc="$2"
  gh api "repos/${REPO}/milestones" -f title="$title" -f description="$desc" >/dev/null || true
}

create_milestone "M1: The Spine (Native ECS + Zero-Copy Host)" "Gate: 1M LocalTransform updates from C# in <1.5ms with 0 GC allocations."
create_milestone "M2: The Illusion Compiler (Roslyn + Hot Reload)" "Gate: <100ms state-preserving C# hot-reload + OOP-to-ECS source generation."
create_milestone "M3: GPU-Driven Vulkan 1.3 Renderer" "Gate: 250k dynamic PBR meshes + real-time SDF GI at 120+ FPS (<0.5ms CPU)."
create_milestone "M4: Deterministic Jolt Physics & Audio" "Gate: 30k colliding rigidbodies + zero-cost C# raycasts & spatial audio at 60+ FPS."
create_milestone "M5: Editor UX, Asset Pipeline & Agent CLI" "Gate: <16ms background asset streaming, engine-cli IPC server, 1-click NativeAOT export."

echo "==> 3. Populating Issues across Milestones..."

# --- MILESTONE 1 ---
gh issue create --title "[M1-01] Initialize Monorepo, CMake C++20 Build, and .slnx Workspace" \
  --milestone "M1: The Spine (Native ECS + Zero-Copy Host)" \
  --label "area:native-core,priority:critical" \
  --body "See \`docs/01-MILESTONE-1-THE-SPINE.md\` (Issue M1-01) for full specification and acceptance criteria."

gh issue create --title "[M1-02] Native Memory Arenas & 64-Byte Cache-Aligned Allocators" \
  --milestone "M1: The Spine (Native ECS + Zero-Copy Host)" \
  --label "area:native-core" \
  --body "See \`docs/01-MILESTONE-1-THE-SPINE.md\` (Issue M1-02) for full specification and acceptance criteria."

gh issue create --title "[M1-03] Work-Stealing Job Scheduler & Flecs Hierarchy Pipeline" \
  --milestone "M1: The Spine (Native ECS + Zero-Copy Host)" \
  --label "area:native-core" \
  --body "See \`docs/01-MILESTONE-1-THE-SPINE.md\` (Issue M1-03) for full specification and acceptance criteria."

gh issue create --title "[M1-04] C99 ABI Contract (engine_abi.h) & Automated C# Binding Generator" \
  --milestone "M1: The Spine (Native ECS + Zero-Copy Host)" \
  --label "area:abi-bridge,priority:critical" \
  --body "See \`docs/01-MILESTONE-1-THE-SPINE.md\` (Issue M1-04) for full specification and acceptance criteria."

gh issue create --title "[M1-05] Embed .NET 10 (hostfxr) & 1M Entity Zero-Copy Benchmark" \
  --milestone "M1: The Spine (Native ECS + Zero-Copy Host)" \
  --label "area:abi-bridge,priority:critical" \
  --body "See \`docs/01-MILESTONE-1-THE-SPINE.md\` (Issue M1-05) for full specification and acceptance criteria."

# --- MILESTONE 2 ---
gh issue create --title "[M2-01] Ergonomic IComponent, Entity, and Phase Attributes API" \
  --milestone "M2: The Illusion Compiler (Roslyn + Hot Reload)" \
  --label "area:roslyn-illusion" \
  --body "See \`docs/02-MILESTONE-2-ILLUSION-COMPILER.md\` (Issue M2-01) for full specification and acceptance criteria."

gh issue create --title "[M2-02] Roslyn IIncrementalGenerator (IComponent to Native Chunk Job)" \
  --milestone "M2: The Illusion Compiler (Roslyn + Hot Reload)" \
  --label "area:roslyn-illusion,priority:critical" \
  --body "See \`docs/02-MILESTONE-2-ILLUSION-COMPILER.md\` (Issue M2-02) for full specification and acceptance criteria."

gh issue create --title "[M2-03] Compile-Time Performance & Memory Safety Roslyn Analyzers" \
  --milestone "M2: The Illusion Compiler (Roslyn + Hot Reload)" \
  --label "area:roslyn-illusion" \
  --body "See \`docs/02-MILESTONE-2-ILLUSION-COMPILER.md\` (Issue M2-03) for full specification and acceptance criteria."

gh issue create --title "[M2-04] Stateless Collectible AssemblyLoadContext 100ms Hot-Reload" \
  --milestone "M2: The Illusion Compiler (Roslyn + Hot Reload)" \
  --label "area:abi-bridge,area:roslyn-illusion,priority:critical" \
  --body "See \`docs/02-MILESTONE-2-ILLUSION-COMPILER.md\` (Issue M2-04) for full specification and acceptance criteria."

# --- MILESTONE 3 ---
gh issue create --title "[M3-01] SDL3 Windowing, Vulkan 1.3 Bootstrap (volk + VMA), and Render Graph" \
  --milestone "M3: GPU-Driven Vulkan 1.3 Renderer" \
  --label "area:renderer,priority:critical" \
  --body "See \`docs/03-MILESTONE-3-VULKAN-RENDERER.md\` (Issue M3-01) for full specification and acceptance criteria."

gh issue create --title "[M3-02] Bindless Descriptor Table & GPU-Driven Indirect Drawing" \
  --milestone "M3: GPU-Driven Vulkan 1.3 Renderer" \
  --label "area:renderer,priority:critical" \
  --body "See \`docs/03-MILESTONE-3-VULKAN-RENDERER.md\` (Issue M3-02) for full specification and acceptance criteria."

gh issue create --title "[M3-03] Slang Shader Compiler Pipeline & C# GraphicsCompositor API" \
  --milestone "M3: GPU-Driven Vulkan 1.3 Renderer" \
  --label "area:renderer,area:abi-bridge" \
  --body "See \`docs/03-MILESTONE-3-VULKAN-RENDERER.md\` (Issue M3-03) for full specification and acceptance criteria."

gh issue create --title "[M3-04] Zero-Bake Real-Time GI (Signed Distance Fields + Radiance Cascades)" \
  --milestone "M3: GPU-Driven Vulkan 1.3 Renderer" \
  --label "area:renderer" \
  --body "See \`docs/03-MILESTONE-3-VULKAN-RENDERER.md\` (Issue M3-04) for full specification and acceptance criteria."

# --- MILESTONE 4 ---
gh issue create --title "[M4-01] Jolt Physics Integration & Shared Work-Stealing Thread Pool" \
  --milestone "M4: Deterministic Jolt Physics & Audio" \
  --label "area:physics-audio,priority:critical" \
  --body "See \`docs/04-MILESTONE-4-PHYSICS-AND-AUDIO.md\` (Issue M4-01) for full specification and acceptance criteria."

gh issue create --title "[M4-02] Zero-Glue Parallel Flecs <-> Jolt Transform Synchronization" \
  --milestone "M4: Deterministic Jolt Physics & Audio" \
  --label "area:physics-audio,area:native-core" \
  --body "See \`docs/04-MILESTONE-4-PHYSICS-AND-AUDIO.md\` (Issue M4-02) for full specification and acceptance criteria."

gh issue create --title "[M4-03] Zero-Cost C# Spatial Queries (Physics.Raycast / OverlapSphere)" \
  --milestone "M4: Deterministic Jolt Physics & Audio" \
  --label "area:physics-audio,area:abi-bridge" \
  --body "See \`docs/04-MILESTONE-4-PHYSICS-AND-AUDIO.md\` (Issue M4-03) for full specification and acceptance criteria."

gh issue create --title "[M4-04] Low-Latency 3D Spatial Audio Graph (miniaudio)" \
  --milestone "M4: Deterministic Jolt Physics & Audio" \
  --label "area:physics-audio" \
  --body "See \`docs/04-MILESTONE-4-PHYSICS-AND-AUDIO.md\` (Issue M4-04) for full specification and acceptance criteria."

# --- MILESTONE 5 ---
gh issue create --title "[M5-01] Non-Blocking Background Asset Pipeline & .meta GUID Database" \
  --milestone "M5: Editor UX, Asset Pipeline & Agent CLI" \
  --label "area:editor-cli,priority:critical" \
  --body "See \`docs/05-MILESTONE-5-EDITOR-AND-CLI.md\` (Issue M5-01) for full specification and acceptance criteria."

gh issue create --title "[M5-02] Native-Backed Editor UI (Hierarchy, Inspector, Prefabs, Gizmos)" \
  --milestone "M5: Editor UX, Asset Pipeline & Agent CLI" \
  --label "area:editor-cli,priority:critical" \
  --body "See \`docs/05-MILESTONE-5-EDITOR-AND-CLI.md\` (Issue M5-02) for full specification and acceptance criteria."

gh issue create --title "[M5-03] First-Class IPC / JSON-RPC Server & engine-cli Control Plane" \
  --milestone "M5: Editor UX, Asset Pipeline & Agent CLI" \
  --label "area:editor-cli" \
  --body "See \`docs/05-MILESTONE-5-EDITOR-AND-CLI.md\` (Issue M5-03) for full specification and acceptance criteria."

gh issue create --title "[M5-04] One-Click Standalone NativeAOT Game Pak Exporter" \
  --milestone "M5: Editor UX, Asset Pipeline & Agent CLI" \
  --label "area:editor-cli,area:abi-bridge" \
  --body "See \`docs/05-MILESTONE-5-EDITOR-AND-CLI.md\` (Issue M5-04) for full specification and acceptance criteria."

echo "==> Done! All labels, milestones, and 20 issues have been created."
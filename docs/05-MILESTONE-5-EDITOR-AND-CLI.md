# Milestone 5: Effortless Editor UX, Instant Asset Streaming & Agent-Ready CLI

* **Timeline:** Months 10–12
* **Verification Gate:** Drop raw `.glb` and `.png` files into `Assets/` with $< 16\text{ ms}$ viewport appearance, inspect/mutate the live scene via `engine-cli`, and export a standalone NativeAOT-compiled 3D game in one click.

---

## GitHub Issues Breakdown

### Issue M5-01: Non-Blocking Background Asset Pipeline & `.meta` GUID Database
* **Labels:** `area:editor-cli`, `priority:critical`
* **Description:** Eliminate Unity-style "Importing Assets..." modal progress bars by streaming raw assets into the viewport immediately while compressing artifacts on background threads.
* **Acceptance Criteria:**
  - [ ] Implement `AssetWatcher` and human-readable `.meta` sidecar files with stable 128-bit `GUID`s.
  - [ ] Memory-map newly dropped `.glb`/`.gltf` and `.png`/`.jpg` files so uncompressed mipmaps appear in the Vulkan viewport within 1 frame ($< 16\text{ ms}$).
  - [ ] Run background worker jobs using `meshoptimizer` (vertex cache + LOD generation) and `bc7enc`/`astcenc` (texture compression) to populate `Library/ArtifactCache/` asynchronously.

---

### Issue M5-02: Native-Backed Editor UI (Hierarchy, Inspector, Prefabs, Gizmos)
* **Labels:** `area:editor-cli`, `priority:critical`
* **Description:** Build the docking Editor UI in `Engine.Editor` directly inspecting Flecs world tables and rendering the Vulkan 1.3 viewport with 3D transform gizmos.
* **Acceptance Criteria:**
  - [ ] Implement Scene Hierarchy panel backed by Flecs `EcsChildOf` queries and `EcsIsA` Prefab instances.
  - [ ] Implement Reflection/Source-Generated Inspector panel that edits native Flecs component memory in-place with full Command-pattern Undo/Redo support.
  - [ ] Implement GPU Object ID picking and 3D Translate/Rotate/Scale viewport gizmos.
  - [ ] Implement instant Play-In-Editor (PIE) mode using Flecs world snapshots (`ecs_world_snapshot`) so entering and exiting Play Mode takes $< 10\text{ ms}$.

---

### Issue M5-03: First-Class IPC / JSON-RPC Server & `engine-cli` Control Plane
* **Labels:** `area:editor-cli`
* **Description:** Embed a high-speed Unix-Socket / Named-Pipe server inside the Editor so CI pipelines, terminal scripts, and AI coding agents can inspect and control the engine programmatically.
* **Acceptance Criteria:**
  - [ ] Implement `CliServer` inside `Engine.Editor` and standalone `engine-cli` binary in `Engine.Cli`.
  - [ ] Support `engine-cli scene tree` (JSON hierarchy dump) and `engine-cli entity set <id> <Component.Field> <val>`.
  - [ ] Support `engine-cli snapshot capture --out frame.png --pass [final|albedo|normals|depth|wireframe]` for automated visual regression testing and AI agent visual feedback.
  - [ ] Support `engine-cli profile --frames 60` returning structured CPU/GPU pass timings and per-system ECS microsecond breakdowns.

---

### Issue M5-04: One-Click Standalone NativeAOT Game Pak Exporter
* **Labels:** `area:editor-cli`, `area:abi-bridge`
* **Description:** Package the native C++ runtime, compiled Slang SPIR-V shaders, compressed `ArtifactCache` assets, and NativeAOT-compiled C# gameplay assemblies into a zero-dependency standalone executable.
* **Acceptance Criteria:**
  - [ ] Pack all referenced assets by `GUID` into a memory-mappable `.pak` archive with LZ4/Zstd block compression.
  - [ ] Invoke `dotnet publish -r <rid> /p:PublishAot=true` on the user's `GameScripts` project to compile all C# scripts directly to native machine code (or embed the stripped `hostfxr` runtime).
  - [ ] Verify cold-boot time of the exported standalone game is $< 1.0\text{ second}$.
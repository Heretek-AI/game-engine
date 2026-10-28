# Milestone 2: The Illusion Compiler (Roslyn Source Generators & 100ms Hot-Reload)

* **Timeline:** Months 3–4
* **Verification Gate:** Edit a C# gameplay script (`IComponent`), press `Ctrl+S`, and observe $100,000$ active entities update their behavior in $< 100\text{ ms}$ without losing world state or pausing the engine.

---

## GitHub Issues Breakdown

### Issue M2-01: Ergonomic `IComponent`, `Entity`, and Phase Attributes API
* **Labels:** `area:roslyn-illusion`
* **Description:** Design the user-facing C# scripting surface in `Engine.Runtime` so developers write intuitive per-entity methods without touching pointers or ECS queries.
* **Acceptance Criteria:**
  - [ ] Define `public interface IComponent`, `readonly record struct Entity(ulong Id)`, and `[UpdateInPhase(Phase)]` attributes.
  - [ ] Provide blittable collection & string types (`FixedString64`, `TempList<T>` backed by the native `LinearFrameAllocator`, `AssetRef<T>`).
  - [ ] Expose SIMD-accelerated math types (`float3`, `float4`, `quaternion`, `float4x4`) mapped directly to `System.Numerics` / `System.Runtime.Intrinsics`.

---

### Issue M2-02: Roslyn `IIncrementalGenerator` (`IComponent` to Native Chunk Job)
* **Labels:** `area:roslyn-illusion`, `priority:critical`
* **Description:** Build the Roslyn Source Generator in `Engine.Analyzers` that intercepts any `partial struct : IComponent` with a `Tick(...)` method and compiles it into a zero-copy native chunk runner.
* **Acceptance Criteria:**
  - [ ] Parse `ref` (read-write) and `in` (read-only) component parameters on user `Tick()` methods.
  - [ ] Emit auto-registration code that registers the struct size, alignment (`sizeof(T)`), and component dependencies with Flecs via `NativeApi.EcsRegisterComponent` and `NativeApi.EcsRegisterSystem`.
  - [ ] Emit an `[UnmanagedCallersOnly]` static method that unpacks `EcsChunkView*` into `Span<T>` / `ReadOnlySpan<T>` slices and executes a tight, bounds-check-eliminated loop.

---

### Issue M2-03: Compile-Time Performance & Memory Safety Roslyn Analyzers
* **Labels:** `area:roslyn-illusion`
* **Description:** Prevent GC spikes and memory corruption at compile time by flagging illegal C# patterns inside hot ECS systems as the user types in their IDE.
* **Acceptance Criteria:**
  - [ ] **`ENG0001` (Heap Allocation in Hot Path):** Flag `new` class allocations, string interpolations, LINQ queries, and lambda closures inside `[UpdateInPhase]` methods. Provide a Roslyn CodeFix to replace `new List<T>()` with `TempList<T>`.
  - [ ] **`ENG0002` (Non-Blittable Component Field):** Forbid managed reference types (`string`, `class`, `interface`, arrays) inside any struct implementing `IComponent`.
  - [ ] **`ENG0003` (Blocking Async/Wait):** Forbid `.Wait()`, `.Result`, or `Thread.Sleep()` on engine threads.

---

### Issue M2-04: Stateless Collectible `AssemblyLoadContext` 100ms Hot-Reload
* **Labels:** `area:abi-bridge`, `area:roslyn-illusion`, `priority:critical`
* **Description:** Implement indestructible live code reloading by isolating user gameplay scripts inside a collectible `AssemblyLoadContext` while keeping 100% of component data in native C++ Flecs memory.
* **Acceptance Criteria:**
  - [ ] Implement `ScriptHostManager` in `Engine.Runtime` loading `GameScripts.dll` from an in-memory stream (preventing OS file locks on `.dll` / `.pdb` files).
  - [ ] At the frame sync barrier, unbind old `ManagedSystemCallback` pointers in Flecs, unload the old `AssemblyLoadContext`, load the newly compiled assembly, and re-bind the updated `[UnmanagedCallersOnly]` callbacks.
  - [ ] Support schema evolution: if a user adds or removes a field on an `IComponent` struct during hot-reload, automatically invoke Flecs component table migration so existing entity values remain intact.
  - [ ] Wrap managed system invocations in a native/managed fault barrier: if a user script throws an exception, log the stack trace, disable that system, and keep the engine/editor running without crashing.
# Spec: Illusion Layer end to end (ABI 4.2)

Status: agreed design, spiked and verified on Linux x64 against main `70846a5`.
Implementation brief: [`docs/IMPLEMENTATION_PROMPT.md`](../IMPLEMENTATION_PROMPT.md).

## Goal

A user writes a Unity-style component in C#:

```csharp
public partial struct Mover : IComponent
{
    public float Speed, X, Y;
    public readonly void Tick(ref Mover self, in Health health, float dt)
    {
        self.X += health.Value * dt;
        self.Y += self.Speed * dt;
    }
}
```

and, with no hand-written glue, `Engine.Analyzers` generates code that:

1. registers `Mover` and `Health` as real Flecs components (by full type name, size and alignment),
2. registers a native system whose query is exactly the Tick's component terms,
3. runs the Tick per row directly on Flecs column memory, with the real frame `dt`,
4. allocates nothing per frame and never lets a Tick exception cross the ABI.

Today none of this works: the generator output does not compile (CS1503, the
function pointer it emits lacks the `float dt` parameter), it aliases every
user component onto the `LocalTransform` column, and the native side has no
way to register user components or term-list systems.

## Non-goals

- Hot-reload integration (re-registering systems from a new collectible ALC, schema migration).
- Parallel dispatch of generic systems (they run serially, in registration order).
- Phases, ordering attributes, `Optional`/`Not` terms, singleton or pair terms.
- Removing the existing fixed-archetype path (`register_system` / `run_systems`, `EcsChunkView`). It stays as is.
- Windows/macOS verification (CI covers them; this spike ran on Linux only).

## Design

### Native ABI 4.2 (appended tail, minor bump)

`engine_abi.h` goes from 4.1.0 to 4.2.0. Six function pointers are appended
after `editor_snapshot_discard`. Existing entries keep their order and signatures,
but the loader (`NativeApi.ApiLoader`) requires an exact major.minor.patch match,
so managed bindings and the native library must both be 4.2.0. The minor bump
follows the M5-02 precedent (4.0 to 4.1 also appended a tail), even though the
header comment says to bump MAJOR on table changes:

| Field | Signature | Returns |
|---|---|---|
| `component_register` | `(EngineWorld*, const char* name, uint32_t size, uint32_t alignment)` | `uint64_t` id, 0 on failure |
| `entity_create` | `(EngineWorld*)` | `uint64_t` entity, 0 on failure |
| `entity_set` | `(EngineWorld*, uint64_t entity, uint64_t component, const void* data, uint32_t size)` | `int32_t` 0 or `ENGINE_ABI_ERR_*` |
| `entity_get` | `(EngineWorld*, uint64_t entity, uint64_t component, void* out, uint32_t size)` | `int32_t` 0 or `ENGINE_ABI_ERR_*` |
| `system_register` | `(EngineWorld*, const char* name, const uint64_t* components, const uint32_t* access, uint32_t term_count, EngineSystemCallback cb, void* user_data)` | `int32_t` system index ≥ 0 or `ENGINE_ABI_ERR_*` |
| `systems_run` | `(EngineWorld*, const FrameDispatch*)` | `int32_t` 0 or `ENGINE_ABI_ERR_*` |

`EngineApiTable` grows from 312 to 360 bytes. The callback receives a 120-byte view:

```c
#define ENGINE_ABI_MAX_TERMS 8
typedef struct EngineChunkColumns {
  void* columns[ENGINE_ABI_MAX_TERMS];      /* @0   */
  int32_t sizes[ENGINE_ABI_MAX_TERMS];      /* @64  */
  const uint64_t* entities;                 /* @96  */
  int32_t count;                            /* @104 */
  int32_t term_count;                       /* @108 */
  void* user_data;                          /* @112 */
} EngineChunkColumns;
typedef void (*EngineSystemCallback)(const EngineChunkColumns* view, float dt);
```

Error codes: `NULL_ARG -1`, `BAD_ARG -2`, `SIZE_MISMATCH -3`, `NOT_FOUND -4`, `IN_RUN -5`, `INTERNAL -6`.
Access: `ENGINE_ABI_ACCESS_IN 0`, `ENGINE_ABI_ACCESS_INOUT 1`.

### Native implementation

`native/bridge/src/generic_systems.{hpp,cpp}`, class `engine::bridge::GenericSystems`,
owned by `EngineWorldImpl`:

- **Component names are opaque.** Entities are created with `sep = ""` and
  `root_sep = ""` and looked up with `ecs_lookup_path_w_sep(w, 0, name, "", "", false)`,
  so `Game.Mover` is one entity named `Game.Mover`, not `Mover` under a `Game` parent.
- **Re-registration is idempotent and never aborts.** If the name exists and
  its type info matches, the existing id is returned. If it differs, 0 is
  returned without calling `ecs_component_init` (which aborts the process on a
  size mismatch, even in release builds).
- **Systems** own an `ecs_query_t*` with terms `src.id = EcsSelf`,
  `inout = EcsIn` (access 0) or `EcsInOut` (access 1), one value per term,
  `cache_kind = EcsQueryCacheAuto`. Duplicate terms
  and unregistered ids are rejected with `BAD_ARG`.
- **Run** iterates systems in registration order, fills one `EngineChunkColumns`
  per matched table via `ecs_field_w_size`, and calls the callback. While a run
  is active, `entity_create`, `entity_set`, `system_register` and nested
  `systems_run` are refused (`0` or `IN_RUN`).
- `shutdown()` finalizes queries before `ecs_fini` (called first in `ApiWorldDestroy`).

### Managed side

- `tools/abi_codegen` learns the new types and emits `NativeApi.EngineChunkColumns`
  (`Size = 120`), the constants (`MaxTerms`, `AccessIn`, `ErrNullArg`, …) and the
  six entries. All six are plain `Cdecl` (no `SuppressGCTransition`: they call
  back into managed code or take locks).
- `Engine.Runtime.Ecs` wraps the table: `ComponentId<T>` (cached UTF-8 full type
  name, `sizeof(T)`, `Layout.AlignOf<T>()`), `CreateEntity`, `Set<T>`, `Get<T>`,
  `RunSystems`. It throws `InvalidOperationException` on native errors; it is
  setup-path code, not frame-path code.
- `ComponentSystemGenerator` emits, per `IComponent` struct with a `Tick`:
  `ComponentSize`, `Faulted`, `Register(table, world, userData)` and an
  `[UnmanagedCallersOnly(Cdecl)] RunChunk(EngineChunkColumns*, float dt)`.

Tick shape rules (anything else is "unrunnable": `Register` returns -2 and `RunChunk` is a no-op):

1. Component parameters are `ref T` (inout) or `in T` (in), where `T : IComponent`.
2. The first component parameter is the declaring type itself (`ref Self self`).
3. Component types are distinct, at most 8.
4. A `float` parameter, by value or `in`, receives `dt`. `ref float`, `double` and by-value structs are unrunnable.

Fault isolation: the row loop is inside `try/catch (Exception)`; the first
exception sets a static `s_faulted` flag and every later chunk of that type is skipped.

### Verification

- `engine_generic_systems` (native CTest): registration, errors, a 2-term run, in-run refusals.
- `GeneratorDriverTests` (Engine.Tests): runs the real generator through
  `CSharpGeneratorDriver`, asserts 0 errors, and calls `RunChunk` through a function pointer on stack buffers.
- `engine_illusion_sample` (CTest, needs the `dotnet build` fixture):
  `samples/IllusionSample` runs generated systems on native Flecs columns, checks
  values, fault isolation, and 0 managed bytes over 100 frames of 100k rows.

## Decisions log

| # | Decision | Why |
|---|---|---|
| 1 | Append a 4.2 tail, keep the 4.1 fixed-archetype path. | Existing entries keep order and signature, so Benchmark1M and hot-reload code is unchanged; hosts still need regenerated 4.2 bindings because the version check is exact. |
| 2 | Components keyed by the C# full type name, opaque (no path tokenizing). | Deterministic across processes and reloads; `sep=""` avoids Flecs creating `Game` parent entities. |
| 3 | Size mismatch on re-register returns 0, checked before `ecs_component_init`. | Flecs v4.1.6 aborts on `INVALID_COMPONENT_SIZE`, even in release (spike). |
| 4 | View carries 8 fixed column slots (120 B) instead of a pointer to an array. | No allocation per chunk; fixed layout is easy to assert on both sides. |
| 5 | `float dt` passed as a callback argument. | Matches the existing `EcsChunkView` callback shape and fixes the CS1503 bug at the root. |
| 6 | Generic systems run serially in registration order, only inside `systems_run` (not inside the 4.1 `run_systems`). | Parallel dispatch needs conflict analysis; out of scope. |
| 7 | No `SuppressGCTransition` on the new entries. | `systems_run` calls back into managed code; suppressing the transition there is undefined behavior. |
| 8 | Tick exceptions caught per type and latched. | An exception unwinding through a native frame crashes the process. |
| 9 | Generated `Register` takes the `ApiTable*` explicitly. | `ApiLoader` only resolves the bare name `engine_core`; samples and tests load by absolute path. |
| 10 | Columns are not assumed 64-byte aligned. | Spike printed `mod64=32` for a Flecs column; `AGENTS.md`'s `alignas(64)` claim does not hold for user components. |

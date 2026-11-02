# Scripting

Gameplay code is blittable `IComponent` structs plus plain `Tick` methods.
The Roslyn generator turns each `Tick` into a zero-copy chunk runner;
hot reload swaps the assembly while component data stays in Flecs.

## Components: plain data, no references

A component is a `partial struct` implementing `IComponent` with
blittable fields only — floats, `Entity`, `FixedString64`, math types,
other component structs. No `string`, no classes, no arrays: the ENG0002
analyzer rejects them at compile time.

```csharp
// Snippet: a minimal mover component (blittable: two floats).
// Requires: Engine.Runtime (IComponent).
public partial struct Mover : Engine.Runtime.IComponent
{
    public float Speed;
    public float Distance;
}
```

## Systems: Tick methods on the component

Per-entity logic is a `Tick` method with `ref` (read-write) and `in`
(read-only) parameters. Mark it with `[UpdateInPhase]` to choose when it
runs:

```csharp
// Snippet: advance a mover along +X at Speed units per second.
// Requires: Engine.Runtime (IComponent, Phase, UpdateInPhaseAttribute).
public partial struct Walker : Engine.Runtime.IComponent
{
    public float Speed;
    public float X;

    [Engine.Runtime.UpdateInPhase(Engine.Runtime.Phase.Update)]
    public readonly void Tick(ref Walker self, float dt)
    {
        self.X += self.Speed * dt;
    }
}
```

The generator emits the chunk runner next to this code; native calls it
directly per row range with no delegate, no marshalling, no managed
allocation on the frame path. Throwing out of a `Tick` faults only that
entity's row — the frame continues.

## Reading input and time in a Tick

`Input` is a static view, safe to call from systems. It reads the
zero-copy native snapshot (`GetKey`/`GetKeyDown`/`GetAxis`); the frame
delta arrives as the `Tick` parameter `dt` (matching
`IComponent.Tick(ref T self, float dt)` in `IComponent.cs`):

```csharp
// Snippet: torque-style input gather for one frame.
// Requires: Engine.Runtime (Input, Key).
static float GatherDrive(float dt)
{
    float drive = Engine.Runtime.Input.GetAxis("Horizontal");
    if (Engine.Runtime.Input.GetKey(Engine.Runtime.Key.W))
    {
        drive += 1.0f;
    }

    return drive * dt;
}
```

`GetAxis("Horizontal")` returns 1 with D held, −1 with A held, 0 with
both, and the left-stick X value when no key is held. Action maps load
once from `input_actions.json` at startup.

## Hot reload: save and keep playing

`FileSystemWatcher` (debounced 200 ms) rebuilds the game assembly on
save and reloads it through the collectible `ScriptHostManager`: old
context unbinds and unloads (proved by a `WeakReference`), the fresh
load binds the new chunk runners, and every component row stays in
native memory — the scene never resets:

```csharp
// Snippet: the hot-reload generation contract.
// Requires: Engine.Runtime (ScriptHostManager).
int ReloadGeneration(Engine.Runtime.ScriptHostManager host) => host.Generation;
```

Compile errors go to the console and the old assembly keeps running.
Schema evolution migrates rows: same name + same size binds in place, a
size change migrates field-by-field, a hash mismatch skips the table
with a warning instead of crashing.

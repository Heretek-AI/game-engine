# Physics

Rigid bodies live in Jolt (3D) or Box2D (2D); C# components describe
them and queries observe them. Collision events arrive through a
native-owned ring drained once per fixed step — zero managed allocation.

## Bodies and colliders

Attach a body component plus one collider to an entity and the sync
system creates the native body, steps it, and writes transforms back.
Friction lives on the surface material in Box2D v3 (`material.*`, not a
bare `friction` field); contact events are opt-in per shape
(`enableContactEvents`), and bodies spawned exactly resting generate no
begin events — drop them so the first impact registers.

## Raycasts and overlap queries

`Physics` borrows a caller-owned native world (destroy the world only
after the handle) and answers queries against it. The batch form pins
caller spans and fans out over worker lanes with no per-call alloc:

```csharp
// Snippet: ray straight down from a spawn point to find the floor.
// Requires: Engine.Runtime (Physics, Ray, RaycastHit).
bool FindFloor(Engine.Runtime.Physics physics, float x, float y, float z, out Engine.Runtime.RaycastHit hit)
{
    var ray = new Engine.Runtime.Ray(x, y, z, 0.0f, -1.0f, 0.0f);
    return physics.Raycast(in ray, out hit);
}
```

Single `Raycast` returns the closest hit (`IsHit`, point, fraction,
normal); `SphereCast` sweeps a radius along the ray; `OverlapSphere`
fills a caller span with body ids; `RaycastBatch` runs N rays in
parallel. A test that drops a sphere from y=5 lands at y=0.5 ± 0.05
after 2 simulated seconds at 50 Hz; a downward raycast from y=10 hits
the floor at y=0.

## Collision events without garbage

Drain the ring once per fixed step into a caller span and handle
enter/exit pairs. The steady-state path allocates nothing — a 100-frame
drain loop shows `GC.GetAllocatedBytesForCurrentThread()` unchanged:

```csharp
// Snippet: count enter events in a drained 2D contact buffer.
// Requires: Engine.Bindings (NativeApi).
int CountEnters(System.Span<Engine.Bindings.NativeApi.CollisionEvent2D> events)
{
    int enters = 0;
    foreach (var e in events)
    {
        if (e.Kind == 0)
        {
            enters++;
        }
    }

    return enters;
}
```

A sensor box reports `sensor = 1` instead of a contact. Moving a resting
body up 3 units produces exactly one exit; a sphere falling on a floor
produces exactly one enter within 2 s.

## 2D physics

Box2D v3 worlds step at a fixed 60 Hz: create the world with gravity,
add static ground plus dynamic boxes, step 240 times for a 4 s drop.
Three boxes dropped 2 m settle stacked (y = 0.5, 1.5, 2.5 ± 0.01) with
zero residual speed and 4+ begin-touch events across the fall.

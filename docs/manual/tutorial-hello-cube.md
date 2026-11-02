# Tutorial 1: spinning cube (HelloCube)

Build the smallest rendering program from scratch: one camera, one
light, one cube that spins. Estimated time: 20 minutes. You need a
built engine (`dotnet build Engine.slnx -c Release`).

## Step 1 — Create the world and renderer

Borrow a native world, then create a hidden (offscreen) renderer for
it. The renderer is `IDisposable` — destroy it before the world:

```csharp
// Snippet: bind the API table from an absolute library path (HelloCube
// pattern: NativeLibrary load + engine_get_api_table export, ABI-size
// checked by the caller). Returns the table pointer (null = no export).
// Requires: Engine.Bindings (NativeApi), System.Runtime.InteropServices.
unsafe Engine.Bindings.NativeApi.ApiTable* BindTable(string libPath)
{
    var handle = System.Runtime.InteropServices.NativeLibrary.Load(System.IO.Path.GetFullPath(libPath));
    if (!System.Runtime.InteropServices.NativeLibrary.TryGetExport(handle, "engine_get_api_table", out var addr))
    {
        return null;
    }

    return ((delegate* unmanaged[Cdecl]<Engine.Bindings.NativeApi.ApiTable*>)addr)();
}
```

`Renderer.Create` throws `InvalidOperationException("renderer_create")`
when the runner has no Vulkan device; gate on `ENGINE_REQUIRE_GPU=1`
(fail) vs unset (skip) exactly like `HelloCube/Program.cs`.

## Step 2 — Add a spinner component

Gameplay state is a blittable struct; behavior is a `Tick` the generator
turns into a chunk runner. The spinner yaws its entity's `LocalTransform`
in place:

```csharp
// Snippet: yaw integrator shared by the tutorial cube.
// Self-contained math: quaternion yaw step at Speed rad/s.
void YawStep(ref float qx, ref float qy, ref float qz, ref float qw, float speed, float dt)
{
    var half = speed * dt * 0.5f;
    var s = System.MathF.Sin(half);
    var c = System.MathF.Cos(half);
    (qx, qy, qz, qw) = (qx * c + qz * s, qy * c + qw * s, qz * c - qx * s, qw * c - qy * s);
}
```

Full version with the generator attributes lives in
`samples/HelloCube/Components.cs` (`Spinner`: `Speed` field, `Tick`
taking `ref Spinner` + `ref LocalTransform` + `float dt`).

## Step 3 — Spawn camera, light, cube

Resolve component ids by C# full name (`Ecs.ComponentId<T>()` — the
same ids native registered), upload one unit cube (24 verts, 36
indices), spawn the three entities, and propagate once before the first
frame:

```csharp
// Snippet: main-camera descriptor used by every tutorial scene.
// Requires: Engine.Runtime (Camera).
Engine.Runtime.Camera TutorialCamera()
{
    return new Engine.Runtime.Camera
    {
        FovYRad = 60.0f * System.MathF.PI / 180.0f,
        NearZ = 0.1f,
        FarZ = 100.0f,
        IsMain = 1,
    };
}
```

The camera carries a hand-written `WorldTransform` (lookAt basis, eye in
row 3) and NO `LocalTransform` — `propagate()` would overwrite `World`
from `Local`, and `Local`-less entities are roots that keep their rows.
The cube carries `LocalTransform` + `WorldTransform` + `Spinner` +
`MeshRenderer` on the same table so the spinner query matches.

## Step 4 — Run 10 frames and screenshot

```csharp
// Snippet: tutorial frame loop budget (10 frames at 60 Hz).
// Pure math; the loop itself is Ecs.RunSystems + propagate + Frame.
int TutorialFrames() => 10;
```

Expected: `HELLO-CUBE-OK`, exit 0, and a PNG whose golden passes at
±2 channels / 0.1% pixels. If the golden fails, the harness prints the
`.actual.png` and `.diff.png` paths — open the diff (red = different)
before touching the scene.

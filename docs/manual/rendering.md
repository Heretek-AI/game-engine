# Rendering

The renderer draws ECS entities: cameras pick the view, directional
lights shade it, `MeshRenderer` draws it. This page covers the C#
surface; the GPU-driven path (culling, indirect draws) is internal.

## Camera, light, mesh

Register the render components by C# full name (`Ecs.ComponentId<T>()`
resolves the ids the native side registered idempotently), upload one
mesh, spawn the three entities:

```csharp
// Snippet: the camera every 3D scene needs (60-degree, main view).
// Requires: Engine.Runtime (Camera).
Engine.Runtime.Camera MainCamera()
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

Lights carry the direction light *travels* (pointing down into the
scene), plus an intensity. Meshes are uploaded once through
`Renderer.MeshCreate` (interleaved `pos/normal/uv`, 32 bytes per vertex)
and referenced by the returned id:

```csharp
// Snippet: a warm key light pointing down into the scene.
// Requires: Engine.Runtime (DirectionalLight).
Engine.Runtime.DirectionalLight KeyLight()
{
    var light = new Engine.Runtime.DirectionalLight { Intensity = 1.0f };
    unsafe
    {
        var len = System.MathF.Sqrt(0.25f + 0.81f + 0.4225f);
        light.Direction[0] = -0.5f / len;
        light.Direction[1] = -0.9f / len;
        light.Direction[2] = -0.65f / len;
    }

    return light;
}
```

The mesh fragment shader does Lambert lighting
(`max(dot(n, -light_dir), 0) * 0.9 + 0.1`) and sRGB-encodes in-shader
(lavapipe offers no `_SRGB` swapchain format, so the encode cannot live
in the surface).

## Driving frames from C#

`Renderer.Create` borrows the world (destroy the renderer before the
world), `Frame()` pumps SDL events, runs systems, renders and presents,
and returns true when the window was closed:

```csharp
// Snippet: fixed-step frame budget shared by samples and autoplay.
// Pure math: N frames at 60 Hz take N/60 seconds of game time.
double GameSeconds(int frames) => frames / 60.0;
```

After `Frame()`, call `EngineWorldPropagate` so moved `LocalTransform`
rows flow into `WorldTransform` before the next render.

## Screenshots and goldens

`renderer.Screenshot(path)` captures RGBA8 (BGRA-swizzled, always RGBA
on disk) after the frame. Golden comparison allows every channel ±2
with at most 0.1% of pixels differing; on mismatch the harness writes
`<name>.actual.png` and `<name>.diff.png` (red where different) next to
the build dir, and `ENGINE_UPDATE_GOLDENS=1` overwrites the golden:

```csharp
// Snippet: golden file locations for a named capture.
// Pure path math; mirrors native/tests/golden_compare.hpp.
string GoldenPaths(string goldenDir, string buildDir, string name)
{
    var golden = System.IO.Path.Combine(goldenDir, name + ".png");
    var actual = System.IO.Path.Combine(buildDir, "golden_out", name + ".actual.png");
    return $"{golden} vs {actual}";
}
```

## 2D and UI rendering

Sprites batch by atlas (`SpriteRenderer2D`: atlas id, sorting layer and
order, UV rect, tint) under an `OrthoCamera` (height, aspect, optional
pixel-perfect). One draw call per atlas covers 10k sprites. Game UI
renders RmlUi documents through the engine (`UIDocument.Load`, element
lookup, text set, click events) — see the P5-04 HUD golden.

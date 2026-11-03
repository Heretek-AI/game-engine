// SceneViewportTest.cs — P3-02: Scene viewport acceptance (CPU copy).
//
// Headless golden: CPU-rasterized grid (P1-08 stand-in — the native
// ForwardRenderer does not exist in-tree) into a WriteableBitmap, compared
// with Golden.Check (channel ±2, ≤0.1% pixels). Fly assertion: injected
// RMB+W for 10 ticks moves the camera forward.
// Usage: Engine.EditorApp.Tests <out-dir> (runs shell checks + viewport).
using System.Numerics;
using Avalonia.Media.Imaging;
using Engine.Editor;

namespace Engine.EditorApp.Tests;

internal static class SceneViewportTest
{
    public static int Run(string outDir, string goldenDir)
    {
        var failures = 0;

        // --- camera unit checks (no native) --------------------------------
        var cam = EditorCamera.WithDefault();
        failures += Check(cam.Position is { X: 2, Y: 2, Z: 3 }, "viewport: default pose is (2,2,3)");
        var fwd = cam.Forward;
        failures += Check(Vector3.Dot(fwd, Vector3.Normalize(-cam.Position)) > 0.999f, "viewport: default looks at origin");
        var vp = cam.GetViewProjection(1280, 720);
        failures += Check(vp.Length == 16, "viewport: viewprojection is 16 floats");

        // Dolly + frame smoke.
        var dolly = EditorCamera.WithDefault();
        var d0 = dolly.DistanceTo(Vector3.Zero);
        dolly.Dolly(1.0f, d0);
        failures += Check(dolly.DistanceTo(Vector3.Zero) < d0, "viewport: dolly moves toward target");
        var frame = EditorCamera.WithDefault();
        frame.FrameTo([new Vector3(5, 0, 0), new Vector3(7, 0, 0)]);
        failures += Check(frame.DistanceTo(new Vector3(6, 0, 0)) <= 6.0f, "viewport: frame centers selection");

        // --- fly assertion: injected RMB+W for 10 ticks --------------------
        // Simulates right-mouse held + W down: 10 Fly(fwd) ticks at 1/60s.
        var fly = EditorCamera.WithDefault();
        var startDist = fly.DistanceTo(Vector3.Zero);
        for (var i = 0; i < 10; i++)
        {
            fly.Fly(fwd: true, back: false, left: false, right: false, up: false, down: false, dt: 1.0f / 60.0f);
        }

        var endDist = fly.DistanceTo(Vector3.Zero);
        failures += Check(endDist < startDist, "editor: fly moves camera");

        // --- headless golden: grid + entities ------------------------------
        var scene = EditorCamera.WithDefault();
        var viewport = new SceneViewport(scene, 1280, 720);
        var points = new List<Vector3>
        {
            new(0, 0.5f, 0),
            new(2, 0.5f, 1),
            new(-2, 0.5f, -1),
        };
        var pixels = new byte[1280 * 720 * 4];
        viewport.RenderPoints(points, pixels);

        // Sanity: something other than background got drawn.
        var nonBg = 0;
        for (var i = 0; i < pixels.Length; i += 4)
        {
            if (pixels[i] != SceneViewport.Background[0]
                || pixels[i + 1] != SceneViewport.Background[1]
                || pixels[i + 2] != SceneViewport.Background[2])
            {
                nonBg++;
            }
        }

        failures += Check(nonBg > 1000, "viewport: grid + markers rasterize");

        // Wrap the CPU buffer in a WriteableBitmap for the golden compare.
        using var bmp = PixelsToBitmap(pixels, 1280, 720);
        var shotPath = Path.Combine(outDir, "editor_scene_grid_1280x720.png");
        bmp.Save(shotPath, Avalonia.Media.Imaging.PngBitmapEncoderOptions.Default);
        Console.WriteLine($"[PASS] viewport: screenshot saved to {shotPath}");

        failures += Golden.Check("editor_scene_grid_1280x720", bmp, goldenDir);
        Console.WriteLine($"[info] viewport: entities drawn = {viewport.LastEntityCount}");
        return failures;
    }

    private static WriteableBitmap PixelsToBitmap(byte[] rgba, int width, int height)
    {
        var bmp = new WriteableBitmap(
            new Avalonia.PixelSize(width, height),
            new Avalonia.Vector(96, 96),
            Avalonia.Platform.PixelFormat.Rgba8888,
            Avalonia.Platform.AlphaFormat.Premul);
        using var locked = bmp.Lock();
        unsafe
        {
            fixed (byte* src = rgba)
            {
                Buffer.MemoryCopy(src, (void*)locked.Address, rgba.Length, rgba.Length);
            }
        }

        return bmp;
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }
}

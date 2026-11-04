// ViewportPanelGolden.cs — P3-13: headless SceneViewportView acceptance.
//
// Runs INSIDE the shell window session (one Avalonia Setup per process):
//   1. ENGINE_VIEWPORT_GPU=0 + SDL_VIDEO_DRIVER=offscreen force the CPU
//      fallback; ResolveInterop() returns null (no probe, no throw).
//   2. A headless SceneViewportView reports CpuReadback and never engages
//      zero-copy (no compositor in this session).
//   3. PresentFrame() on a presenter-less view returns false and does not
//      attach a bitmap (no presenter, no CPU bytes available).
//
// The control is NOT added to the shell window, so the existing
// editor_shell_1280x720 golden is untouched (no regression to the CPU path).
using Avalonia.Controls;
using Engine.EditorApp;

namespace Engine.EditorApp.Tests;

internal static class ViewportPanelGolden
{
    public static int Check(Window window)
    {
        var failures = 0;

        var prevGpu = Environment.GetEnvironmentVariable(SceneViewportView.ForceFallbackEnvVar);
        var prevSdl = Environment.GetEnvironmentVariable("SDL_VIDEO_DRIVER");
        Environment.SetEnvironmentVariable(SceneViewportView.ForceFallbackEnvVar, "0");
        Environment.SetEnvironmentVariable("SDL_VIDEO_DRIVER", "offscreen");
        try
        {
            failures += CheckOk(SceneViewportView.IsFallbackForced, "viewport-panel: ENGINE_VIEWPORT_GPU=0 forces fallback");

            using var view = new SceneViewportView();
            failures += CheckOk(view.ResolveInterop() is null, "viewport-panel: headless resolves null interop (CPU path)");
            failures += CheckOk(!view.PresentFrame(), "viewport-panel: presenter-less frame stays on CPU");
            failures += CheckOk(!view.CpuAttachedForTest, "viewport-panel: no bitmap without a presenter");
            failures += CheckOk(
                view.LastMode == Engine.Editor.ViewportPresentMode.CpuReadback,
                "viewport-panel: headless view reports CpuReadback");
            failures += CheckOk(!view.ZeroCopyEngaged, "viewport-panel: headless view never zero-copy");
            failures += CheckOk(view.Name == "Scene", "viewport-panel: Scene name preserved for panel lookup");
        }
        finally
        {
            Environment.SetEnvironmentVariable(SceneViewportView.ForceFallbackEnvVar, prevGpu);
            Environment.SetEnvironmentVariable("SDL_VIDEO_DRIVER", prevSdl);
        }

        return failures;
    }

    private static int CheckOk(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }
}

// Program.cs — P3-01: editor shell acceptance: menu titles, panel names,
// window size, title, golden screenshot. Prints [PASS] lines + the golden
// line, exits 0 only when all pass. Usage: Engine.EditorApp.Tests <out-dir>.
using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Media.Imaging;
using Avalonia.VisualTree;
using Engine.EditorApp;

namespace Engine.EditorApp.Tests;

internal static class Program
{
    public static int Main(string[] args)
    {
        var failures = 0;
        failures += Check(MainWindow.MenuTitles.Length == 5, "shell: five menus");
        failures += Check(MainWindow.MenuTitles[0] == "File", "shell: menu File first");
        failures += Check(MainWindow.MenuTitles[2] == "GameObject", "shell: menu GameObject present");
        failures += Check(MainWindow.MenuTitles[4] == "Help", "shell: menu Help last");
        failures += Check(MainWindow.PanelNames.Length == 7, "shell: seven panels (P3-09 adds Profiler)");
        failures += Check(MainWindow.PanelNames.Contains("Console"), "shell: console panel present");
        failures += Check(MainWindow.PanelNames.Contains("Profiler"), "shell: profiler panel present");

        var builder = AppBuilder.Configure<Engine.EditorApp.App>()
            .UseSkia()
            .UseHeadless(new AvaloniaHeadlessPlatformOptions { UseHeadlessDrawing = false })
            .LogToTrace();
        builder.SetupWithoutStarting();

        var window = new MainWindow();
        window.Show();
        failures += Check(window.Title == "Engine Editor", "shell: window title");
        failures += Check(window.ClientSize is { Width: 1280, Height: 720 }, "shell: default size 1280x720");
        var names = CollectNames(window);
        var missing = MainWindow.PanelNames.Where(p => !names.Contains(p)).ToArray();
        failures += Check(missing.Length == 0, "shell: all panels in visual tree");

        var outDir = args.Length > 0 ? args[0] : Path.GetTempPath();
        Directory.CreateDirectory(outDir);
        var frame = window.CaptureRenderedFrame();
        if (frame is null)
        {
            Console.WriteLine("[FAIL] golden: editor_shell_1280x720 (no frame captured)");
            return 1;
        }

        var shotPath = Path.Combine(outDir, "editor_shell_1280x720.png");
        frame.Save(shotPath, PngBitmapEncoderOptions.Default);
        Console.WriteLine($"[PASS] shell: screenshot saved to {shotPath}");

        // The golden was rendered on Linux. Font smoothing differs per OS
        // (~1% of pixels on macOS/Windows), so only Linux compares pixels;
        // the structural checks above run everywhere.
        if (!OperatingSystem.IsLinux())
        {
            Console.WriteLine("[SKIP] golden: editor_shell_1280x720 (Linux-rendered golden; font rasterization differs on this OS)");
            return failures;
        }

        var goldenDir = GoldenDir();
        failures += Golden.Check("editor_shell_1280x720", frame, goldenDir);
        return failures;
    }

    private static HashSet<string> CollectNames(Visual visual)
    {
        var names = new HashSet<string>(StringComparer.Ordinal);
        foreach (var descendant in visual.GetVisualDescendants())
        {
            if (descendant is Control control && !string.IsNullOrEmpty(control.Name))
            {
                names.Add(control.Name);
            }
        }

        return names;
    }

    private static string GoldenDir()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir is not null)
        {
            var candidate = Path.Combine(dir.FullName, "managed", "Engine.EditorApp.Tests", "golden");
            if (Directory.Exists(candidate))
            {
                return candidate;
            }

            dir = dir.Parent;
        }

        return Path.Combine(AppContext.BaseDirectory, "golden");
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }
}

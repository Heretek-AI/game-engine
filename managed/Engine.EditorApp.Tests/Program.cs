// Program.cs — P3-12: editor shell acceptance: menu titles, dock model,
// window size, title, golden screenshot, layout save/restore round-trip.
// Prints [PASS] lines + the golden line, exits 0 only when all pass.
// Usage: Engine.EditorApp.Tests <out-dir>.
using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Media.Imaging;
using Avalonia.LogicalTree;
using Avalonia.Threading;
using Avalonia.VisualTree;
using Dock.Model.Controls;
using Dock.Model.Core;
using Engine.EditorApp;
using System.Text.Json;

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
        Dispatcher.UIThread.RunJobs();
        failures += Check(window.Title == "Engine Editor", "shell: window title");
        failures += Check(window.ClientSize is { Width: 1280, Height: 720 }, "shell: default size 1280x720");

        // The dock hosts panels inside tab strips: inactive tabs (Game, Console,
        // Profiler) are not in the visual tree. Assert on the dock model instead:
        // every panel id exists as a tool, and the default arrangement matches
        // P3-01 (Hierarchy left, Scene+Game center, Inspector right,
        // Project+Console+Profiler bottom).
        failures += CheckDockModel(window);
        failures += Check(DockHasResetMenu(window), "shell: Window > Reset Layout present");
        // Round-trip mutates a layout (moves Console), so it runs on a
        // separate window — the golden capture below keeps the default
        // arrangement.
        var roundTripWindow = new MainWindow();
        roundTripWindow.Show();
        Dispatcher.UIThread.RunJobs();
        failures += CheckLayoutRoundTrip(roundTripWindow);
        roundTripWindow.Close();
        failures += CheckProjectOpenRestore();

        var outDir = args.Length > 0 ? args[0] : Path.GetTempPath();
        Directory.CreateDirectory(outDir);
        // Pump the dispatcher once more so dock tab switches settle before capture.
        Dispatcher.UIThread.RunJobs();
        var frame = window.CaptureRenderedFrame();
        if (frame is null)
        {
            Console.WriteLine("[FAIL] golden: editor_shell_1280x720 (no frame captured)");
            return 1;
        }

        var shotPath = Path.Combine(outDir, "editor_shell_1280x720.png");
        frame.Save(shotPath, PngBitmapEncoderOptions.Default);
        Console.WriteLine($"[PASS] shell: screenshot saved to {shotPath}");

        // The goldens were rendered on Linux. Font smoothing differs per OS
        // (~1% of pixels on macOS/Windows), so only Linux compares pixels;
        // the structural checks run everywhere.
        var comparePixels = OperatingSystem.IsLinux();
        var goldenDir = GoldenDir();
        if (comparePixels)
        {
            failures += Golden.Check("editor_shell_1280x720", frame, goldenDir);
        }
        else
        {
            Console.WriteLine("[SKIP] golden: editor_shell_1280x720 (Linux-rendered golden; font rasterization differs on this OS)");
        }

        // P3-03: hierarchy panel checks + golden (3 nested entities) —
        // captured from the SAME window session (Avalonia allows one Setup
        // per process, so the panel renders into the shell window).
        failures += HierarchyPanelGolden.Check(window, outDir, goldenDir, comparePixels);
        // P3-05: inspector panel headless acceptance (2.5+Enter, Ctrl+Z, LM golden).
        failures += InspectorPanelGolden.Check(window, outDir, goldenDir);
        // P3-13: viewport control headless checks (GPU toggle + CPU fallback).
        // Runs in the same session; the control is never added to the shell
        // window, so the editor_shell_1280x720 golden is untouched.
        failures += ViewportPanelGolden.Check(window);
        window.Close();
        // EXIT-CODE CONTRACT (P3 gate): print a stable summary line and exit
        // non-zero on ANY failure; the CI step greps for "FAILED" on failure.
        Console.WriteLine(failures == 0
            ? "editor-shell: all checks passed"
            : $"editor-shell: {failures} FAILED");
        return failures == 0 ? 0 : 1;
    }

    private static int CheckDockModel(MainWindow window)
    {
        var failures = 0;
        var tools = AllTools(window.Layout).ToDictionary(t => t.Id, StringComparer.Ordinal);
        var missing = MainWindow.PanelNames.Where(p => !tools.ContainsKey(p)).ToArray();
        failures += Check(missing.Length == 0,
            missing.Length == 0
                ? "shell: all panels in dock model"
                : $"shell: all panels in dock model (missing {string.Join(",", missing)})");
        failures += Check(TabGroupMembers(window.Layout, "Center"),
            "shell: Scene+Game tabbed in center");
        failures += Check(TabGroupMembers(window.Layout, "Bottom"),
            "shell: Project+Console+Profiler tabbed along bottom");

        string VisibleText(string id) => window.Layouts.Factory.Panels.TryGetValue(id, out var panel)
            ? panel.Name ?? string.Empty
            : string.Empty;
        failures += Check(VisibleText("Console") == "Console", "shell: console panel present");
        failures += Check(VisibleText("Profiler") == "Profiler", "shell: profiler panel present");
        return failures;
    }

    private static bool TabGroupMembers(IRootDock layout, string dockId)
    {
        var dock = Find<IDock>(layout, dockId);
        if (dock?.VisibleDockables is null)
        {
            return false;
        }

        var ids = dock.VisibleDockables.OfType<ITool>().Select(t => t.Id).ToHashSet(StringComparer.Ordinal);
        return dockId switch
        {
            "Center" => ids.SetEquals(["Scene", "Game"]),
            "Bottom" => ids.SetEquals(["Project", "Console", "Profiler"]),
            _ => false,
        };
    }

    private static T? Find<T>(IDockable node, string id)
        where T : class, IDockable
    {
        if (node is T match && node.Id == id)
        {
            return match;
        }

        if (node is IDock dock)
        {
            foreach (var child in dock.VisibleDockables ?? [])
            {
                var found = Find<T>(child, id);
                if (found is not null)
                {
                    return found;
                }
            }
        }

        return null;
    }

    private static IEnumerable<ITool> AllTools(IDockable node)
    {
        if (node is ITool tool)
        {
            yield return tool;
        }

        if (node is IDock dock)
        {
            foreach (var child in dock.VisibleDockables ?? [])
            {
                foreach (var nested in AllTools(child))
                {
                    yield return nested;
                }
            }
        }
    }

    private static bool DockHasResetMenu(MainWindow window)
    {
        foreach (var descendant in window.GetLogicalDescendants())
        {
            if (descendant is MenuItem mi
                && (string?)mi.Header == "Reset Layout"
                && mi.Name == "ResetLayout")
            {
                return true;
            }
        }

        return false;
    }

    /// <summary>
    /// P3-12 acceptance: move Console into the Inspector's tab group, save,
    /// restore, and compare the serialized JSON for identity.
    /// </summary>
    private static int CheckLayoutRoundTrip(MainWindow window)
    {
        var failures = 0;
        var layouts = window.Layouts;
        var moved = layouts.Layout;
        var console = Find<ITool>(moved, EditorDockFactory.ConsoleId);
        var right = Find<IDock>(moved, "Right");
        if (console is null || right is null)
        {
            Console.WriteLine("[FAIL] layout: Console tool or Right dock missing");
            return 1;
        }

        // Move Console into the Inspector's tab group (Right dock), then save.
        var factory = (EditorDockFactory)moved.Factory!;
        var previousOwner = console.Owner as IDock;
        previousOwner?.VisibleDockables?.Remove(console);
        factory.AddDockable(right, console);
        factory.SetActiveDockable(console);

        var movedOwners = OwnersOf(moved);
        var tmpDir = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        layouts.Save(tmpDir);
        var savedJson = File.ReadAllText(EditorLayoutService.LayoutPathFor(tmpDir));
        failures += Check(savedJson.Contains("\"Console\""), "layout: saved JSON contains Console");

        var restored = layouts.Deserialize(savedJson);
        var again = layouts.Serialize();

        failures += Check(SameArrangement(movedOwners, OwnersOf(restored)), "layout: moved Console restores identically");
        failures += Check(CanonicalJsonEquals(savedJson, again), "layout: serialized JSON round-trip identical");
        failures += Check(ConsoleIn(restored, "Right"),
            "layout: restored Console sits in the Inspector tab group");

        // Window > Reset Layout restores the P3-01 default (Console back bottom).
        layouts.ResetLayout();
        failures += Check(ConsoleIn(layouts.Layout, "Bottom"),
            "layout: Reset Layout restores Console to bottom");

        // P3-12 Save/Open contract: Open restores from <project>/.editor/layout.json.
        var reopened = layouts.Open(tmpDir);
        failures += Check(reopened, "layout: Open restores saved layout");
        failures += Check(ConsoleIn(layouts.Layout, "Right"),
            "layout: Open puts Console back in the Inspector tab group");
        return failures;
    }

    /// <summary>
    /// P3-12 app wiring: a MainWindow opened on a project dir restores the
    /// saved layout (instead of always building the default), and a window
    /// opened without one keeps the P3-01 default.
    /// </summary>
    private static int CheckProjectOpenRestore()
    {
        var failures = 0;
        var tmpDir = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());

        // Seed: default layouts, move Console to the Inspector tab, save.
        var seed = new MainWindow();
        var moved = seed.Layouts.Layout;
        var console = Find<ITool>(moved, EditorDockFactory.ConsoleId);
        var right = Find<IDock>(moved, "Right");
        if (console is null || right is null)
        {
            Console.WriteLine("[FAIL] app-open: seed layout missing Console/Right");
            return 1;
        }

        var previousOwner = console.Owner as IDock;
        previousOwner?.VisibleDockables?.Remove(console);
        ((EditorDockFactory)moved.Factory!).AddDockable(right, console);
        seed.Layouts.Save(tmpDir);

        // Open on the project dir: the window must show the SAVED
        // arrangement (Console in Right), not the default (Console bottom).
        var window = new MainWindow(tmpDir);
        window.Show();
        Dispatcher.UIThread.RunJobs();
        failures += Check(ConsoleIn(window.Layout, "Right"),
            "app-open: project window restores saved Console tab");
        window.Close();

        // No project dir: default arrangement stands.
        var plain = new MainWindow();
        failures += Check(ConsoleIn(plain.Layout, "Bottom"),
            "app-open: default window keeps Console at bottom");
        plain.Close();
        return failures;
    }

    /// <summary>
    /// Console-in-dock assertion that fails (not vacuous-passes) when the
    /// Console tool or the dock is missing: null == null would otherwise
    /// report [PASS] on a broken restore that dropped the dock subtree.
    /// </summary>
    private static bool ConsoleIn(IRootDock layout, string dockId)
    {
        var owner = Find<ITool>(layout, EditorDockFactory.ConsoleId)?.Owner;
        return owner is not null && owner == Find<IDock>(layout, dockId);
    }

    /// <summary>Map each panel id to its owning dock on a live layout.</summary>
    private static Dictionary<string, string?> OwnersOf(IRootDock root)
    {
        var map = new Dictionary<string, string?>(StringComparer.Ordinal);
        void Walk(IDockable node, string? owner)
        {
            var id = node.Id;
            if (node is IDock dock)
            {
                foreach (var child in dock.VisibleDockables ?? [])
                {
                    if (MainWindow.PanelNames.Contains(child.Id))
                    {
                        map[child.Id] = id;
                    }

                    Walk(child, id);
                }
            }
            else if (MainWindow.PanelNames.Contains(id))
            {
                map.TryAdd(id, owner);
            }
        }

        Walk(root, null);
        return map;
    }

    /// <summary>Compare two panel-to-dock arrangement maps for identity.</summary>
    private static bool SameArrangement(
        Dictionary<string, string?> a, Dictionary<string, string?> b)
    {
        if (a.Count != b.Count)
        {
            Console.WriteLine($"[INFO] arrangement panel counts differ: {a.Count} vs {b.Count}");
            return false;
        }

        foreach (var name in MainWindow.PanelNames)
        {
            var hasA = a.TryGetValue(name, out var oa);
            var hasB = b.TryGetValue(name, out var ob);
            if (!hasA || !hasB || oa != ob)
            {
                Console.WriteLine($"[INFO] arrangement differs at {name}: {(hasA ? oa : "<missing>")} vs {(hasB ? ob : "<missing>")}");
                return false;
            }
        }

        return true;
    }

    /// <summary>
    /// Canonical JSON identity: parse both documents and compare with object
    /// keys sorted (arrays keep order — tab/dock order is arrangement).
    /// String comparison is brittle (serializer key order), so identity is
    /// decided on the canonical form. Prints the first divergence path.
    /// Two normalizations, both documented:
    /// - $id/$ref serializer metadata is resolved through each node's
    ///   stable Id: $refs are emitted as the target node's Id string and
    ///   $id numbering is dropped (deserialization assigns fresh ids; the
    ///   reference GRAPH is what must match, not its numbering).
    /// - IsActive is dropped: it is transient runtime focus state, not
    ///   persisted arrangement (the app never sets it deterministically).
    /// </summary>
    private static bool CanonicalJsonEquals(string a, string b)
    {
        using var da = JsonDocument.Parse(a);
        using var db = JsonDocument.Parse(b);
        var sa = Canonical(da.RootElement, RefTargets(da.RootElement));
        var sb = Canonical(db.RootElement, RefTargets(db.RootElement));
        if (sa != sb)
        {
            var common = 0;
            while (common < sa.Length && common < sb.Length && sa[common] == sb[common])
            {
                common++;
            }

            Console.WriteLine($"[INFO] canonical JSON differs at char {common} (lens {sa.Length}/{sb.Length}):");
            Console.WriteLine($"[INFO]   left : ...{sa.Substring(Math.Max(0, common - 60), Math.Min(60, common))}>>> {sa.Substring(common, Math.Min(120, sa.Length - common))}");
            Console.WriteLine($"[INFO]   right: ...{sb.Substring(Math.Max(0, common - 60), Math.Min(60, common))}>>> {sb.Substring(common, Math.Min(120, sb.Length - common))}");
            return false;
        }

        return true;
    }

    private static Dictionary<string, string> RefTargets(JsonElement root)
    {
        var map = new Dictionary<string, string>(StringComparer.Ordinal);
        void Walk(JsonElement el)
        {
            if (el.ValueKind == JsonValueKind.Object)
            {
                string? id = null;
                string? nodeId = null;
                foreach (var p in el.EnumerateObject())
                {
                    if (p.NameEquals("$id"))
                    {
                        id = p.Value.GetRawText();
                    }
                    else if (p.NameEquals("Id") && p.Value.ValueKind == JsonValueKind.String)
                    {
                        nodeId = p.Value.GetString();
                    }
                    else
                    {
                        Walk(p.Value);
                    }
                }

                if (id != null && nodeId != null && !map.ContainsKey(id))
                {
                    map[id] = nodeId;
                }
            }
            else if (el.ValueKind == JsonValueKind.Array)
            {
                foreach (var item in el.EnumerateArray())
                {
                    Walk(item);
                }
            }
        }

        Walk(root);
        return map;
    }

    private static string Canonical(JsonElement el, Dictionary<string, string> refs)
    {
        switch (el.ValueKind)
        {
            case JsonValueKind.Object:
                var parts = new List<string>();
                foreach (var p in el.EnumerateObject().OrderBy(p => p.Name, StringComparer.Ordinal))
                {
                    if (p.NameEquals("IsActive"))
                    {
                        continue;
                    }

                    if (p.NameEquals("$id"))
                    {
                        continue;
                    }

                    if (p.NameEquals("$ref"))
                    {
                        var key = p.Value.GetRawText();
                        var target = refs.TryGetValue(key, out var t) ? t : "?unresolved:" + key;
                        parts.Add("\"$ref\":" + JsonEncodedText.Encode(target).ToString());
                        continue;
                    }

                    parts.Add(JsonEncodedText.Encode(p.Name).ToString() + ":" + Canonical(p.Value, refs));
                }

                return "{" + string.Join(",", parts) + "}";
            case JsonValueKind.Array:
                return "[" + string.Join(",", el.EnumerateArray().Select(e => Canonical(e, refs))) + "]";
            case JsonValueKind.String:
                return JsonEncodedText.Encode(el.GetString()!).ToString();
            case JsonValueKind.Number:
                return el.GetRawText();
            case JsonValueKind.True:
                return "true";
            case JsonValueKind.False:
                return "false";
            default:
                return "null";
        }
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

// InspectorPanelGolden.cs — P3-05: headless Inspector panel acceptance.
//
// Inside the shell window session (one Avalonia Setup per process):
// spawns an entity with LocalTransform + MeshRenderer, binds the
// InspectorPanel, types 2.5 + Enter into LocalTransform.position.x
// (asserts the value through EditorSession.GetLocal), sends Ctrl+Z
// (asserts restore), then renders the inspector golden (LocalTransform
// + MeshRenderer sections). Needs the native engine_core beside the
// test binary (same-dir resolution).
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Input;
using Avalonia.Media.Imaging;
using Avalonia.LogicalTree;
using Avalonia.VisualTree;
using Engine.Bindings;
using Engine.Editor;
using Engine.Runtime;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Engine.EditorApp.Tests;

internal static class InspectorPanelGolden
{
    public static unsafe int Check(Avalonia.Controls.Window window, string outDir, string goldenDir)
    {
        var failures = 0;

        var dir = AppContext.BaseDirectory;
        var loaded = false;
        string? lastError = null;
        IntPtr preloaded = IntPtr.Zero;
        foreach (var cand in new[] { Path.Combine(dir, "libengine_core.so"), Path.Combine(dir, "libengine_core.so.0.1.0") })
        {
            try
            {
                var flecs = Path.Combine(dir, "libflecs.so");
                if (File.Exists(flecs))
                {
                    try { NativeLibrary.Load(flecs); } catch { }
                }

                preloaded = NativeLibrary.Load(cand);
                loaded = true;
                break;
            }
            catch (Exception ex)
            {
                lastError = ex.GetType().Name + ": " + ex.Message;
            }
        }

        if (!loaded)
        {
            Console.WriteLine($"[SKIP] inspector-panel live (no native lib: {lastError})");
            return failures;
        }

        try
        {
            LoadTableFromHandle(preloaded);
        }
        catch (Exception ex) when (ex is DllNotFoundException || ex is EntryPointNotFoundException || ex is InvalidOperationException)
        {
            Console.WriteLine($"[SKIP] inspector-panel live (no native lib: {ex.GetType().Name})");
            return failures;
        }

        var table = EditorSession.Table;
        NativeApi.EngineWorld* world = table->EngineWorldCreate();
        failures += CheckOk(world != null, "inspector-panel: world_create");
        if (world == null)
        {
            return failures;
        }

        try
        {
            using var session = new EditorSession(world);
            var inspector = new Inspector(session);

            var entity = session.Spawn("Cube");
            failures += CheckOk(entity != 0, "inspector-panel: spawn Cube");
            session.EnsureLocal(entity);

            // MeshRenderer row: zeroed 20B image through AddComponentCommand.
            inspector.Execute(new AddComponentCommand(session, entity, "Engine.Runtime.MeshRenderer", new byte[20]));
            failures += CheckOk(session.HasComponent(entity, "Engine.Runtime.MeshRenderer", 20), "inspector-panel: MeshRenderer added");

            // Panel bound to the live session, hosted in its own window so
            // the golden frames the inspector (the shell window keeps its
            // own Content for its golden; one Setup per process still holds
            // because both windows share the same Application).
            var panel = new InspectorPanel(inspector);
            var panelWindow = new Window
            {
                Title = "Inspector",
                Width = 1280,
                Height = 720,
                Content = panel,
            };
            panelWindow.Show();
            panel.Select(entity, session);
            var undoBaseline = inspector.UndoCount;
            panel.UpdateLayout();
            Avalonia.Threading.Dispatcher.UIThread.RunJobs();
            panelWindow.UpdateLayout();
            Avalonia.Threading.Dispatcher.UIThread.RunJobs();

            // position.x editor exists and starts at 0 (scoped to the panel window).
            var posX = FindEditor(panel, "position", "");
            failures += CheckOk(posX != null, "inspector-panel: position.x editor present");

            // Type 2.5 + Enter: one undo step (LostFocus/Enter semantics).
            // Headless note: KeyPress(Enter) on the window does not route
            // into the box's KeyDown handler, and moving focus headless
            // does not raise LostFocus either. Focus the box first (real
            // path: the user typed into the focused editor), then raise the
            // box's own KeyDown with Enter so the panel commits exactly one
            // undo step through SetFieldCommand.
            if (posX != null)
            {
                posX.Focus();
                Avalonia.Threading.Dispatcher.UIThread.RunJobs();
                posX.Text = "2.5";
                Avalonia.Threading.Dispatcher.UIThread.RunJobs();
                posX.RaiseEvent(new KeyEventArgs
                {
                    RoutedEvent = InputElement.KeyDownEvent,
                    Key = Key.Enter,
                });
                Avalonia.Threading.Dispatcher.UIThread.RunJobs();
                panel.Refresh(session);
            }


            var pose = new float[10];
            session.GetLocal(entity, pose);
            failures += CheckOk(MathF.Abs(pose[0] - 2.5f) < 1e-4f, "inspector-panel: typing 2.5+Enter sets position.x");
            failures += CheckOk(inspector.UndoCount == undoBaseline + 1, "inspector-panel: one undo step committed");

            // Ctrl+Z restores the old value (same shared stack the docked
            // UI's Ctrl+Z binding drives).
            failures += CheckOk(inspector.Undo(), "inspector-panel: Ctrl+Z succeeds");
            panel.Refresh(session);
            session.GetLocal(entity, pose);
            failures += CheckOk(MathF.Abs(pose[0]) < 1e-4f, "inspector-panel: Ctrl+Z restores position.x");

            // Redo for the golden (shows the edited value path is intact).
            _ = inspector.Redo();
            panel.Refresh(session);

            // Component sections: LocalTransform + MeshRenderer expanders.
            failures += CheckOk(HasSection(panel, "LocalTransform"), "inspector-panel: LocalTransform section present");
            failures += CheckOk(HasSection(panel, "Engine.Runtime.MeshRenderer"), "inspector-panel: MeshRenderer section present");

            // Golden: inspector showing LocalTransform + MeshRenderer.
            Directory.CreateDirectory(outDir);
            var frame = panelWindow.CaptureRenderedFrame();
            if (frame is null)
            {
                Console.WriteLine("[FAIL] golden: editor_inspector_lm (no frame captured)");
                return failures + 1;
            }

            var shotPath = Path.Combine(outDir, "editor_inspector_lm.png");
            frame.Save(shotPath, PngBitmapEncoderOptions.Default);
            Console.WriteLine($"[PASS] inspector-panel: screenshot saved to {shotPath}");
            failures += Golden.Check("editor_inspector_lm", frame, goldenDir);
            panelWindow.Close();
        }
        finally
        {
            table->EngineWorldDestroy(world);
        }

        return failures;
    }

    private static TextBox? FindEditor(InspectorPanel panel, string field, string _lane)
    {
        foreach (var descendant in panel.GetLogicalDescendants())
        {
            if (descendant is TextBox box && box.Name == $"Edit_{field}")
            {
                return box;
            }
        }

        return null;
    }

    private static bool HasSection(InspectorPanel panel, string component)
    {
        foreach (var descendant in panel.GetLogicalDescendants())
        {
            if (descendant is Expander exp && exp.Name == $"Component_{component}")
            {
                return true;
            }
        }

        return false;
    }

    private static int CheckOk(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }

    private static unsafe void LoadTableFromHandle(IntPtr handle)
    {
        if (!NativeLibrary.TryGetExport(handle, "engine_get_api_table", out var addr))
        {
            throw new EntryPointNotFoundException("engine_get_api_table not exported.");
        }

        var getTable = (delegate* unmanaged[Cdecl, SuppressGCTransition]<NativeApi.ApiTable*>)addr;
        var table = getTable();
        if (table == null)
        {
            throw new InvalidOperationException("engine_get_api_table returned null.");
        }

        EditorSession.TableOverride = (IntPtr)table;
    }
}

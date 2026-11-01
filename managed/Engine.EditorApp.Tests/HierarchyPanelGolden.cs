// HierarchyPanelGolden.cs — P3-03: headless Hierarchy panel acceptance.
//
// Runs INSIDE the shell window session (one Avalonia Setup per process):
// builds a 3-entity nested scene (A -> B -> C) over a LIVE native session,
// drives selection, rename, drag-drop reparent + Ctrl+Z through the panel,
// then renders the panel golden (3 nested entities). Needs the native
// engine_core beside the test binary (same-dir resolution).
//
// Rename (F2 + Enter), drag-drop (window.DragDrop onto A's row) and
// Ctrl+Z/Ctrl+Y all go through the panel's own input handlers.
using Avalonia;
using Avalonia.Controls;
using Avalonia.Headless;
using Avalonia.Input;
using Avalonia.Input.Raw;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.VisualTree;
using Engine.Bindings;
using Engine.Editor;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Engine.EditorApp.Tests;

internal static class HierarchyPanelGolden
{
    public static unsafe int Check(Window window, string outDir, string goldenDir, bool comparePixels)
    {
        var failures = 0;

        // Same-dir native preload (compositor_live precedent): pin
        // libflecs + libengine_core by absolute path first. NOTE: .NET's
        // NativeLibrary probing ignores LD_LIBRARY_PATH for bare names, so
        // try the bare load too — one of the two paths resolves.
        var dir = AppContext.BaseDirectory;
        var loaded = false;
        string? lastError = null;
        IntPtr preloaded = IntPtr.Zero;
        // Cover macOS (.dylib), Windows (.dll), Linux (.so) — same candidate
        // list as CompositorApiTests (Engine.Tests), which stages the same libs.
        foreach (var cand in new[] { Path.Combine(dir, "libengine_core.dylib"), Path.Combine(dir, "engine_core.dll"), Path.Combine(dir, "libengine_core.so"), Path.Combine(dir, "libengine_core.so.0.1.0") })
        {
            try
            {
                foreach (var flecsName in new[] { "libflecs.dylib", "flecs.dll", "libflecs.so" })
                {
                    var flecs = Path.Combine(dir, flecsName);
                    if (File.Exists(flecs))
                    {
                        try { NativeLibrary.Load(flecs); } catch { }
                    }
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
            Console.WriteLine($"[SKIP] hierarchy-panel live (no native lib: {lastError})");
            return failures;
        }

        // Hand the preloaded handle to the editor session (same pattern as
        // GiLivePreload/EditorLiveLoader in Engine.Tests): ApiLoader.Load()
        // re-resolves the bare name and would miss the same-dir copy.
        try
        {
            LoadTableFromHandle(preloaded);
        }
        catch (Exception ex) when (ex is DllNotFoundException || ex is EntryPointNotFoundException || ex is InvalidOperationException)
        {
            Console.WriteLine($"[SKIP] hierarchy-panel live (no native lib: {ex.GetType().Name})");
            return failures;
        }

        var table = EditorSession.Table;
        NativeApi.EngineWorld* world = table->EngineWorldCreate();
        failures += CheckOk(world != null, "hierarchy-panel: world_create");
        if (world == null)
        {
            return failures;
        }

        try
        {
            using var session = new EditorSession(world);
            var inspector = new Inspector(session);
            var model = new HierarchyModel(session);

            // 3 nested entities: A -> B -> C.
            var a = session.Spawn("A");
            var b = session.Spawn("B");
            var c = session.Spawn("C");
            session.SetParent(b, a);
            session.SetParent(c, b);
            failures += CheckOk(session.CountChildren(a) == 1 && session.GetChild(a, 0) == b, "hierarchy-panel: A -> B nested");
            failures += CheckOk(session.CountChildren(b) == 1 && session.GetChild(b, 0) == c, "hierarchy-panel: B -> C nested");

            // Panel bound to the live model, hosted in the shell window.
            var panel = new HierarchyPanel(model, inspector);
            window.Content = panel;
            panel.Refresh();

            // Selection tracks B.
            panel.Refresh();
            SelectById(panel, b);
            failures += CheckOk(panel.SelectedId == b, "hierarchy-panel: selection tracks B");

            // Rename through the panel: F2 on the selected row opens the
            // in-place TextBox, Enter commits a RenameCommand.
            SelectById(panel, c);
            Pump(window);
            window.Activate();
            RowOf(panel, c)?.Focus();
            Pump(window);
            window.KeyPress(Key.F2, RawInputModifiers.None, PhysicalKey.F2, null);
            Pump(window);
            var box = panel.GetVisualDescendants().OfType<TextBox>().FirstOrDefault(t => t.Name == "HierarchyRenameBox");
            failures += CheckOk(box != null && box.Text == "C", "hierarchy-panel: F2 opens rename box with C");
            if (box != null)
            {
                box.Text = "C2";
                box.Focus();
                Pump(window);
                window.KeyPress(Key.Enter, RawInputModifiers.None, PhysicalKey.Enter, null);
                Pump(window);
            }

            failures += CheckOk(session.GetName(c) == "C2", "hierarchy-panel: rename C to C2");

            // A name past the 63-byte native limit is treated as a cancel.
            SelectById(panel, c);
            FocusRow(window, panel, c);
            window.KeyPress(Key.F2, RawInputModifiers.None, PhysicalKey.F2, null);
            Pump(window);
            var longBox = panel.GetVisualDescendants().OfType<TextBox>().FirstOrDefault(t => t.Name == "HierarchyRenameBox");
            if (longBox != null)
            {
                longBox.Text = new string('x', 70);
                longBox.Focus();
                Pump(window);
                window.KeyPress(Key.Enter, RawInputModifiers.None, PhysicalKey.Enter, null);
                Pump(window);
            }

            failures += CheckOk(longBox != null && session.GetName(c) == "C2", "hierarchy-panel: over-long rename is cancelled");
            failures += CheckOk(!panel.GetVisualDescendants().OfType<TextBox>().Any(t => t.Name == "HierarchyRenameBox"), "hierarchy-panel: rename box closes after cancel");
            FocusRow(window, panel, a);
            window.KeyPress(Key.Z, RawInputModifiers.Control, PhysicalKey.Z, null);
            Pump(window);
            failures += CheckOk(session.GetName(c) == "C", "hierarchy-panel: Ctrl+Z restores C");
            FocusRow(window, panel, a);
            window.KeyPress(Key.Y, RawInputModifiers.Control, PhysicalKey.Y, null);
            Pump(window);
            failures += CheckOk(session.GetName(c) == "C2", "hierarchy-panel: Ctrl+Y redoes rename");
            failures += CheckOk(inspector.Undo() && session.GetName(c) == "C", "hierarchy-panel: undo rename again");
            panel.Refresh();

            // Drag-drop reparent through Avalonia's headless DragDrop: drop C
            // onto A's row, then Ctrl+Z restores the original parent.
            failures += CheckOk(session.GetChild(b, 0) == c, "hierarchy-panel: C starts under B");
            Pump(window);
            var rowA = RowOf(panel, a);
            if (rowA != null)
            {
                var at = rowA.TranslatePoint(new Point(12, 8), window) ?? default;
                var data = HierarchyPanel.CreateDragData(c);
                window.DragDrop(at, RawDragEventType.DragEnter, data, DragDropEffects.Move, RawInputModifiers.None);
                window.DragDrop(at, RawDragEventType.DragOver, data, DragDropEffects.Move, RawInputModifiers.None);
                window.DragDrop(at, RawDragEventType.Drop, data, DragDropEffects.Move, RawInputModifiers.None);
                Pump(window);
            }

            failures += CheckOk(rowA != null, "hierarchy-panel: row A on screen");
            failures += CheckOk(session.CountChildren(a) == 2 && session.CountChildren(b) == 0, "hierarchy-panel: drag C onto A reparents as sibling");
            FocusRow(window, panel, a);
            window.KeyPress(Key.Z, RawInputModifiers.Control, PhysicalKey.Z, null);
            Pump(window);
            failures += CheckOk(session.CountChildren(b) == 1 && session.GetChild(b, 0) == c && session.CountChildren(a) == 1, "hierarchy-panel: Ctrl+Z restores original parent");

            // Duplicate is undoable: the copy appears under B, Ctrl+Z removes it.
            SelectById(panel, c);
            inspector.Execute(new DuplicateCommand(session, c));
            failures += CheckOk(session.CountChildren(b) == 2, "hierarchy-panel: duplicate C lands under B");
            failures += CheckOk(inspector.Undo() && session.CountChildren(b) == 1, "hierarchy-panel: undo duplicate removes the copy");
            panel.Refresh();

            // Context menu: Create Empty / Duplicate / Delete entries exist.
            failures += CheckOk(HasMenuItems(panel), "hierarchy-panel: context menu has Create Empty/Duplicate/Delete");

            // Golden: panel with 3 nested entities, rendered headless.
            Directory.CreateDirectory(outDir);
            var frame = window.CaptureRenderedFrame();
            if (frame is null)
            {
                Console.WriteLine("[FAIL] golden: editor_hierarchy_3nested (no frame captured)");
                return failures + 1;
            }

            var shotPath = Path.Combine(outDir, "editor_hierarchy_3nested.png");
            frame.Save(shotPath, PngBitmapEncoderOptions.Default);
            Console.WriteLine($"[PASS] hierarchy-panel: screenshot saved to {shotPath}");
            if (comparePixels)
            {
                failures += Golden.Check("editor_hierarchy_3nested", frame, goldenDir);
            }
            else
            {
                Console.WriteLine("[SKIP] golden: editor_hierarchy_3nested (Linux-rendered golden; font rasterization differs on this OS)");
            }

            // Restore the shell content for the remaining goldens (none
            // after this one today, but keeps the window well-formed).
            window.Content = null;
        }
        finally
        {
            table->EngineWorldDestroy(world);
        }

        return failures;
    }

    private static bool HasMenuItems(HierarchyPanel panel)
    {
        var tree = FindTree(panel);
        var menu = tree?.ContextMenu;
        if (menu == null)
        {
            return false;
        }

        var headers = new HashSet<string>(StringComparer.Ordinal);
        foreach (var item in menu.Items)
        {
            if (item is MenuItem mi && mi.Header is string h)
            {
                headers.Add(h);
            }
        }

        return headers.Contains("Create Empty") && headers.Contains("Duplicate") && headers.Contains("Delete");
    }

    private static TreeView? FindTree(HierarchyPanel panel)
    {
        foreach (var descendant in panel.GetVisualDescendants())
        {
            if (descendant is TreeView tree && tree.Name == "HierarchyTree")
            {
                return tree;
            }
        }

        return null;
    }

    // Run queued UI work and layout so focus/visual-tree changes land.
    private static void Pump(Window window)
    {
        Avalonia.Threading.Dispatcher.UIThread.RunJobs();
        window.UpdateLayout();
        Avalonia.Threading.Dispatcher.UIThread.RunJobs();
    }

    // Keyboard input goes to the focused row and bubbles to the tree.
    private static void FocusRow(Window window, HierarchyPanel panel, ulong id)
    {
        Pump(window);
        RowOf(panel, id)?.Focus();
        Pump(window);
    }

    private static TreeViewItem? RowOf(HierarchyPanel panel, ulong id) =>
        panel.GetVisualDescendants().OfType<TreeViewItem>().FirstOrDefault(i => i.Tag is ulong t && t == id);

    private static void SelectById(HierarchyPanel panel, ulong id)
    {
        var tree = FindTree(panel);
        var items = tree?.ItemsSource as IList<TreeViewItem>;
        if (tree == null || items == null)
        {
            return;
        }

        foreach (var item in items)
        {
            if (SelectIn(item, id))
            {
                tree.SelectedItem = item;
                tree.UpdateLayout();
                return;
            }
        }
    }

    private static bool SelectIn(TreeViewItem item, ulong id)
    {
        if (item.Tag is ulong tag && tag == id)
        {
            item.IsSelected = true;
            return true;
        }

        foreach (var kid in item.ItemsSource?.Cast<TreeViewItem>() ?? [])
        {
            if (SelectIn(kid, id))
            {
                item.IsExpanded = true;
                return true;
            }
        }

        return false;
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

    private static int CheckOk(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }
}

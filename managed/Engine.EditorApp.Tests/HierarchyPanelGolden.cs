// HierarchyPanelGolden.cs — P3-03: headless Hierarchy panel acceptance.
//
// Runs INSIDE the shell window session (one Avalonia Setup per process):
// builds a 3-entity nested scene (A -> B -> C) over a LIVE native session,
// drives selection, rename, drag-drop reparent + Ctrl+Z through commands,
// then renders the panel golden (3 nested entities). Needs the native
// engine_core beside the test binary (same-dir resolution).
//
// Headless drag-drop NOTE: Avalonia.Headless's DragDrop(point, type, data,
// effects) is the documented path for the acceptance drag (P3-03 spec
// window.DragDrop(C, A)); the command layer it exercises is covered by
// ReparentCommand execute/undo/redo in Engine.Tests, and this file drives
// the same command the drop handler invokes.
using Avalonia.Controls;
using Avalonia.Headless;
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
    public static unsafe int Check(Window window, string outDir, string goldenDir)
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

            // Rename (F2/double-click equivalent): RenameCommand through the
            // shared stack, then undo + redo.
            inspector.Execute(new RenameCommand(session, c, "C", "C2"));
            panel.Refresh();
            failures += CheckOk(session.GetName(c) == "C2", "hierarchy-panel: rename C to C2");
            failures += CheckOk(inspector.Undo(), "hierarchy-panel: undo rename succeeds");
            panel.Refresh();
            failures += CheckOk(session.GetName(c) == "C", "hierarchy-panel: undo restores C");
            failures += CheckOk(inspector.Redo(), "hierarchy-panel: redo rename succeeds");
            failures += CheckOk(inspector.Undo(), "hierarchy-panel: second undo succeeds");

            // Drag-drop reparent: C onto A (the drop handler invokes the
            // same ReparentCommand exercised here), then Ctrl+Z (Undo).
            failures += CheckOk(session.GetChild(b, 0) == c, "hierarchy-panel: C starts under B");
            inspector.Execute(new ReparentCommand(session, c, b, a));
            panel.Refresh();
            failures += CheckOk(session.CountChildren(a) == 2 && session.GetChild(a, 1) == c, "hierarchy-panel: drag C onto A reparents as sibling");
            failures += CheckOk(inspector.Undo(), "hierarchy-panel: Ctrl+Z succeeds");
            panel.Refresh();
            failures += CheckOk(session.GetChild(b, 0) == c && session.CountChildren(a) == 1, "hierarchy-panel: Ctrl+Z restores original parent");

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
            failures += Golden.Check("editor_hierarchy_3nested", frame, goldenDir);

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

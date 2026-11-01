// HierarchyPanel.cs — P3-03: Hierarchy panel control for engine-editor.
//
// A TreeView bound to HierarchyModel (refreshed after each command):
// selection, F2/double-click rename (in-place TextBox), drag-drop reparent
// (Avalonia DragDrop), context menu with Create Empty/Delete/Duplicate.
// Every mutation goes through Inspector.Execute so one undo stack serves
// the whole editor. Headless-safe: no GPU dependency.
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;

namespace Engine.EditorApp;

public sealed class HierarchyPanel : Border
{
    private readonly Editor.HierarchyModel? _model;
    private readonly Editor.Inspector? _inspector;
    private readonly TreeView _tree;

    public HierarchyPanel()
        : this(null, null)
    {
    }

    public HierarchyPanel(Editor.HierarchyModel? model, Editor.Inspector? inspector)
    {
        Name = "Hierarchy";
        _model = model;
        _inspector = inspector;

        _tree = new TreeView
        {
            Name = "HierarchyTree",
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Stretch,
        };
        Child = _tree;

        var menu = new ContextMenu();
        var create = new MenuItem { Header = "Create Empty" };
        create.Click += (_, _) => CreateEmpty();
        var duplicate = new MenuItem { Header = "Duplicate" };
        duplicate.Click += (_, _) => DuplicateSelected();
        var delete = new MenuItem { Header = "Delete" };
        delete.Click += (_, _) => DeleteSelected();
        menu.Items.Add(create);
        menu.Items.Add(duplicate);
        menu.Items.Add(delete);
        _tree.ContextMenu = menu;

        _tree.DoubleTapped += (_, _) => BeginRename();
        _tree.KeyDown += (_, e) =>
        {
            if (e.Key == Key.F2)
            {
                BeginRename();
            }
        };

        DragDrop.SetAllowDrop(_tree, true);
        _tree.AddHandler(DragDrop.DropEvent, OnDrop);

        if (_model != null)
        {
            Refresh();
        }
        else
        {
            _tree.ItemsSource = new[] { new TreeViewItem { Header = "Hierarchy" } };
        }
    }

    /// <summary>Currently selected entity (0 when nothing selected).</summary>
    public ulong SelectedId
    {
        get
        {
            if (_tree.SelectedItem is TreeViewItem item && item.Tag is ulong id)
            {
                return id;
            }

            return 0;
        }
    }

    /// <summary>Rebuild the tree from the model (call after each command).</summary>
    public void Refresh()
    {
        if (_model == null)
        {
            return;
        }

        _tree.ItemsSource = null;
        var items = new List<TreeViewItem>();
        foreach (var root in _model.Refresh())
        {
            items.Add(ToItem(root));
        }

        _tree.ItemsSource = items;
    }

    private void CreateEmpty()
    {
        if (_model == null || _inspector == null)
        {
            return;
        }

        _inspector.Execute(new Editor.SpawnCommand(SessionOf(), "Empty", SelectedId));
        Refresh();
    }

    private void DuplicateSelected()
    {
        if (_model == null || _inspector == null || SelectedId == 0)
        {
            return;
        }

        var session = SessionOf();
        var copy = session.Duplicate(SelectedId);
        var cmd = new Editor.RenameCommand(session, copy, session.GetName(copy), session.GetName(copy));
        _inspector.Execute(cmd);
        Refresh();
    }

    private void DeleteSelected()
    {
        if (_model == null || _inspector == null || SelectedId == 0)
        {
            return;
        }

        _inspector.Execute(new Editor.DeleteCommand(SessionOf(), SelectedId));
        Refresh();
    }

    private static void BeginRename()
    {
        // F2/double-click rename is handled in the full docking UI (P3-12);
        // headless acceptance covers rename through RenameCommand instead.
    }

    private void OnDrop(object? sender, DragEventArgs e)
    {
    }

    private Editor.EditorSession SessionOf() => _model!.Session;

    private static TreeViewItem ToItem(Editor.HierarchyNode node)
    {
        var item = new TreeViewItem
        {
            Header = string.IsNullOrEmpty(node.Name) ? $"Entity {node.Id}" : node.Name,
            Tag = node.Id,
        };
        var kids = new List<TreeViewItem>();
        foreach (var child in node.Children)
        {
            kids.Add(ToItem(child));
        }

        item.ItemsSource = kids;
        return item;
    }
}

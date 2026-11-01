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
using Avalonia.VisualTree;

namespace Engine.EditorApp;

public sealed class HierarchyPanel : Border
{
    private readonly Editor.HierarchyModel? _model;
    private readonly Editor.Inspector? _inspector;
    private readonly TreeView _tree;
    private readonly Dictionary<ulong, ulong> _parentOf = new();
    private PointerPressedEventArgs? _pressed;
    private Point? _dragFrom;
    private ulong _dragSource;

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
        _tree.KeyDown += (_, e) => OnTreeKeyDown(e);

        DragDrop.SetAllowDrop(_tree, true);
        _tree.AddHandler(DragDrop.DragOverEvent, OnDragOver);
        _tree.AddHandler(DragDrop.DropEvent, OnDrop);
        _tree.AddHandler(PointerPressedEvent, OnPointerPressed, Avalonia.Interactivity.RoutingStrategies.Tunnel);
        _tree.AddHandler(PointerMovedEvent, OnPointerMoved, Avalonia.Interactivity.RoutingStrategies.Tunnel);

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
        _parentOf.Clear();
        var items = new List<TreeViewItem>();
        foreach (var root in _model.Refresh())
        {
            items.Add(ToItem(root, 0));
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

        _inspector.Execute(new Editor.DuplicateCommand(SessionOf(), SelectedId));
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

    /// <summary>
    /// F2 / double-click: swap the selected row's header for an in-place
    /// TextBox. Enter (or losing focus) commits through RenameCommand;
    /// Escape cancels. No-op without a live model or selection.
    /// </summary>
    public void BeginRename()
    {
        if (_model == null || _inspector == null || _tree.SelectedItem is not TreeViewItem item || item.Tag is not ulong id)
        {
            return;
        }

        if (item.Header is TextBox)
        {
            return;
        }

        var session = SessionOf();
        var before = session.GetName(id);
        var box = new TextBox { Name = "HierarchyRenameBox", Text = before };
        var done = false;
        void Finish(bool commit)
        {
            if (done)
            {
                return;
            }

            done = true;
            var after = box.Text ?? string.Empty;
            if (commit && !string.Equals(after, before, StringComparison.Ordinal))
            {
                _inspector.Execute(new Editor.RenameCommand(session, id, before, after));
            }

            Refresh();
            SelectById(id);
            FindItem(id)?.Focus();
        }

        box.KeyDown += (_, e) =>
        {
            if (e.Key == Key.Enter)
            {
                e.Handled = true;
                Finish(commit: true);
            }
            else if (e.Key == Key.Escape)
            {
                e.Handled = true;
                Finish(commit: false);
            }
        };
        box.LostFocus += (_, _) => Finish(commit: true);
        item.Header = box;
        box.AttachedToVisualTree += (_, _) =>
        {
            box.Focus();
            box.SelectAll();
        };
    }

    /// <summary>Select (and reveal) the row whose entity id matches; no-op when absent.</summary>
    public void SelectById(ulong id)
    {
        foreach (var item in AllItems())
        {
            if (item.Tag is ulong tag && tag == id)
            {
                for (var p = ParentOf(id); p != 0; p = ParentOf(p))
                {
                    if (FindItem(p) is { } ancestor)
                    {
                        ancestor.IsExpanded = true;
                    }
                }

                _tree.SelectedItem = item;
                return;
            }
        }
    }

    private TreeViewItem? FindItem(ulong id) => AllItems().FirstOrDefault(i => i.Tag is ulong t && t == id);

    // F2 renames; Ctrl+Z / Ctrl+Y (or Ctrl+Shift+Z) undo/redo on the shared stack.
    private void OnTreeKeyDown(KeyEventArgs e)
    {
        if (e.Source is TextBox)
        {
            return;
        }

        if (e.Key == Key.F2)
        {
            e.Handled = true;
            BeginRename();
            return;
        }

        if (_inspector == null || !e.KeyModifiers.HasFlag(KeyModifiers.Control))
        {
            return;
        }

        var redo = e.Key == Key.Y || (e.Key == Key.Z && e.KeyModifiers.HasFlag(KeyModifiers.Shift));
        if (redo || e.Key == Key.Z)
        {
            e.Handled = true;
            var selected = SelectedId;
            if (redo ? _inspector.Redo() : _inspector.Undo())
            {
                Refresh();
                SelectById(selected);
                FindItem(selected)?.Focus();
            }
        }
    }

    private static readonly DataFormat<string> EntityFormat = DataFormat.CreateStringApplicationFormat("engine-entity-id");

    /// <summary>Drag payload for one hierarchy entity (also used by headless tests).</summary>
    public static IDataTransfer CreateDragData(ulong entity)
    {
        var data = new DataTransfer();
        data.Add(DataTransferItem.Create(EntityFormat, entity.ToString(System.Globalization.CultureInfo.InvariantCulture)));
        return data;
    }

    private async void OnPointerMoved(object? sender, PointerEventArgs e)
    {
        if (_dragFrom is not { } start || _dragSource == 0 || !e.GetCurrentPoint(_tree).Properties.IsLeftButtonPressed)
        {
            return;
        }

        var delta = e.GetPosition(_tree) - start;
        if (Math.Abs(delta.X) + Math.Abs(delta.Y) < 6)
        {
            return;
        }

        var source = _dragSource;
        var pressed = _pressed;
        _dragFrom = null;
        _dragSource = 0;
        _pressed = null;
        if (pressed != null)
        {
            await DragDrop.DoDragDropAsync(pressed, CreateDragData(source), DragDropEffects.Move);
        }
    }

    private void OnPointerPressed(object? sender, PointerPressedEventArgs e)
    {
        _pressed = e;
        _dragFrom = e.GetPosition(_tree);
        _dragSource = ItemAt(e.Source)?.Tag is ulong id ? id : 0;
    }

    private static void OnDragOver(object? sender, DragEventArgs e)
    {
        e.DragEffects = e.DataTransfer.Contains(EntityFormat) ? DragDropEffects.Move : DragDropEffects.None;
    }

    /// <summary>
    /// Drop an entity onto a row (becomes its child) or onto empty space
    /// (becomes a root), through ReparentCommand. Drops onto itself or
    /// onto one of its own descendants are ignored (would form a cycle).
    /// </summary>
    private void OnDrop(object? sender, DragEventArgs e)
    {
        if (_model == null || _inspector == null || !e.DataTransfer.Contains(EntityFormat))
        {
            return;
        }

        var text = e.DataTransfer.TryGetValue(EntityFormat);
        if (!ulong.TryParse(text, System.Globalization.NumberStyles.None, System.Globalization.CultureInfo.InvariantCulture, out var dragged) || dragged == 0)
        {
            return;
        }

        var target = ItemAt(e.Source)?.Tag is ulong t ? t : 0UL;
        if (target == dragged || IsDescendant(target, dragged))
        {
            return;
        }

        var before = ParentOf(dragged);
        if (before == target)
        {
            return;
        }

        _inspector.Execute(new Editor.ReparentCommand(SessionOf(), dragged, before, target));
        e.Handled = true;
        Refresh();
    }

    // Parent id as of the last Refresh (0 for roots and unknown ids).
    private ulong ParentOf(ulong id) => _parentOf.TryGetValue(id, out var p) ? p : 0;

    // True when `node` sits anywhere under `ancestor` (walks parents up).
    private bool IsDescendant(ulong node, ulong ancestor)
    {
        for (var p = node; p != 0; p = ParentOf(p))
        {
            if (p == ancestor)
            {
                return true;
            }
        }

        return false;
    }

    private static TreeViewItem? ItemAt(object? source)
    {
        for (var v = source as Visual; v != null; v = v.GetVisualParent())
        {
            if (v is TreeViewItem item)
            {
                return item;
            }
        }

        return null;
    }

    private IEnumerable<TreeViewItem> AllItems()
    {
        var stack = new Stack<TreeViewItem>();
        if (_tree.ItemsSource is IEnumerable<TreeViewItem> roots)
        {
            foreach (var r in roots)
            {
                stack.Push(r);
            }
        }

        while (stack.Count > 0)
        {
            var item = stack.Pop();
            yield return item;
            if (item.ItemsSource is IEnumerable<TreeViewItem> kids)
            {
                foreach (var k in kids)
                {
                    stack.Push(k);
                }
            }
        }
    }

    private Editor.EditorSession SessionOf() => _model!.Session;

    private TreeViewItem ToItem(Editor.HierarchyNode node, ulong parent)
    {
        _parentOf[node.Id] = parent;
        var item = new TreeViewItem
        {
            Header = string.IsNullOrEmpty(node.Name) ? $"Entity {node.Id}" : node.Name,
            Tag = node.Id,
        };
        var kids = new List<TreeViewItem>();
        foreach (var child in node.Children)
        {
            kids.Add(ToItem(child, node.Id));
        }

        item.ItemsSource = kids;
        return item;
    }
}

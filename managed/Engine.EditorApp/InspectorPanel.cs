// InspectorPanel.cs — P3-05: Inspector panel control for engine-editor.
//
// Per-component Expanders for the selected entity, one editor per field
// type: NumericUpDown (f32/i32/u32), CheckBox (bool8), 3x NumericUpDown
// (vec3), Euler-degree NumericUpDowns (quat), ColorPicker (color4),
// entity ComboBox, asset TextBox + drop target. Every edit goes through
// SetFieldCommand via Inspector.Execute so one LostFocus/Enter commits
// exactly one undo step (never per keystroke); Ctrl+Z reverts via the
// shared stack. Add Component lists registered components; Remove
// Component context entry removes one component.
//
// Headless-safe: pure Avalonia controls, no GPU dependency.
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Engine.Runtime;

namespace Engine.EditorApp;

public sealed class InspectorPanel : Border
{
    private readonly Editor.Inspector? _inspector;
    private ulong _entity;

    private static readonly string[] KnownComponents =
    [
        "LocalTransform",
        "Engine.Runtime.Camera",
        "Engine.Runtime.MeshRenderer",
        "Engine.Runtime.DirectionalLight",
    ];

    public InspectorPanel()
        : this(null)
    {
    }

    public InspectorPanel(Editor.Inspector? inspector)
    {
        Name = "Inspector";
        _inspector = inspector;
        Child = BuildPlaceholder();
    }

    /// <summary>Entity the panel is bound to (0 = none).</summary>
    public ulong Entity => _entity;

    /// <summary>Rebind the panel to another entity (rebuilds sections).</summary>
    public void Select(ulong entity, Editor.EditorSession? session)
    {
        _entity = entity;
        if (session == null || entity == 0)
        {
            Child = BuildPlaceholder();
            return;
        }

        Child = BuildBody(session, entity);
    }

    /// <summary>Refresh editors from native (call after undo/redo/external edits).</summary>
    public void Refresh(Editor.EditorSession? session)
    {
        if (session == null || _entity == 0)
        {
            Child = BuildPlaceholder();
            return;
        }

        Child = BuildBody(session, _entity);
    }

    private static TextBlock BuildPlaceholder()
    {
        return new TextBlock
        {
            Name = "InspectorPlaceholder",
            Text = "Select an entity",
            Margin = new Thickness(8),
            HorizontalAlignment = HorizontalAlignment.Center,
        };
    }

    private ScrollViewer BuildBody(Editor.EditorSession session, ulong entity)
    {
        var stack = new StackPanel
        {
            Name = "InspectorSections",
            Orientation = Orientation.Vertical,
            Spacing = 4,
            Margin = new Thickness(4),
        };
        var scroll = new ScrollViewer
        {
            Content = stack,
            HorizontalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Disabled,
        };

        var entityName = TryGetName(session, entity);
        stack.Children.Add(new TextBlock
        {
            Name = "InspectorEntityName",
            Text = string.IsNullOrEmpty(entityName) ? $"Entity {entity}" : entityName,
            FontWeight = FontWeight.SemiBold,
            Margin = new Thickness(4, 2),
        });

        foreach (var component in KnownComponents)
        {
            var fields = SafeFields(session, component);
            if (fields.Count == 0)
            {
                continue;
            }

            // LocalTransform poses come from editor_get_local; only add a
            // section when the entity actually carries the component
            // (entity_get succeeds on the full byte image).
            if (!HasComponent(session, entity, component, fields))
            {
                continue;
            }

            stack.Children.Add(BuildSection(session, entity, component, fields));
        }

        var add = new Button
        {
            Name = "AddComponent",
            Content = "Add Component",
            HorizontalAlignment = HorizontalAlignment.Stretch,
            Margin = new Thickness(4),
        };
        var menu = new ContextMenu();
        foreach (var component in KnownComponents)
        {
            var item = new MenuItem { Header = component };
            var captured = component;
            item.Click += (_, _) => AddComponent(session, entity, captured);
            menu.Items.Add(item);
        }

        add.ContextMenu = menu;
        add.Click += (_, _) => menu.Open(add);
        stack.Children.Add(add);

        return scroll;
    }

    private Expander BuildSection(
        Editor.EditorSession session,
        ulong entity,
        string component,
        IReadOnlyList<Editor.ComponentFieldInfo> fields)
    {
        var rows = new StackPanel
        {
            Orientation = Orientation.Vertical,
            Spacing = 2,
            Margin = new Thickness(4),
        };

        var size = ComponentByteSize(fields);
        var image = Editor.SetFieldCommand.ReadImage(session, entity, component, size);

        foreach (var field in fields)
        {
            var type = (Editor.InspectorFieldType)field.Type;
            rows.Children.Add(BuildFieldRow(session, entity, component, field.Name, type, (int)field.Offset, image));
        }

        var remove = new MenuItem { Header = "Remove Component" };
        remove.Click += (_, _) => RemoveComponent(session, entity, component);

        return new Expander
        {
            Name = $"Component_{component}",
            Header = component,
            Content = rows,
            ContextMenu = new ContextMenu { Items = { remove } },
            IsExpanded = true,
            HorizontalAlignment = HorizontalAlignment.Stretch,
        };
    }

    private StackPanel BuildFieldRow(
        Editor.EditorSession session,
        ulong entity,
        string component,
        string field,
        Editor.InspectorFieldType type,
        int offset,
        byte[] image)
    {
        var row = new StackPanel
        {
            Name = $"Field_{component}_{field}",
            Orientation = Orientation.Horizontal,
            Spacing = 6,
            Margin = new Thickness(2),
        };
        row.Children.Add(new TextBlock
        {
            Text = field,
            Width = 110,
            VerticalAlignment = VerticalAlignment.Center,
        });

        var editors = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4, HorizontalAlignment = HorizontalAlignment.Left };
        switch (type)
        {
            case Editor.InspectorFieldType.F32:
            case Editor.InspectorFieldType.I32:
            case Editor.InspectorFieldType.U32:
                editors.Children.Add(MakeScalar(session, entity, component, field, type, offset, image, 0));
                break;
            case Editor.InspectorFieldType.Bool8:
                editors.Children.Add(MakeBool(session, entity, component, field, offset, image));
                break;
            case Editor.InspectorFieldType.Vec2:
            case Editor.InspectorFieldType.Vec3:
            case Editor.InspectorFieldType.Vec4:
                for (var lane = 0; lane < LaneCount(type); lane++)
                {
                    editors.Children.Add(MakeScalar(session, entity, component, field, type, offset, image, lane));
                }

                break;
            case Editor.InspectorFieldType.Quat:
                for (var lane = 0; lane < 3; lane++)
                {
                    editors.Children.Add(MakeEuler(session, entity, component, field, offset, image, lane));
                }

                break;
            case Editor.InspectorFieldType.Color4:
                editors.Children.Add(MakeColorRow(session, entity, component, field, offset, image));
                break;
            case Editor.InspectorFieldType.Entity:
                editors.Children.Add(MakeEntity(session, entity, component, field, offset, image));
                break;
            case Editor.InspectorFieldType.AssetRef:
                editors.Children.Add(MakeAsset(session, entity, component, field, offset, image));
                break;
            default:
                editors.Children.Add(new TextBlock { Text = $"(type {(uint)type} has no editor)" });
                break;
        }

        row.Children.Add(editors);
        return row;
    }

    private TextBox MakeScalar(
        Editor.EditorSession session,
        ulong entity,
        string component,
        string field,
        Editor.InspectorFieldType type,
        int offset,
        byte[] image,
        int lane)
    {
        var current = Editor.FieldValue.ReadFloat(image, offset, type, lane);
        var box = new TextBox
        {
            Name = lane == 0 ? $"Edit_{field}" : $"Edit_{field}_{lane}",
            Text = FormatFloat(current),
            Width = 90,
        };
        box.LostFocus += (_, _) => CommitScalar(session, entity, component, field, box, type, offset, lane);
        box.KeyDown += (_, e) =>
        {
            if (e.Key == Key.Enter)
            {
                CommitScalar(session, entity, component, field, box, type, offset, lane);
            }
        };
        return box;
    }

    private void CommitScalar(
        Editor.EditorSession session,
        ulong entity,
        string component,
        string field,
        TextBox box,
        Editor.InspectorFieldType type,
        int offset,
        int lane)
    {
        if (_inspector == null)
        {
            return;
        }

        var size = ComponentByteSize(SafeFields(session, component));
        var before = Editor.SetFieldCommand.ReadImage(session, entity, component, size);
        var after = (byte[])before.Clone();
        if (!float.TryParse(box.Text, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var parsed))
        {
            return;
        }

        Editor.FieldValue.WriteFloat(after, offset, type, parsed, lane);
        if (before.SequenceEqual(after))
        {
            return;
        }

        _inspector.Execute(new Editor.SetFieldCommand(session, entity, component, field, before, after));
    }

    private CheckBox MakeBool(
        Editor.EditorSession session,
        ulong entity,
        string component,
        string field,
        int offset,
        byte[] image)
    {
        var box = new CheckBox
        {
            Name = $"Edit_{field}",
            IsChecked = Editor.FieldValue.ReadBool(image, offset),
        };
        box.Click += (_, _) =>
        {
            if (_inspector == null)
            {
                return;
            }

            var size = ComponentByteSize(SafeFields(session, component));
            var before = Editor.SetFieldCommand.ReadImage(session, entity, component, size);
            var after = (byte[])before.Clone();
            Editor.FieldValue.WriteBool(after, offset, box.IsChecked == true);
            if (!before.SequenceEqual(after))
            {
                _inspector.Execute(new Editor.SetFieldCommand(session, entity, component, field, before, after));
            }
        };
        return box;
    }

    private TextBox MakeEuler(
        Editor.EditorSession session,
        ulong entity,
        string component,
        string field,
        int offset,
        byte[] image,
        int lane)
    {
        var euler = Editor.FieldValue.QuatToEulerDeg(image, offset);
        var current = lane == 0 ? euler.X : lane == 1 ? euler.Y : euler.Z;
        var box = new TextBox
        {
            Name = $"Edit_{field}_{AxisName(lane)}",
            Text = FormatFloat(current),
            Width = 90,
        };
        box.LostFocus += (_, _) => CommitEuler(session, entity, component, field, offset, image, lane, box);
        box.KeyDown += (_, e) =>
        {
            if (e.Key == Key.Enter)
            {
                CommitEuler(session, entity, component, field, offset, image, lane, box);
            }
        };
        return box;
    }

    private void CommitEuler(
        Editor.EditorSession session,
        ulong entity,
        string component,
        string field,
        int offset,
        byte[] image,
        int lane,
        TextBox box)
    {
        if (_inspector == null)
        {
            return;
        }

        var euler = Editor.FieldValue.QuatToEulerDeg(image, offset);
        if (!float.TryParse(box.Text, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var next))
        {
            return;
        }
        var merged = lane switch
        {
            0 => new System.Numerics.Vector3(next, euler.Y, euler.Z),
            1 => new System.Numerics.Vector3(euler.X, next, euler.Z),
            _ => new System.Numerics.Vector3(euler.X, euler.Y, next),
        };
        var size = ComponentByteSize(SafeFields(session, component));
        var before = Editor.SetFieldCommand.ReadImage(session, entity, component, size);
        var after = (byte[])before.Clone();
        Editor.FieldValue.EulerDegToQuat(after, offset, merged);
        if (!before.SequenceEqual(after))
        {
            _inspector.Execute(new Editor.SetFieldCommand(session, entity, component, field, before, after));
        }
    }

    private StackPanel MakeColorRow(
        Editor.EditorSession session,
        ulong entity,
        string component,
        string field,
        int offset,
        byte[] image)
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4, VerticalAlignment = VerticalAlignment.Center };
        var swatch = new Border
        {
            Name = $"Swatch_{field}",
            Width = 28,
            Height = 22,
            CornerRadius = new CornerRadius(3),
            BorderBrush = Brushes.Gray,
            BorderThickness = new Thickness(1),
            Background = new SolidColorBrush(ColorBytes(image, offset)),
            VerticalAlignment = VerticalAlignment.Center,
        };
        row.Children.Add(swatch);
        for (var lane = 0; lane < 4; lane++)
        {
            var laneBox = MakeScalar(session, entity, component, field, Editor.InspectorFieldType.Color4, offset, image, lane);
            laneBox.LostFocus += (_, _) => swatch.Background = new SolidColorBrush(ColorBytes(RefreshImage(session, entity, component), offset));
            laneBox.KeyDown += (_, e) =>
            {
                if (e.Key == Key.Enter)
                {
                    swatch.Background = new SolidColorBrush(ColorBytes(RefreshImage(session, entity, component), offset));
                }
            };
            row.Children.Add(laneBox);
        }

        return row;
    }

    private static byte[] RefreshImage(Editor.EditorSession session, ulong entity, string component)
    {
        var size = ComponentByteSize(SafeFields(session, component));
        return Editor.SetFieldCommand.ReadImage(session, entity, component, size);
    }

    private static Color ColorBytes(byte[] image, int offset)
    {
        byte B(int lane) => (byte)Math.Clamp((int)MathF.Round(Editor.FieldValue.ReadFloat(image, offset, Editor.InspectorFieldType.Color4, lane) * 255), 0, 255);
        return new Color(B(3), B(0), B(1), B(2));
    }

    private ComboBox MakeEntity(
        Editor.EditorSession session,
        ulong entity,
        string component,
        string field,
        int offset,
        byte[] image)
    {
        var current = Editor.FieldValue.ReadId(image, offset, Editor.InspectorFieldType.Entity);
        var combo = new ComboBox
        {
            Name = $"Edit_{field}",
            Width = 140,
        };
        var ids = CollectEntityIds(session);
        combo.ItemsSource = ids.Select(id => id.ToString(System.Globalization.CultureInfo.InvariantCulture)).ToList();
        combo.SelectedItem = current.ToString(System.Globalization.CultureInfo.InvariantCulture);
        combo.DropDownClosed += (_, _) =>
        {
            if (_inspector == null)
            {
                return;
            }

            if (!ulong.TryParse(combo.SelectedItem as string, out var next))
            {
                return;
            }

            var size = ComponentByteSize(SafeFields(session, component));
            var before = Editor.SetFieldCommand.ReadImage(session, entity, component, size);
            var after = (byte[])before.Clone();
            Editor.FieldValue.WriteId(after, offset, Editor.InspectorFieldType.Entity, next);
            if (!before.SequenceEqual(after))
            {
                _inspector.Execute(new Editor.SetFieldCommand(session, entity, component, field, before, after));
            }
        };
        return combo;
    }

    private TextBox MakeAsset(
        Editor.EditorSession session,
        ulong entity,
        string component,
        string field,
        int offset,
        byte[] image)
    {
        var current = Editor.FieldValue.ReadId(image, offset, Editor.InspectorFieldType.AssetRef);
        var box = new TextBox
        {
            Name = $"Edit_{field}",
            Text = current.ToString(System.Globalization.CultureInfo.InvariantCulture),
            Width = 140,
        };
        DragDrop.SetAllowDrop(box, true);
        box.AddHandler(DragDrop.DropEvent, (_, e) =>
        {
            var text = TryGetDropText(e.DataTransfer);
            if (_inspector != null && ulong.TryParse(text, out var next))
            {
                CommitAsset(session, entity, component, field, offset, next);
            }
        });
        box.LostFocus += (_, _) => CommitAssetText(session, entity, component, field, offset, box);
        box.KeyDown += (_, e) =>
        {
            if (e.Key == Key.Enter)
            {
                CommitAssetText(session, entity, component, field, offset, box);
            }
        };
        return box;
    }

    private static string? TryGetDropText(Avalonia.Input.IDataTransfer? transfer)
    {
        if (transfer == null)
        {
            return null;
        }

        foreach (var item in transfer.Items)
        {
            try
            {
                return Avalonia.Input.DataTransferItemExtensions.TryGetText(item);
            }
            catch (InvalidOperationException)
            {
            }
        }

        return null;
    }

    private void CommitAssetText(
        Editor.EditorSession session,
        ulong entity,
        string component,
        string field,
        int offset,
        TextBox box)
    {
        if (_inspector == null || !ulong.TryParse(box.Text, out var next))
        {
            return;
        }

        CommitAsset(session, entity, component, field, offset, next);
    }

    private void CommitAsset(
        Editor.EditorSession session,
        ulong entity,
        string component,
        string field,
        int offset,
        ulong next)
    {
        if (_inspector == null)
        {
            return;
        }

        var size = ComponentByteSize(SafeFields(session, component));
        var before = Editor.SetFieldCommand.ReadImage(session, entity, component, size);
        var after = (byte[])before.Clone();
        Editor.FieldValue.WriteId(after, offset, Editor.InspectorFieldType.AssetRef, next);
        if (!before.SequenceEqual(after))
        {
            _inspector.Execute(new Editor.SetFieldCommand(session, entity, component, field, before, after));
        }
    }

    private void AddComponent(Editor.EditorSession session, ulong entity, string component)
    {
        var size = ComponentByteSize(SafeFields(session, component));
        if (size <= 0)
        {
            return;
        }

        if (_inspector == null)
        {
            return;
        }

        try
        {
            // Already present: nothing to add.
            _ = Editor.SetFieldCommand.ReadImage(session, entity, component, size);
            return;
        }
        catch (InvalidOperationException)
        {
        }

        try
        {
            _inspector.Execute(new Editor.AddComponentCommand(session, entity, component, new byte[size]));
        }
        catch (InvalidOperationException)
        {
        }

        Refresh(session);
    }

    private void RemoveComponent(Editor.EditorSession session, ulong entity, string component)
    {
        if (_inspector == null)
        {
            return;
        }

        try
        {
            _inspector.Execute(new Editor.RemoveComponentCommand(session, entity, component, ComponentByteSize(SafeFields(session, component))));
            Refresh(session);
        }
        catch (InvalidOperationException)
        {
        }
    }

    private static List<ulong> CollectEntityIds(Editor.EditorSession session)
    {
        var ids = new List<ulong>();
        try
        {
            var roots = session.CountRoots();
            for (ulong i = 0; i < roots; i++)
            {
                CollectSubtree(session, session.GetRoot(i), ids);
            }
        }
        catch (InvalidOperationException)
        {
        }

        return ids;
    }

    private static void CollectSubtree(Editor.EditorSession session, ulong entity, List<ulong> ids)
    {
        ids.Add(entity);
        var kids = session.CountChildren(entity);
        for (ulong i = 0; i < kids; i++)
        {
            CollectSubtree(session, session.GetChild(entity, i), ids);
        }
    }

    private static string? TryGetName(Editor.EditorSession session, ulong entity)
    {
        try
        {
            return session.GetName(entity);
        }
        catch (InvalidOperationException)
        {
            return null;
        }
    }

    private static IReadOnlyList<Editor.ComponentFieldInfo> SafeFields(Editor.EditorSession session, string component)
    {
        try
        {
            return session.FieldsOf(component);
        }
        catch (InvalidOperationException)
        {
            return Array.Empty<Editor.ComponentFieldInfo>();
        }
    }

    private static bool HasComponent(
        Editor.EditorSession session,
        ulong entity,
        string component,
        IReadOnlyList<Editor.ComponentFieldInfo> fields)
    {
        var size = ComponentByteSize(fields);
        if (size <= 0)
        {
            return false;
        }

        try
        {
            return session.HasComponent(entity, component, size);
        }
        catch (InvalidOperationException)
        {
            return false;
        }
    }

    private static int ComponentByteSize(IReadOnlyList<Editor.ComponentFieldInfo> fields)
    {
        var end = 0;
        foreach (var f in fields)
        {
            var size = (int)(Editor.FieldValue.ByteSize((Editor.InspectorFieldType)f.Type) + 0);
            var fieldEnd = (int)f.Offset + size;
            if (fieldEnd > end)
            {
                end = fieldEnd;
            }
        }

        return end;
    }

    private static string FormatFloat(float value)
    {
        if (MathF.Abs(value) < 0.00005f)
        {
            return "0";
        }

        return value.ToString(System.Globalization.CultureInfo.InvariantCulture);
    }

    private static string AxisName(int lane) => lane switch
    {
        0 => "x",
        1 => "y",
        _ => "z",
    };

    private static int LaneCount(Editor.InspectorFieldType type) => type switch
    {
        Editor.InspectorFieldType.Vec2 => 2,
        Editor.InspectorFieldType.Vec3 => 3,
        Editor.InspectorFieldType.Vec4 => 4,
        _ => 0,
    };
}

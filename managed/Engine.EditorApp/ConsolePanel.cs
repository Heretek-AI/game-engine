// ConsolePanel.cs — P3-09: console panel control over ConsoleModel.
//
// A virtualized ListBox bound to the model's Filtered view, with level
// filter toggles (Info/Warning/Error CheckBoxes), a search TextBox, Clear,
// and collapse-duplicates. RefreshFromModel() rebinds after Refresh()/edits.
// Live updates arrive via ConsoleModel.InstallSink + this panel's Refresh.
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Engine.Editor;

namespace Engine.EditorApp;

/// <summary>Console panel: virtualized log list + filter/search/clear/collapse.</summary>
public sealed class ConsolePanel : Border
{
    private readonly ConsoleModel? _model;
    private readonly ListBox _list;
    private readonly CheckBox _infoToggle;
    private readonly CheckBox _warningToggle;
    private readonly CheckBox _errorToggle;
    private readonly TextBox _search;
    private readonly CheckBox _collapse;

    public ConsolePanel()
        : this(null)
    {
    }

    public ConsolePanel(ConsoleModel? model)
    {
        Name = "Console";
        _model = model;

        // ListBox virtualizes via its default VirtualizingStackPanel items
        // host (Avalonia 12.1: no opt-in attached property needed).
        _list = new ListBox();

        _infoToggle = new CheckBox { Content = "Info", IsChecked = true };
        _warningToggle = new CheckBox { Content = "Warning", IsChecked = true };
        _errorToggle = new CheckBox { Content = "Error", IsChecked = true };
        _infoToggle.IsCheckedChanged += (_, _) => ApplyFilters();
        _warningToggle.IsCheckedChanged += (_, _) => ApplyFilters();
        _errorToggle.IsCheckedChanged += (_, _) => ApplyFilters();

        _search = new TextBox();
#pragma warning disable CS0618 // Avalonia 12.1 API: Watermark (PlaceholderText is v13+)
        _search.Watermark = "Search logs…";
#pragma warning restore CS0618
        _search.TextChanged += (_, _) => ApplyFilters();

        var clear = new Button { Content = "Clear" };
        clear.Click += (_, _) => { _model?.Clear(); RefreshFromModel(); };

        _collapse = new CheckBox { Content = "Collapse" };
        _collapse.IsCheckedChanged += (_, _) => ApplyFilters();

        var toolbar = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 8,
            Margin = new Thickness(4),
        };
        toolbar.Children.Add(_infoToggle);
        toolbar.Children.Add(_warningToggle);
        toolbar.Children.Add(_errorToggle);
        toolbar.Children.Add(_search);
        toolbar.Children.Add(clear);
        toolbar.Children.Add(_collapse);

        var layout = new DockPanel();
        DockPanel.SetDock(toolbar, Dock.Top);
        layout.Children.Add(toolbar);
        layout.Children.Add(_list);
        Child = layout;

        RefreshFromModel();
    }

    /// <summary>Rebind the list to the model's current filtered view.</summary>
    public void RefreshFromModel()
    {
        ApplyFilters();
    }

    private void ApplyFilters()
    {
        if (_model == null)
        {
            return;
        }

        _model.ShowInfo = _infoToggle.IsChecked == true;
        _model.ShowWarning = _warningToggle.IsChecked == true;
        _model.ShowError = _errorToggle.IsChecked == true;
        _model.Search = _search.Text ?? string.Empty;
        _model.CollapseDuplicates = _collapse.IsChecked == true;
        var rows = _model.Filtered;
        var items = new string[rows.Count];
        for (var i = 0; i < rows.Count; i++)
        {
            var (entry, repeats) = rows[i];
            items[i] = repeats > 1
                ? $"[{entry.Level}] {entry.Text} (x{repeats})"
                : $"[{entry.Level}] {entry.Text}";
        }

        _list.ItemsSource = items;
    }
}

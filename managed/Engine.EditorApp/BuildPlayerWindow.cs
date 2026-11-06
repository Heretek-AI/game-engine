// BuildPlayerWindow.cs — P7-03: editor Build Player window (code-behind, no AXAML).
//
// Matches the Engine.EditorApp style (MainWindow/ConsolePanel are pure
// code-behind). Opened from File > Build > Build Player. Binds to
// BuildPlayerModel (Engine.Editor, headless-testable): OS selector, config
// selector, output folder picker, Build + Build-And-Run buttons, progress
// bar + log view, last-build status + cold-boot time display.
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Platform.Storage;
using Avalonia.Threading;
using Engine.Editor;

namespace Engine.EditorApp;

/// <summary>
/// Build Player window: target OS (Windows/Linux/macOS), configuration
/// (Release/Debug), output folder, Build + Build-And-Run, progress + log,
/// last-build status + cold-boot display. All controls disable while a
/// build is in flight; errors surface in the log + status line.
/// </summary>
public sealed class BuildPlayerWindow : Window
{
    public BuildPlayerModel Model { get; }

    public ComboBox OsSelector { get; } = new() { Name = "OsSelector" };
    public ComboBox ConfigSelector { get; } = new() { Name = "ConfigSelector" };
    public TextBox OutputDirBox { get; } = new() { Name = "OutputDirBox" };
    public Button BrowseButton { get; } = new() { Name = "BrowseButton", Content = "Browse…" };
    public Button BuildButton { get; } = new() { Name = "BuildButton", Content = "Build" };
    public Button BuildAndRunButton { get; } = new() { Name = "BuildAndRunButton", Content = "Build And Run" };
    public ProgressBar BuildProgress { get; } = new() { Name = "BuildProgress", Minimum = 0, Maximum = 1 };
    public ListBox LogView { get; } = new() { Name = "LogView" };
    public TextBlock StatusText { get; } = new() { Name = "StatusText" };
    public TextBlock ColdBootText { get; } = new() { Name = "ColdBootText" };

    public BuildPlayerWindow()
        : this(new BuildPlayerModel())
    {
    }

    public BuildPlayerWindow(BuildPlayerModel model)
    {
        Model = model ?? throw new ArgumentNullException(nameof(model));
        Title = "Build Player";
        Width = 560;
        Height = 480;

        OsSelector.ItemsSource = Enum.GetNames<PlayerTargetOs>();
        OsSelector.SelectedItem = Model.TargetOs.ToString();
        OsSelector.SelectionChanged += (_, _) =>
        {
            if (Enum.TryParse<PlayerTargetOs>((string?)OsSelector.SelectedItem, out var os))
            {
                Model.TargetOs = os;
                Refresh();
            }
        };

        ConfigSelector.ItemsSource = Enum.GetNames<PlayerBuildConfig>();
        ConfigSelector.SelectedItem = Model.Config.ToString();
        ConfigSelector.SelectionChanged += (_, _) =>
        {
            if (Enum.TryParse<PlayerBuildConfig>((string?)ConfigSelector.SelectedItem, out var cfg))
            {
                Model.Config = cfg;
                Refresh();
            }
        };

        OutputDirBox.Text = Model.OutputDir;
        OutputDirBox.TextChanged += (_, _) => { Model.OutputDir = OutputDirBox.Text ?? string.Empty; Refresh(); };
        BrowseButton.Click += async (_, _) => { await PickOutputFolderAsync().ConfigureAwait(false); };

        BuildButton.Click += async (_, _) => { await RunBuildAsync(andRun: false).ConfigureAwait(false); };
        BuildAndRunButton.Click += async (_, _) => { await RunBuildAsync(andRun: true).ConfigureAwait(false); };

        Model.Changed += OnModelChanged;

        var grid = new Grid { Margin = new Thickness(12) };
        grid.ColumnDefinitions.Add(new ColumnDefinition(GridLength.Auto));
        grid.ColumnDefinitions.Add(new ColumnDefinition(GridLength.Star));
        grid.ColumnDefinitions.Add(new ColumnDefinition(GridLength.Auto));
        for (var i = 0; i < 7; i++)
        {
            grid.RowDefinitions.Add(new RowDefinition(GridLength.Auto));
        }

        Place(grid, 0, new TextBlock { Text = "Target OS:", VerticalAlignment = VerticalAlignment.Center }, OsSelector);
        Place(grid, 1, new TextBlock { Text = "Configuration:", VerticalAlignment = VerticalAlignment.Center }, ConfigSelector);
        Place(grid, 2, new TextBlock { Text = "Output folder:", VerticalAlignment = VerticalAlignment.Center }, OutputDirBox, BrowseButton);

        var buttons = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        buttons.Children.Add(BuildButton);
        buttons.Children.Add(BuildAndRunButton);
        Grid.SetRow(buttons, 3);
        Grid.SetColumnSpan(buttons, 3);
        grid.Children.Add(buttons);

        BuildProgress.Height = 16;
        Grid.SetRow(BuildProgress, 4);
        Grid.SetColumnSpan(BuildProgress, 3);
        grid.Children.Add(BuildProgress);

        LogView.Height = 220;
        Grid.SetRow(LogView, 5);
        Grid.SetColumnSpan(LogView, 3);
        grid.Children.Add(LogView);

        var status = new StackPanel { Spacing = 2 };
        status.Children.Add(StatusText);
        status.Children.Add(ColdBootText);
        Grid.SetRow(status, 6);
        Grid.SetColumnSpan(status, 3);
        grid.Children.Add(status);

        Content = new ScrollViewer { Content = grid };
        Refresh();
        Closed += (_, _) => Model.Changed -= OnModelChanged;
    }

    /// <summary>Re-read the model into every control (call on the UI thread).</summary>
    public void Refresh()
    {
        var busy = Model.State == BuildPlayerState.Building;
        OsSelector.IsEnabled = !busy;
        ConfigSelector.IsEnabled = !busy;
        OutputDirBox.IsEnabled = !busy;
        BrowseButton.IsEnabled = !busy;
        BuildButton.IsEnabled = !busy && Model.CanBuild;
        BuildAndRunButton.IsEnabled = !busy && Model.CanBuild;
        BuildProgress.Value = Model.Progress;
        LogView.ItemsSource = Model.LogLines.ToArray();
        StatusText.Text = Model.StatusLine;
        var r = Model.LastResult;
        ColdBootText.Text = r is null
            ? "Cold boot: —"
            : $"Cold boot: {r.ColdBootSeconds:F2}s · {r.PlayerPath}";
    }

    private static void Place(Grid grid, int row, Control label, Control field, Control? extra = null)
    {
        Grid.SetRow(label, row);
        Grid.SetColumn(label, 0);
        grid.Children.Add(label);
        Grid.SetRow(field, row);
        Grid.SetColumn(field, 1);
        grid.Children.Add(field);
        if (extra is not null)
        {
            Grid.SetRow(extra, row);
            Grid.SetColumn(extra, 2);
            grid.Children.Add(extra);
        }
        else
        {
            Grid.SetColumnSpan(field, 2);
        }
    }

    private void OnModelChanged()
    {
        if (Dispatcher.UIThread.CheckAccess())
        {
            Refresh();
        }
        else
        {
            Dispatcher.UIThread.Post(Refresh);
        }
    }

    private async Task RunBuildAsync(bool andRun)
    {
        Refresh(); // disable controls immediately
        await Model.BuildAsync(andRun).ConfigureAwait(false);
        if (!Dispatcher.UIThread.CheckAccess())
        {
            await Dispatcher.UIThread.InvokeAsync(Refresh);
        }
        else
        {
            Refresh();
        }
    }

    private async Task PickOutputFolderAsync()
    {
        var sp = TopLevel.GetTopLevel(this)?.StorageProvider;
        if (sp is null)
        {
            return;
        }

        var picked = await sp.OpenFolderPickerAsync(new FolderPickerOpenOptions
        {
            Title = "Choose build output folder",
            AllowMultiple = false,
        }).ConfigureAwait(false);
        var dir = picked.Count > 0 ? picked[0].TryGetLocalPath() : null;
        if (string.IsNullOrEmpty(dir))
        {
            return;
        }

        Model.OutputDir = dir;
        await Dispatcher.UIThread.InvokeAsync(() =>
        {
            OutputDirBox.Text = dir;
            Refresh();
        });
    }
}

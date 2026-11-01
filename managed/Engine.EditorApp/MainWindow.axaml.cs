// MainWindow.axaml.cs — P3-01: editor shell: menu bar (File, Edit,
// GameObject, Window, Help — stubbed) + six empty dock panels in the
// default layout (Hierarchy | Inspector | Scene | Game | Project | Console).
// Panels are plain Borders arranged by nested splitters; drag-to-dock
// arrives with the docking control in a later P3 slice.
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;

namespace Engine.EditorApp;

public sealed class MainWindow : Window
{
    public static readonly string[] MenuTitles =
        ["File", "Edit", "GameObject", "Window", "Help"];

    public static readonly string[] PanelNames =
        ["Hierarchy", "Inspector", "Scene", "Game", "Project", "Console", "Profiler"];

    public MainWindow()
    {
        Title = "Engine Editor";
        Width = 1280;
        Height = 720;
        FontFamily = new FontFamily("fonts:Inter");

        var menu = new Menu();
        foreach (var title in MenuTitles)
        {
            menu.Items.Add(new MenuItem { Header = title });
        }

        DockPanel.SetDock(menu, Dock.Top);

        var root = new DockPanel();
        root.Children.Add(menu);

        var hierarchy = Panel("Hierarchy");
        var inspector = Panel("Inspector");
        var scene = Panel("Scene");
        var game = Panel("Game");
        var project = Panel("Project");
        var console = new ConsolePanel();
        var profiler = new ProfilerPanel();

        var center = new Grid
        {
            ColumnDefinitions = new ColumnDefinitions("*,Auto,*"),
        };
        Grid.SetColumn(scene, 0);
        center.Children.Add(scene);
        var centerSplit = new GridSplitter { ResizeDirection = GridResizeDirection.Columns };
        Grid.SetColumn(centerSplit, 1);
        center.Children.Add(centerSplit);
        Grid.SetColumn(game, 2);
        center.Children.Add(game);

        var middle = new Grid
        {
            ColumnDefinitions = new ColumnDefinitions("220,Auto,*,Auto,260"),
        };
        Grid.SetColumn(hierarchy, 0);
        middle.Children.Add(hierarchy);
        var split0 = new GridSplitter { ResizeDirection = GridResizeDirection.Columns };
        Grid.SetColumn(split0, 1);
        middle.Children.Add(split0);
        Grid.SetColumn(center, 2);
        middle.Children.Add(center);
        var split1 = new GridSplitter { ResizeDirection = GridResizeDirection.Columns };
        Grid.SetColumn(split1, 3);
        middle.Children.Add(split1);
        Grid.SetColumn(inspector, 4);
        middle.Children.Add(inspector);

        var lower = new Grid
        {
            ColumnDefinitions = new ColumnDefinitions("*,Auto,*,Auto,*"),
        };
        Grid.SetColumn(project, 0);
        lower.Children.Add(project);
        var split2 = new GridSplitter { ResizeDirection = GridResizeDirection.Columns };
        Grid.SetColumn(split2, 1);
        lower.Children.Add(split2);
        Grid.SetColumn(console, 2);
        lower.Children.Add(console);
        var split3 = new GridSplitter { ResizeDirection = GridResizeDirection.Columns };
        Grid.SetColumn(split3, 3);
        lower.Children.Add(split3);
        Grid.SetColumn(profiler, 4);
        lower.Children.Add(profiler);

        var body = new Grid
        {
            RowDefinitions = new RowDefinitions("*,Auto,180"),
        };
        Grid.SetRow(middle, 0);
        body.Children.Add(middle);
        var bodySplit = new GridSplitter { ResizeDirection = GridResizeDirection.Rows };
        Grid.SetRow(bodySplit, 1);
        body.Children.Add(bodySplit);
        Grid.SetRow(lower, 2);
        body.Children.Add(lower);

        root.Children.Add(body);
        Content = root;
    }

    private static Border Panel(string name)
    {
        return new Border
        {
            Name = name,
            Child = new TextBlock { Text = name, Margin = new Thickness(8) },
        };
    }
}

// MainWindow.axaml.cs — P3-12: editor shell over Dock.Avalonia.
//
// Menu bar (File, Edit, GameObject, Window, Help) + a DockControl whose
// default layout matches P3-01 (Hierarchy left, Scene+Game center,
// Inspector right, Project+Console+Profiler bottom). Panels dock, tab,
// float and close. Window > Reset Layout rebuilds the default.
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Dock.Avalonia.Controls;
using Dock.Model.Controls;

namespace Engine.EditorApp;

public sealed class MainWindow : Window
{
    public static readonly string[] MenuTitles =
        ["File", "Edit", "GameObject", "Window", "Help"];

    public static readonly string[] PanelNames =
        ["Hierarchy", "Inspector", "Scene", "Game", "Project", "Console", "Profiler"];

    public EditorLayoutService Layouts { get; }

    public IRootDock Layout => Layouts.Layout;

    public DockControl DockView { get; }

    public MainWindow()
        : this(new EditorLayoutService(new EditorDockFactory(
            Panel("Hierarchy"),
            Panel("Inspector"),
            Panel("Scene"),
            Panel("Game"),
            Panel("Project"),
            new ConsolePanel(),
            new ProfilerPanel())))
    {
    }

    public MainWindow(EditorLayoutService layouts)
    {
        Layouts = layouts;
        Title = "Engine Editor";
        Width = 1280;
        Height = 720;
        FontFamily = new FontFamily("fonts:Inter");

        var menu = new Menu();
        foreach (var title in MenuTitles)
        {
            menu.Items.Add(new MenuItem { Header = title });
        }

        foreach (var item in menu.Items)
        {
            if (item is MenuItem mi && (string?)mi.Header == "Window")
            {
                mi.Items.Add(new MenuItem { Header = "Reset Layout", Name = "ResetLayout" });
            }
        }

        DockPanel.SetDock(menu, Avalonia.Controls.Dock.Top);

        var root = new Avalonia.Controls.DockPanel();
        root.Children.Add(menu);

        layouts.CreateDefault();
        DataTemplates.Add(layouts.Factory.CreateViewLocator());
        DockView = new DockControl
        {
            Name = "Dock",
            Layout = Layouts.Layout,
        };

        foreach (var item in menu.Items)
        {
            if (item is MenuItem mi && (string?)mi.Header == "Window")
            {
                foreach (var sub in mi.Items)
                {
                    if (sub is MenuItem reset && reset.Name == "ResetLayout")
                    {
                        reset.Click += (_, _) =>
                        {
                            DockView.Layout = layouts.ResetLayout();
                        };
                    }
                }
            }
        }

        root.Children.Add(DockView);
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

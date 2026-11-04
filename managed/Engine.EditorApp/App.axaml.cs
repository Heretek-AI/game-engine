// App.axaml.cs — P3-12: Avalonia application bootstrap for engine-editor.
// Fluent theme + Dock fluent theme + bundled Inter font so goldens never
// depend on machine fonts. DockControl renders nothing without its theme.
// The EditorViewLocator (tool id → panel Control) is registered per-window
// by MainWindow because App has no access to the panels.
using Avalonia;
using Avalonia.Markup.Xaml;
using Avalonia.Styling;
using Dock.Avalonia.Themes.Fluent;

namespace Engine.EditorApp;

public sealed class App : Application
{
    public override void Initialize()
    {
        Styles.Add(new Avalonia.Themes.Fluent.FluentTheme());
        Styles.Add(new DockFluentTheme());
    }

    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<App>()
            .UsePlatformDetect()
            .WithInterFont()
            .LogToTrace();
}

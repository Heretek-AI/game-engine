// App.axaml.cs — P3-01: Avalonia application bootstrap for engine-editor.
// Fluent theme + bundled Inter font so goldens never depend on machine fonts.
using Avalonia;
using Avalonia.Markup.Xaml;
using Avalonia.Styling;

namespace Engine.EditorApp;

public sealed class App : Application
{
    public override void Initialize()
    {
        Styles.Add(new Avalonia.Themes.Fluent.FluentTheme());
    }

    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<App>()
            .UsePlatformDetect()
            .WithInterFont()
            .LogToTrace();
}

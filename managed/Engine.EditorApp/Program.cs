// Program.cs — P3-01: engine-editor entry point.
using Avalonia;

namespace Engine.EditorApp;

internal static class Program
{
    public static int Main(string[] args)
    {
        return App.BuildAvaloniaApp()
            .StartWithClassicDesktopLifetime(args);
    }
}

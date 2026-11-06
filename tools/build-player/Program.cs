// Program.cs — build-player CLI (P7-03): headless player build entry.
//
//   build-player --target win-x64|linux-x64|osx-x64|osx-arm64
//                --project MarbleRun --out dist/
//                [--configuration Release] [--no-native] [--no-game-build]
//
// Thin wrapper over the invocable Engine.Editor.PlayerBuild API: target
// validation, --project/--out handling, dist/<target>/ layout. Exit codes:
// 0 ok; 1 usage/argument error; 2 stage failure (message on stderr).
using Engine.Editor;

internal static class Program
{
    internal static int Main(string[] args)
    {
        if (args.Length == 0 || args[0] is "-h" or "--help" or "help")
        {
            PrintUsage();
            return args.Length == 0 ? 2 : 0;
        }

        var opts = new PlayerBuildOptions();
        for (var i = 0; i < args.Length; i++)
        {
            switch (args[i])
            {
                case "--target" when i + 1 < args.Length:
                    opts.Target = args[++i];
                    break;
                case "--project" when i + 1 < args.Length:
                    opts.Project = args[++i];
                    break;
                case "--out" when i + 1 < args.Length:
                    opts.OutDir = args[++i];
                    break;
                case "--configuration" when i + 1 < args.Length:
                    opts.Configuration = args[++i];
                    break;
                case "--no-native":
                    opts.SkipNative = true;
                    break;
                case "--no-game-build":
                    opts.SkipGameBuild = true;
                    break;
                default:
                    Console.Error.WriteLine($"build-player: bad argument '{args[i]}'.");
                    PrintUsage();
                    return 1;
            }
        }

        try
        {
            var result = PlayerBuild.Build(opts);
            Console.WriteLine($"build-player: target={result.Target} project={result.ProjectName}");
            Console.WriteLine($"build-player: out={result.OutDir}");
            Console.WriteLine($"build-player: native={(result.NativeBuilt ? "built" : result.NativeConfiguredOnly ? "configured-only" : "skipped")}"
                + (result.PlayerBinary != null ? $" ({Path.GetFileName(result.PlayerBinary)})" : string.Empty));
            Console.WriteLine($"build-player: game={Path.GetFileName(result.GameAssembly)} pak=game.pak entries={result.PakEntries}");
            Console.WriteLine($"build-player: manifest={result.ManifestPath}");
            return 0;
        }
        catch (ArgumentException ex)
        {
            Console.Error.WriteLine($"build-player: {ex.Message}");
            return 1;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"build-player: FAILED: {ex.Message}");
            return 2;
        }
    }

    private static void PrintUsage()
    {
        Console.WriteLine("usage: build-player --target <rid> --project <name|dir> --out <dir>");
        Console.WriteLine("  --target win-x64|linux-x64|osx-x64|osx-arm64");
        Console.WriteLine("  --project MarbleRun | <project dir>   (default: MarbleRun)");
        Console.WriteLine("  --out dist/                           (layout: <out>/<target>/)");
        Console.WriteLine("  --configuration Release");
        Console.WriteLine("  --no-native       skip cmake engine-player stage");
        Console.WriteLine("  --no-game-build   reuse prebuilt game assembly");
        Console.WriteLine("Equivalent invocable API: Engine.Editor.PlayerBuild.Build(new PlayerBuildOptions { ... }).");
    }
}

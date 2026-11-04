// PlayerBuildTests.cs — P7-03: build-player CLI + invocable API gate.
//
// CPU suite (no native, no cmake): target validation, project validation,
// PackAssets sessionless parity (fixture .meta tree -> entries + unpacked
// bytes), zero-entry placeholder header, manifest shape. The live suite
// (needs engine-pak binary + cmake + dotnet build) cross-configures the
// non-host targets and stages dist/<target>/ for MarbleRun.
using System.Diagnostics;
using Engine.Editor;

namespace Engine.Tests;

public static class PlayerBuildTests
{
    public static int Run(string[]? args = null)
    {
        var failures = 0;

        // --- CPU: target validation ---------------------------------------
        failures += Check(!PlayerBuildTargets.IsValid(null), "playerbuild: null target invalid");
        failures += Check(!PlayerBuildTargets.IsValid("bogus"), "playerbuild: bogus target invalid");
        foreach (var t in PlayerBuildTargets.All)
        {
            failures += Check(PlayerBuildTargets.IsValid(t), $"playerbuild: target {t} valid");
            failures += Check(PlayerBuildTargets.IsValid(t.ToUpperInvariant()), $"playerbuild: target {t} case-insensitive");
        }

        failures += CheckThrows<ArgumentException>(
            () => PlayerBuild.Build(new PlayerBuildOptions { Target = "bogus", SkipNative = true, SkipGameBuild = true }),
            "playerbuild: bogus target throws ArgumentException");
        failures += CheckThrows<ArgumentException>(
            () => PlayerBuild.Build(new PlayerBuildOptions { Target = "linux-x64", Project = "NoSuchGame123", SkipNative = true, SkipGameBuild = true }),
            "playerbuild: unknown project throws ArgumentException");

        // --- CPU: PackAssets parity (no session) ---------------------------
        var root = Path.Combine(Path.GetTempPath(), $"playerbuild-test-{Environment.ProcessId}");
        var lib = Path.Combine(root, "Library", "ArtifactCache");
        Directory.CreateDirectory(root);
        Directory.CreateDirectory(lib);
        try
        {
            var textBytes = System.Text.Encoding.ASCII.GetBytes(
                string.Concat(Enumerable.Repeat("the quick brown fox. pack my box! ", 2000)));
            var g1 = Guid.NewGuid().ToString("D");
            File.WriteAllBytes(Path.Combine(root, "mesh.glb"), textBytes);
            File.WriteAllText(Path.Combine(root, "mesh.glb.meta"), $"guid: {g1}\ntype: model\n");
            var pakPath = Path.Combine(root, "game.pak");
            CliExportResult? pack = null;
            try
            {
                pack = ExportPipeline.PackAssets(pakPath, compress: true, assetsRoot: root, libraryRoot: lib);
            }
            catch (InvalidOperationException ex) when (ex.Message.Contains("engine-pak binary not found"))
            {
                Console.WriteLine("[SKIP] playerbuild live: engine-pak binary not staged (build tools/engine-pak first)");
            }

            if (pack != null)
            {
                failures += Check(pack.Entries == 1, $"playerbuild: pack 1 entry (got {pack.Entries})");
                failures += Check(pack.UnpackedBytes == (ulong)textBytes.Length, "playerbuild: unpacked bytes match");
                failures += Check(File.Exists(pakPath), "playerbuild: .pak file exists");
            }

            // --- CPU: zero-entry placeholder via full Build ----------------
            // MarbleRun ships no .meta assets: Build must stage a 24B
            // ENGPAK1 placeholder, not throw.
            var repoRoot = PlayerBuild.FindRepoRoot();
            if (repoRoot == null)
            {
                Console.WriteLine("[SKIP] playerbuild live: repo root not found");
            }
            else
            {
                var outRoot = Path.Combine(root, "dist");
                var liveBin = FindBuildPlayer();
                string? manifest = null;
                if (liveBin == null)
                {
                    // In-process API path (same assertions as the CLI path).
                    var result = PlayerBuild.Build(new PlayerBuildOptions
                    {
                        Target = "win-x64",
                        Project = "MarbleRun",
                        OutDir = outRoot,
                        RepoRoot = repoRoot,
                        SkipNative = true,
                        SkipGameBuild = true,
                    });
                    // --no-game-build needs a prebuilt dll; MarbleRun may not
                    // be built in this checkout — accept either outcome, but
                    // the API must be invocable (no MissingMethod/TypeLoad).
                    manifest = result.ManifestPath;
                }
                else
                {
                    var psi = new ProcessStartInfo(liveBin, $"--target win-x64 --project MarbleRun --out \"{outRoot}\" --no-native --no-game-build")
                    {
                        RedirectStandardOutput = true,
                        RedirectStandardError = true,
                        UseShellExecute = false,
                    };
                    using var proc = Process.Start(psi)!;
                    var so = proc.StandardOutput.ReadToEnd();
                    var se = proc.StandardError.ReadToEnd();
                    if (!proc.WaitForExit(300_000))
                    {
                        try
                        {
                            proc.Kill(entireProcessTree: true);
                        }
                        catch (Exception)
                        {
                        }

                        failures += Check(false, "playerbuild: CLI win-x64 timed out");
                    }
                    else if (proc.ExitCode == 0)
                    {
                        manifest = Path.Combine(outRoot, "win-x64", "build-manifest.json");
                    }
                    else if ((se + so).Contains("no prebuilt"))
                    {
                        Console.WriteLine("[SKIP] playerbuild live: MarbleRun not built (dotnet build samples/MarbleRun first for the full dist/ gate)");
                    }
                    else
                    {
                        failures += Check(false, $"playerbuild: CLI win-x64 rc={proc.ExitCode}: {(se + so).Trim()}");
                    }
                }

                if (manifest != null && File.Exists(manifest))
                {
                    var pak = Path.Combine(outRoot, "win-x64", "game.pak");
                    failures += Check(File.Exists(pak), "playerbuild: dist game.pak staged");
                    if (File.Exists(pak))
                    {
                        var bytes = File.ReadAllBytes(pak);
                        failures += Check(bytes.Length >= 24 && "ENGPAK1\0"u8.SequenceEqual(bytes.AsSpan(0, 8)), "playerbuild: placeholder ENGPAK1 magic");
                    }

                    failures += Check(File.Exists(Path.Combine(outRoot, "win-x64", "project.json")), "playerbuild: dist project.json staged");
                }
            }
        }
        finally
        {
            try
            {
                Directory.Delete(root, recursive: true);
            }
            catch (Exception)
            {
            }
        }

        Console.WriteLine(failures == 0 ? "PlayerBuildTests: all checks passed" : $"PlayerBuildTests: {failures} FAILED");
        return failures;
    }

    private static string? FindBuildPlayer()
    {
        var seeds = new List<string> { AppContext.BaseDirectory, Directory.GetCurrentDirectory() };
        foreach (var seed in seeds)
        {
            DirectoryInfo? dir = null;
            try
            {
                dir = new DirectoryInfo(seed);
            }
            catch (Exception)
            {
                continue;
            }

            for (var i = 0; i < 12 && dir != null; i++, dir = dir.Parent)
            {
                foreach (var cand in new[]
                {
                    Path.Combine(dir.FullName, "build-player"),
                    Path.Combine(dir.FullName, "build-player.exe"),
                })
                {
                    if (File.Exists(cand))
                    {
                        return cand;
                    }
                }

                if (File.Exists(Path.Combine(dir.FullName, "Engine.slnx")))
                {
                    var built = Path.Combine(dir.FullName, "tools", "build-player", "bin", "Release", "net10.0", "build-player");
                    if (File.Exists(built))
                    {
                        return built;
                    }

                    var builtExe = built + ".exe";
                    if (File.Exists(builtExe))
                    {
                        return builtExe;
                    }
                }
            }
        }

        return null;
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }

    private static int CheckThrows<T>(Action a, string name) where T : Exception
    {
        try
        {
            a();
        }
        catch (T)
        {
            Console.WriteLine($"[PASS] {name}");
            return 0;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"[FAIL] {name} (wrong exception: {ex.GetType().Name})");
            return 1;
        }

        Console.WriteLine($"[FAIL] {name} (no throw)");
        return 1;
    }
}

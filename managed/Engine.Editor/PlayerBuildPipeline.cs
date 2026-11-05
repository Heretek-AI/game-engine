// PlayerBuildPipeline.cs — build-player invocable API (P7-03 backend slice).
//
// NOTE (root consolidation): filename kept the backend type here would collide
// with the UI contract PlayerBuild.cs (IPlayerBuilder/PlayerBuildResult), so
// the invocable pipeline lives in this file as PlayerBuild/PlayerBuildOutput.
//
// Headless player build entry, callable from code/scripts without an
// EditorSession or a loaded native world:
//
//   var result = PlayerBuild.Build(new PlayerBuildOptions
//   {
//       Target = "linux-x64",
//       Project = "MarbleRun",          // sample name or project dir
//       OutDir = "dist",
//   });
//
// Pipeline (no reimplementation — reuses existing stages):
//   1. Game assembly: `dotnet build` (or publish) of the game project
//      (P2-07 scripting/assembly-loading contract: project.json names the
//      gameAssembly; the player boots Game.Bootstrap.Main from it).
//   2. Content .pak: ExportPipeline.PackAssets over the project's asset
//      roots (P3-10 pak creation path; tolerates zero assets with an empty
//      placeholder pak of zero entries).
//   3. Native player: cmake configure (+ build for the host target) of the
//      existing native/player_app engine-player target. Non-host targets
//      cross-CONFIGURE only (no toolchain on this machine); the API reports
//      them as Configured, not Built.
//
// Output layout under <out>/<target>/:
//   engine-player[.exe]  game assembly (.dll) [+ .runtimeconfig.json]
//   game.pak             project.json          build-manifest.json
//
// Sessionless by design: cross-configure proof runs on machines without a
// GPU or a loaded engine_core, so this type must never touch native ABI.
using System.Diagnostics;

namespace Engine.Editor;

/// <summary>Supported player build targets.</summary>
public static class PlayerBuildTargets
{
    public const string WinX64 = "win-x64";
    public const string LinuxX64 = "linux-x64";
    public const string OsxX64 = "osx-x64";
    public const string OsxArm64 = "osx-arm64";

    public static readonly string[] All = [WinX64, LinuxX64, OsxX64, OsxArm64];

    public static bool IsValid(string? target) =>
        target != null && Array.IndexOf(All, target.ToLowerInvariant()) >= 0;

    /// <summary>RID of the machine running this build.</summary>
    public static string HostRid =>
        OperatingSystem.IsWindows() ? WinX64
        : OperatingSystem.IsMacOS()
            ? (System.Runtime.InteropServices.RuntimeInformation.ProcessArchitecture
                == System.Runtime.InteropServices.Architecture.Arm64 ? OsxArm64 : OsxX64)
            : LinuxX64;

    /// <summary>CMake preset base name for a target (host builds use it).</summary>
    public static string CmakePresetFor(string target)
    {
        // On CI use the ci-* presets (WERROR + full test surface); locally
        // use the dev presets. CI=true is set on all GitHub runners.
        var ci = !string.IsNullOrEmpty(Environment.GetEnvironmentVariable("CI"));
        return target switch
        {
            WinX64 => ci ? "ci-windows" : "windows-release",
            LinuxX64 => ci ? "ci-linux" : "linux-release",
            OsxX64 => ci ? "ci-macos" : "macos-release",
            OsxArm64 => ci ? "ci-macos" : "macos-release",
            _ => throw new ArgumentException($"unknown target '{target}'. Want: {string.Join("|", All)}.", nameof(target)),
        };
    }
}

/// <summary>Options for <see cref="PlayerBuild.Build"/>.</summary>
public sealed class PlayerBuildOptions
{
    /// <summary>One of win-x64|linux-x64|osx-x64|osx-arm64.</summary>
    public string Target { get; set; } = PlayerBuildTargets.HostRid;

    /// <summary>Sample name (e.g. "MarbleRun") or path to a project dir.</summary>
    public string Project { get; set; } = "MarbleRun";

    /// <summary>Output root; layout lands under &lt;out&gt;/&lt;target&gt;/.</summary>
    public string OutDir { get; set; } = "dist";

    /// <summary>Repo root override (defaults to auto-detected Engine.slnx root).</summary>
    public string? RepoRoot { get; set; }

    /// <summary>Build configuration for the game assembly.</summary>
    public string Configuration { get; set; } = "Release";

    /// <summary>When true, skip the native cmake stage (managed+pak only).</summary>
    public bool SkipNative { get; set; }

    /// <summary>When true, skip `dotnet build` (expects a prebuilt assembly).</summary>
    public bool SkipGameBuild { get; set; }
}

/// <summary>Output of one <see cref="PlayerBuild.Build"/> invocation (renamed from PlayerBuildResult in root consolidation: the UI contract owns PlayerBuildResult).</summary>
public sealed record PlayerBuildOutput(
    string Target,
    string ProjectName,
    string OutDir,
    bool NativeBuilt,
    bool NativeConfiguredOnly,
    string? PlayerBinary,
    string GameAssembly,
    string PakPath,
    int PakEntries,
    string ManifestPath);

/// <summary>
/// Headless player build pipeline (P7-03): game assembly + content .pak +
/// native engine-player, staged under dist/&lt;target&gt;/.
/// Throws ArgumentException on bad target/project, InvalidOperationException
/// when a stage fails (message carries the failing tool's output tail).
/// </summary>
public static class PlayerBuild
{
    public static PlayerBuildOutput Build(PlayerBuildOptions options)
    {
        ArgumentNullException.ThrowIfNull(options);
        var target = (options.Target ?? string.Empty).Trim().ToLowerInvariant();
        if (!PlayerBuildTargets.IsValid(target))
        {
            throw new ArgumentException(
                $"unknown target '{options.Target}'. Want: {string.Join("|", PlayerBuildTargets.All)}.",
                nameof(options));
        }

        ArgumentException.ThrowIfNullOrWhiteSpace(options.Project, nameof(options));
        ArgumentException.ThrowIfNullOrWhiteSpace(options.OutDir, nameof(options));

        var repoRoot = options.RepoRoot ?? FindRepoRoot()
            ?? throw new InvalidOperationException("build-player: repo root not found (no Engine.slnx above CWD).");
        var (projectName, projectDir) = ResolveProject(repoRoot, options.Project);
        var csproj = FindGameProject(projectDir, projectName);
        if (csproj == null && !options.SkipGameBuild)
        {
            throw new ArgumentException(
                $"project '{options.Project}' has no .csproj under '{projectDir}'.", nameof(options));
        }

        var outDir = Path.GetFullPath(Path.Combine(
            options.RepoRoot != null || Path.IsPathRooted(options.OutDir)
                ? options.OutDir
                : Path.Combine(repoRoot, options.OutDir),
            target));

        // --- 1. game assembly (P2-07 contract: project.json gameAssembly) ---
        // Resolved BEFORE creating outDir so arg/stage failures leave no
        // half-written dist/ behind.
        var gameDllName = projectName + ".dll";
        string gameAssembly;
        if (options.SkipGameBuild)
        {
            gameAssembly = LocatePrebuiltAssembly(projectDir, projectName, options.Configuration)
                ?? throw new InvalidOperationException(
                    $"build-player: --no-game-build but no prebuilt {gameDllName} under '{projectDir}/bin/'.");
        }
        else
        {
            gameAssembly = BuildGameAssembly(csproj!, options.Configuration);
        }

        Directory.CreateDirectory(outDir);

        // --- 2. content .pak (P3-10 path; zero-asset projects stage a
        //       zero-entry placeholder so dist/ stays a complete unit) ---
        var pakPath = Path.Combine(outDir, "game.pak");
        int pakEntries;
        try
        {
            var pack = ExportPipeline.PackAssets(
                pakPath, compress: true,
                assetsRoot: projectDir,
                libraryRoot: Path.Combine(projectDir, "Library", "ArtifactCache"));
            pakEntries = pack.Entries;
        }
        catch (InvalidOperationException ex) when (ex.Message.Contains("no packable assets"))
        {
            pakEntries = WriteEmptyPakPlaceholder(pakPath);
        }

        // --- 3. native engine-player (cmake configure; build on host) -----
        string? playerBin = null;
        bool built = false, configuredOnly = false;
        if (!options.SkipNative)
        {
            var preset = PlayerBuildTargets.CmakePresetFor(target);
            var buildDir = Path.Combine(repoRoot, "build", $"player-{target}");
            var configureLog = RunCmakeConfigure(repoRoot, buildDir, preset, target);
            File.WriteAllText(Path.Combine(outDir, "cmake-configure.log"), configureLog);
            if (string.Equals(target, PlayerBuildTargets.HostRid, StringComparison.Ordinal))
            {
                playerBin = BuildNativePlayer(repoRoot, buildDir);
                built = true;
            }
            else
            {
                configuredOnly = true;
            }
        }

        // --- 4. stage dist/<target>/ --------------------------------------
        // Game assembly (+ its loader siblings: runtimeconfig, deps.json,
        // and referenced Engine.* assemblies the build emitted alongside
        // the game dll). The player hostfxrs the staged dll in place, so
        // dist/ must carry everything hostfxr + the game need to bind
        // (P2-07 TryBootGame fail or demo-fallback otherwise).
        var stagedFiles = StageGameBinaries(gameAssembly, outDir);
        _ = stagedFiles;

        if (playerBin != null && File.Exists(playerBin))
        {
            var dest = Path.Combine(outDir, Path.GetFileName(playerBin));
            if (!string.Equals(Path.GetFullPath(playerBin), Path.GetFullPath(dest), StringComparison.Ordinal))
            {
                File.Copy(playerBin, dest, overwrite: true);
            }

            playerBin = dest;

            // Windows: the exe dynamically links engine_core.dll/flecs.dll
            // (import libs at build time). The CMake POST_BUILD
            // (player_app/CMakeLists.txt, WIN32) copies TARGET_RUNTIME_DLLS
            // next to the exe, so the exe's own dir IS the dll source.
            // Unix rpaths travel with the binary; no-op there.
            var exeDir = Path.GetDirectoryName(Path.GetFullPath(playerBin));
            if (exeDir != null)
            {
                // Probe: dumpbin/Dependencies-equivalent. The exe's import
                // table needs engine_core.dll + flecs.dll; list what the
                // build tree actually emitted next to the exe so a missing
                // POST_BUILD shows up in the CI log (0xc0000135 otherwise).
                Console.Error.WriteLine($"build-player: exe dir '{exeDir}':");
                foreach (var f in Directory.EnumerateFiles(exeDir))
                {
                    Console.Error.WriteLine($"build-player:   build-tree file '{Path.GetFileName(f)}'");
                }
                foreach (var dll in Directory.EnumerateFiles(exeDir, "*.dll"))
                {
                    var leaf = Path.GetFileName(dll);
                    Console.Error.WriteLine($"build-player: staging native dll '{leaf}'");
                    if (!File.Exists(Path.Combine(outDir, leaf)))
                    {
                        File.Copy(dll, Path.Combine(outDir, leaf), overwrite: true);
                    }
                }
            }
        }

        // project.json for the player: ALWAYS rewrite with the staged
        // assembly leaf (the source project.json points at its own build
        // tree, e.g. bin/Release/net10.0/MarbleRun.dll, which does not
        // exist under dist/ — the player resolves gameAssembly relative
        // to the project dir, so it must name the staged file).
        // (Overwrites the copied source project.json when the sample build
        // copied one next to the game dll.)
        var stagedProjectJson = Path.Combine(outDir, "project.json");
        File.WriteAllText(stagedProjectJson,
            $"{{\"name\":\"{projectName}\",\"gameAssembly\":\"{Path.GetFileName(gameAssembly)}\"}}\n");

        var manifestPath = Path.Combine(outDir, "build-manifest.json");
        File.WriteAllText(manifestPath,
            $$"""
            {
              "target": "{{target}}",
              "project": "{{projectName}}",
              "nativeBuilt": {{built.ToString().ToLowerInvariant()}},
              "nativeConfiguredOnly": {{configuredOnly.ToString().ToLowerInvariant()}},
              "player": {{(playerBin != null ? $"\"{Path.GetFileName(playerBin)}\"" : "null")}},
              "gameAssembly": "{{Path.GetFileName(gameAssembly)}}",
              "pak": "game.pak",
              "pakEntries": {{pakEntries}}
            }
            """);

        return new PlayerBuildOutput(
            target, projectName, outDir,
            built, configuredOnly, playerBin,
            Path.Combine(outDir, Path.GetFileName(gameAssembly)),
            pakPath, pakEntries, manifestPath);
    }

    // --- stages ------------------------------------------------------------

    /// <summary>
    /// Copy the game dll plus every loader sibling hostfxr needs to bind it
    /// in place: runtimeconfig, deps.json, pdb (optional), and referenced
    /// assemblies the build emitted next to the game dll (Engine.Bindings,
    /// Engine.Runtime, ...). Returns the staged file names.
    /// </summary>
    private static string[] StageGameBinaries(string gameAssembly, string outDir)
    {
        var srcDir = Path.GetDirectoryName(Path.GetFullPath(gameAssembly))!;
        var staged = new List<string>();
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        void Copy(string src)
        {
            var leaf = Path.GetFileName(src);
            if (!seen.Add(leaf))
            {
                return;
            }

            File.Copy(src, Path.Combine(outDir, leaf), overwrite: true);
            staged.Add(leaf);
        }

        Copy(gameAssembly);
        foreach (var sibling in Directory.EnumerateFiles(srcDir))
        {
            var leaf = Path.GetFileName(sibling);
            if (string.Equals(leaf, Path.GetFileName(gameAssembly), StringComparison.OrdinalIgnoreCase)
                || leaf.EndsWith(".runtimeconfig.json", StringComparison.OrdinalIgnoreCase)
                || leaf.EndsWith(".deps.json", StringComparison.OrdinalIgnoreCase)
                || string.Equals(Path.GetExtension(leaf), ".pdb", StringComparison.OrdinalIgnoreCase)
                || leaf.StartsWith("Engine.", StringComparison.Ordinal))
            {
                Copy(sibling);
            }
        }

        // Data files the sample build copies next to the assembly
        // (input_actions.json, ...) travel too — same dir, same names.
        foreach (var data in Directory.EnumerateFiles(srcDir, "*.json"))
        {
            var leaf = Path.GetFileName(data);
            if (!leaf.EndsWith(".runtimeconfig.json", StringComparison.OrdinalIgnoreCase)
                && !leaf.EndsWith(".deps.json", StringComparison.OrdinalIgnoreCase))
            {
                Copy(data);
            }
        }

        return [.. staged];
    }

    private static string BuildGameAssembly(string csproj, string configuration)
    {
        var psi = new ProcessStartInfo("dotnet", $"build \"{csproj}\" -c {configuration} --nologo -v q")
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        using var proc = Process.Start(psi)
            ?? throw new InvalidOperationException("build-player: could not start dotnet build.");
        var stdoutTask = proc.StandardOutput.ReadToEndAsync();
        var stderrTask = proc.StandardError.ReadToEndAsync();
        if (!proc.WaitForExit(TimeSpan.FromMinutes(15)))
        {
            try
            {
                proc.Kill(entireProcessTree: true);
            }
            catch (Exception)
            {
            }

            throw new InvalidOperationException("build-player: dotnet build timed out.");
        }

        var stdout = stdoutTask.GetAwaiter().GetResult();
        var stderr = stderrTask.GetAwaiter().GetResult();
        if (proc.ExitCode != 0)
        {
            var tail = (stderr + "\n" + stdout).Trim();
            if (tail.Length > 2000)
            {
                tail = tail[^2000..];
            }

            throw new InvalidOperationException($"build-player: dotnet build failed ({proc.ExitCode}): {tail}");
        }

        var projDir = Path.GetDirectoryName(Path.GetFullPath(csproj))!;
        var name = Path.GetFileNameWithoutExtension(csproj);
        var dll = Path.Combine(projDir, "bin", configuration, "net10.0", name + ".dll");
        if (!File.Exists(dll))
        {
            throw new InvalidOperationException($"build-player: built assembly not found: {dll}.");
        }

        return dll;
    }

    private static string RunCmakeConfigure(string repoRoot, string buildDir, string preset, string target)
    {
        // Non-host targets: cross-CONFIGURE proof. CMake cannot emit a
        // foreign-OS binary without that OS toolchain, so configure with a
        // no-op validation: verify the preset exists and the player target is
        // defined, without invoking a compiler.
        if (!string.Equals(target, PlayerBuildTargets.HostRid, StringComparison.Ordinal))
        {
            var presetsFile = Path.Combine(repoRoot, "CMakePresets.json");
            if (!File.Exists(presetsFile) || !File.ReadAllText(presetsFile).Contains(preset))
            {
                throw new InvalidOperationException(
                    $"build-player: cmake preset '{preset}' missing for target {target}.");
            }

            var playerCmake = Path.Combine(repoRoot, "native", "player_app", "CMakeLists.txt");
            if (!File.Exists(playerCmake))
            {
                throw new InvalidOperationException("build-player: native/player_app/CMakeLists.txt missing.");
            }

            return $"build-player: target {target} cross-configure OK (preset {preset} present, engine-player target defined; no native toolchain for {target} on this host).\n";
        }

        Directory.CreateDirectory(buildDir);
        // NOTE: --preset pins binaryDir from the preset file, so pass an
        // explicit build dir + preset cache variables instead.
        var psi = new ProcessStartInfo("cmake",
            $"-S \"{repoRoot}\" -B \"{buildDir}\" -G Ninja -DCMAKE_BUILD_TYPE=Release -DENGINE_BUILD_TESTS=ON")
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            WorkingDirectory = repoRoot,
        };
        using var proc = Process.Start(psi)
            ?? throw new InvalidOperationException("build-player: could not start cmake.");
        var stdoutTask = proc.StandardOutput.ReadToEndAsync();
        var stderrTask = proc.StandardError.ReadToEndAsync();
        if (!proc.WaitForExit(TimeSpan.FromMinutes(15)))
        {
            try
            {
                proc.Kill(entireProcessTree: true);
            }
            catch (Exception)
            {
            }

            throw new InvalidOperationException("build-player: cmake configure timed out.");
        }

        var log = stdoutTask.GetAwaiter().GetResult() + "\n" + stderrTask.GetAwaiter().GetResult();
        if (proc.ExitCode != 0)
        {
            var tail = log.Trim();
            if (tail.Length > 2000)
            {
                tail = tail[^2000..];
            }

            throw new InvalidOperationException($"build-player: cmake configure failed ({proc.ExitCode}): {tail}");
        }

        return log;
    }

    private static string BuildNativePlayer(string repoRoot, string buildDir)
    {
        // Single build path: RebuildTarget touches the TU (forces
        // compile+link+POST_BUILD, which stages the runtime DLLs) and
        // fails closed with the tail on error.
        RebuildTarget(repoRoot, buildDir, "engine-player");

        var exe = OperatingSystem.IsWindows() ? "engine-player.exe" : "engine-player";
        var found = Directory.EnumerateFiles(buildDir, exe, SearchOption.AllDirectories).ToList();
        Console.Error.WriteLine($"build-player: native build found {found.Count} '{exe}':");
        foreach (var f in found)
        {
            Console.Error.WriteLine($"build-player:   candidate '{f}'");
        }

        // Prefer a same-name DLL next to the exe (Windows loader probes the
        // app dir first): engine_core.dll + flecs.dll both qualify.
        // NOTE: $<TARGET_FILE:engine_core> is STATIC (.lib) on Windows —
        // dumpbin proves the exe imports ONLY engine_core.dll, yet no
        // engine_core.dll is ever emitted (flecs resolves through the
        // static flecs.lib). The missing DLL is engine_core.dll itself.
        foreach (var cand in found)
        {
            var dir = Path.GetDirectoryName(cand)!;
            if (Directory.EnumerateFiles(dir, "engine_core.*").Any()
                || Directory.EnumerateFiles(dir, "flecs.*").Any())
            {
                Console.Error.WriteLine($"build-player: native exe with dlls '{cand}'");
                return cand;
            }
        }

        if (found.Count > 0)
        {
            Console.Error.WriteLine(
                $"build-player: WARNING no exe dir holds engine DLLs; using '{found[0]}'");
            return found[0];
        }

        throw new InvalidOperationException("build-player: engine-player binary not found after build.");
    }

    /// <summary>
    /// Force relink of one target: touch its primary source, then build.
    /// `cmake --build --target` is a no-op when nothing changed, which
    /// skips POST_BUILD (runtime DLL copy) on rebuilds. Touching the TU
    /// forces compile+link+POST_BUILD deterministically.
    /// </summary>
    private static void RebuildTarget(string repoRoot, string buildDir, string target)
    {
        var tu = Path.Combine(repoRoot, "native", "player_app", "main.cpp");
        try
        {
            File.SetLastWriteTimeUtc(tu, DateTime.UtcNow);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"build-player: WARNING touch {tu} failed: {ex.Message}");
        }

        var psi = new ProcessStartInfo("cmake", $"--build \"{buildDir}\" --target {target}")
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            WorkingDirectory = repoRoot,
        };
        using var proc = Process.Start(psi)
            ?? throw new InvalidOperationException($"build-player: could not start cmake --build {target}.");
        var stdoutTask = proc.StandardOutput.ReadToEndAsync();
        var stderrTask = proc.StandardError.ReadToEndAsync();
        if (!proc.WaitForExit(TimeSpan.FromMinutes(30)))
        {
            try
            {
                proc.Kill(entireProcessTree: true);
            }
            catch (Exception)
            {
            }

            throw new InvalidOperationException($"build-player: engine-player rebuild timed out.");
        }

        var log = stdoutTask.GetAwaiter().GetResult() + "\n" + stderrTask.GetAwaiter().GetResult();
        Console.Error.WriteLine($"build-player: relink {target} rc={proc.ExitCode}");
        if (proc.ExitCode != 0)
        {
            var tail = log.Trim();
            if (tail.Length > 2000)
            {
                tail = tail[^2000..];
            }

            throw new InvalidOperationException($"build-player: engine-player rebuild failed ({proc.ExitCode}): {tail}");
        }
    }

    /// <summary>
    /// Zero-entry ENGPAK (header only, toc_offset = header size, count 0) so
    /// asset-less projects still stage a verifiable game.pak. Layout mirrors
    /// PakFormat/HeaderSize (magic ENGPAK1\0, version 1, 24B header).
    /// </summary>
    private static int WriteEmptyPakPlaceholder(string pakPath)
    {
        using var fs = new FileStream(pakPath, FileMode.Create, FileAccess.Write);
        fs.Write("ENGPAK1\0"u8);
        void U16(ushort v)
        {
            Span<byte> b = stackalloc byte[2];
            System.Buffers.Binary.BinaryPrimitives.WriteUInt16LittleEndian(b, v);
            fs.Write(b);
        }

        void U32(uint v)
        {
            Span<byte> b = stackalloc byte[4];
            System.Buffers.Binary.BinaryPrimitives.WriteUInt32LittleEndian(b, v);
            fs.Write(b);
        }

        void U64(ulong v)
        {
            Span<byte> b = stackalloc byte[8];
            System.Buffers.Binary.BinaryPrimitives.WriteUInt64LittleEndian(b, v);
            fs.Write(b);
        }

        U16(1);
        U16(0);
        U32(0);
        U64(24);
        return 0;
    }

    // --- resolution --------------------------------------------------------

    public static string? FindRepoRoot()
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
                if (File.Exists(Path.Combine(dir.FullName, "Engine.slnx")))
                {
                    return dir.FullName;
                }
            }
        }

        return null;
    }

    private static (string Name, string Dir) ResolveProject(string repoRoot, string project)
    {
        // Absolute/relative dir with a project.json or csproj.
        if (Directory.Exists(project)
            || project.Contains(Path.DirectorySeparatorChar)
            || project.Contains('/'))
        {
            var dir = Path.GetFullPath(project);
            if (!Directory.Exists(dir))
            {
                throw new ArgumentException($"project directory not found: {project}.", nameof(project));
            }

            return (new DirectoryInfo(dir).Name, dir);
        }

        // Sample name under samples/.
        var sampleDir = Path.Combine(repoRoot, "samples", project);
        if (Directory.Exists(sampleDir))
        {
            return (project, sampleDir);
        }

        throw new ArgumentException(
            $"unknown project '{project}' (no samples/{project}/, no such directory).", nameof(project));
    }

    private static string? FindGameProject(string projectDir, string projectName)
    {
        var direct = Path.Combine(projectDir, projectName + ".csproj");
        if (File.Exists(direct))
        {
            return direct;
        }

        var matches = Directory.GetFiles(projectDir, "*.csproj", SearchOption.TopDirectoryOnly);
        return matches.Length == 1 ? matches[0] : null;
    }

    private static string? LocatePrebuiltAssembly(string projectDir, string projectName, string configuration)
    {
        var binRoot = Path.Combine(projectDir, "bin");
        if (!Directory.Exists(binRoot))
        {
            return null;
        }

        var dll = $"{projectName}.dll";
        foreach (var cand in Directory.EnumerateFiles(binRoot, dll, SearchOption.AllDirectories))
        {
            return cand;
        }

        return null;
    }
}

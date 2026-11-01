// ProjectApiTests.cs — P3-10: projects, saving, and building from the editor.
//
// CPU suite (no native): ProjectService create/open/save-file/entity-count,
// BuildService MSBuild error parsing, EditorConsole level filters + collapse.
// Live suite (needs engine_core): name round-trip via editor_get_name,
// create -> capture -> save -> reopen over ProjectService with entity-count
// compare, plus a deliberate C# syntax error yielding >=1 console Error
// entry naming the file.
using System.Runtime.InteropServices;
using Engine.Bindings;
using Engine.Editor;

namespace Engine.Tests;

public static class ProjectApiTests
{
    public static unsafe int Run(string[]? args = null)
    {
        var failures = 0;

        // --- CPU: project layout --------------------------------------------
        var tmp = Path.Combine(Path.GetTempPath(), $"proj-test-{Environment.ProcessId}");
        try
        {
            if (Directory.Exists(tmp))
            {
                Directory.Delete(tmp, true);
            }

            var proj = ProjectService.Create(tmp, "Game");
            failures += Check(File.Exists(Path.Combine(tmp, "project.json")), "project: project.json created");
            failures += Check(Directory.Exists(Path.Combine(tmp, "assets")), "project: assets/ created");
            failures += Check(File.Exists(Path.Combine(tmp, "scenes", "main.scene.json")), "project: scenes/main.scene.json created");
            failures += Check(File.Exists(Path.Combine(tmp, "Game.csproj")), "project: Game.csproj created");
            failures += Check(File.ReadAllText(Path.Combine(tmp, "Game.csproj")).Contains("Engine.Runtime"), "project: Game.csproj refs Engine.Runtime");
            failures += Check(ProjectService.CountEntities(Path.Combine(tmp, "scenes", "main.scene.json")) == 0, "project: empty scene has 0 entities");

            var reopened = ProjectService.Open(tmp);
            failures += Check(reopened.Name == "Game", "project: open reads name");
            failures += Check(reopened.StartScene == ProjectService.DefaultScenePath, "project: open reads startScene");

            // --- CPU: error parsing ------------------------------------------
            var log = "Game/Player.cs(12,5): error CS1002: ; expected [/tmp/x/Game.csproj]\n"
                + "Build succeeded.\n";
            var entries = BuildService.ParseErrors(log);
            failures += Check(entries.Count == 1, "build-cpu: one error parsed");
            failures += Check(entries.Count == 1 && entries[0].Level == ConsoleLevel.Error, "build-cpu: error level");
            failures += Check(entries.Count == 1 && (entries[0].File ?? string.Empty).EndsWith("Player.cs", StringComparison.Ordinal), "build-cpu: file name captured");
            failures += Check(BuildService.ParseErrors("Build succeeded.\n").Count == 0, "build-cpu: clean log parses empty");

            // --- CPU: console -------------------------------------------------
            var console = new EditorConsole();
            console.Info("hello");
            failures += Check(console.Visible().Count == 1 && console.Visible()[0].Text == "hello", "console-cpu: info visible");
            console.ShowInfo = false;
            failures += Check(console.Visible().Count == 0, "console-cpu: info filter hides");
            console.ShowInfo = true;
            console.Error("boom", "Foo.cs");
            failures += Check(console.ErrorCount() == 1, "console-cpu: error counted");
            console.Warning("w");
            console.Warning("w");
            failures += Check(console.Visible(collapse: true).Count == 3, "console-cpu: collapse merges repeats");
        }
        finally
        {
            try
            {
                Directory.Delete(tmp, true);
            }
            catch (Exception)
            {
            }
        }

        // --- Live: save/reopen + broken build ---------------------------------
        var liveProbe = (args ?? []).Contains("--live-native-probe");
        IntPtr liveHandle = IntPtr.Zero;
        try
        {
            liveHandle = GiLivePreload.TryPreloadEngineCoreHandle();
            if (liveHandle != IntPtr.Zero)
            {
                ProjectLiveLoader.Load(liveHandle);
            }
            else
            {
                Engine.Bindings.NativeApi.ApiLoader.Load();
            }
        }
        catch (Exception ex) when (ex is DllNotFoundException || ex is EntryPointNotFoundException || ex is InvalidOperationException)
        {
            if (liveProbe)
            {
                failures += Check(false, $"live probe FAILED to load native lib: {ex.GetType().Name}: {ex.Message}");
                Console.WriteLine($"ProjectApiTests: {failures} FAILED");
                return failures;
            }

            Console.WriteLine($"[SKIP] project live (no native lib: {ex.GetType().Name})");
            Console.WriteLine(failures == 0 ? "ProjectApiTests: all checks passed" : $"ProjectApiTests: {failures} FAILED");
            return failures;
        }

        try
        {
            failures += RunLive();
        }
        catch (Exception ex)
        {
            failures += Check(false, $"project live threw: {ex.GetType().Name}: {ex.Message}");
        }

        Console.WriteLine(failures == 0 ? "ProjectApiTests: all checks passed" : $"ProjectApiTests: {failures} FAILED");
        return failures;
    }

    private static unsafe int RunLive()
    {
        var failures = 0;
        var table = EditorSession.Table;
        Engine.Bindings.NativeApi.EngineWorld* world = table->EngineWorldCreate();
        failures += Check(world != null, "live: world_create");
        if (world == null)
        {
            return failures;
        }

        var root = Path.Combine(Path.GetTempPath(), $"proj-live-{Environment.ProcessId}");
        try
        {
            if (Directory.Exists(root))
            {
                Directory.Delete(root, true);
            }

            var proj = ProjectService.Create(root, "Game");
            using var session = new EditorSession(world);

            // Name round-trip via ABI 4.3 editor_get_name.
            var a = session.Spawn("Alpha");
            var b = session.Spawn("Beta");
            session.EnsureLocal(a);
            session.EnsureLocal(b);
            session.SetLocal(a, new float[] { 1, 2, 3, 0, 0, 0, 1, 1, 1, 1 });
            session.SetLocal(b, new float[] { 4, 5, 6, 0, 0, 0, 1, 2, 2, 2 });
            session.SetParent(b, a);
            failures += Check(session.GetName(a) == "Alpha", "live: get_name round-trips");
            var unknownRejected = false;
            try
            {
                session.GetName(999999001);
            }
            catch (InvalidOperationException)
            {
                unknownRejected = true;
            }

            failures += Check(unknownRejected, "live: get_name rejects unknown entity");

            var scenePath = Path.Combine(root, proj.StartScene);
            ProjectService.Save(session, scenePath);
            failures += Check(ProjectService.CountEntities(scenePath) == 2, "live: saved scene has 2 entities");

            // Reopen into a fresh world: entity count must match.
            Engine.Bindings.NativeApi.EngineWorld* world2 = table->EngineWorldCreate();
            failures += Check(world2 != null, "live: second world_create");
            if (world2 != null)
            {
                try
                {
                    using var session2 = new EditorSession(world2);
                    var ids = ProjectService.Load(session2, scenePath);
                    failures += Check(ids.Count == 2, "live: reopen loads 2 entities");
                    failures += Check(session2.CountRoots() == 1, "live: reopen has 1 root");
                    failures += Check(session2.GetName(ids[0]) == "Alpha", "live: reopen keeps names");
                    var pose = new float[10];
                    session2.GetLocal(ids[1], pose);
                    failures += Check(MathF.Abs(pose[0] - 4) < 1e-6f, "live: reopen keeps poses");
                }
                finally
                {
                    table->EngineWorldDestroy(world2);
                }
            }

            // Deliberate C# syntax error -> >=1 console Error naming the file.
            var brokenDir = Path.Combine(root, "Broken");
            Directory.CreateDirectory(brokenDir);
            var brokenCsproj = Path.Combine(brokenDir, "Broken.csproj");
            File.WriteAllText(brokenCsproj, """
                <Project Sdk="Microsoft.NET.Sdk">
                  <PropertyGroup>
                    <TargetFramework>net10.0</TargetFramework>
                    <Nullable>enable</Nullable>
                    <ImplicitUsings>enable</ImplicitUsings>
                  </PropertyGroup>
                </Project>
                """);
            var badFile = Path.Combine(brokenDir, "Oops.cs");
            File.WriteAllText(badFile, "class Oops { void M() { this is not C# } }\n");
            var console = new EditorConsole();
            var result = BuildService.Build(brokenCsproj, console);
            failures += Check(result.ExitCode != 0, "live: broken build exits nonzero");
            var errs = console.All();
            failures += Check(errs.Count >= 1 && errs.Any(e => e.Level == ConsoleLevel.Error), "live: broken build yields Error entry");
            failures += Check(errs.Any(e => (e.File ?? string.Empty).EndsWith("Oops.cs", StringComparison.Ordinal)), "live: Error entry names the file");
        }
        catch (Exception ex)
        {
            failures += Check(false, $"project live threw: {ex.GetType().Name}: {ex.Message}");
        }
        finally
        {
            table->EngineWorldDestroy(world);
            try
            {
                Directory.Delete(root, true);
            }
            catch (Exception)
            {
            }
        }

        return failures;
    }

    private static unsafe class ProjectLiveLoader
    {
        public static void Load(IntPtr handle)
        {
            if (!NativeLibrary.TryGetExport(handle, "engine_get_api_table", out var addr))
            {
                throw new EntryPointNotFoundException("engine_get_api_table not exported.");
            }

            var getTable = (delegate* unmanaged[Cdecl, SuppressGCTransition]<NativeApi.ApiTable*>)addr;
            var table = getTable();
            if (table == null)
            {
                throw new InvalidOperationException("engine_get_api_table returned null.");
            }

            EditorSession.TableOverride = (IntPtr)table;
        }
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }
}

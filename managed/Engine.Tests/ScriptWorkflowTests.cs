// ScriptWorkflowTests.cs — P3-11 acceptance: edit-save hot reload in the editor.
//
// Verbatim acceptance: a test edits a system's constant in a temp project,
// waits for the reload, and observes the new behaviour within 2s of the file
// write; a syntax error leaves the old behaviour running and logs an Error.
//
// Strategy: drive the REAL ScriptBuilder + ScriptWatcher against a temp
// `assets/` dir holding one source file. Behaviour is read through the
// stable dispatcher trampoline (slot 0) over a synthetic chunk — the same
// probe HotReloadTests uses — so the test proves end-to-end dispatch of the
// freshly built bytes, not just emit success.
using System.Runtime.InteropServices;
using Engine.Bindings;
using Engine.Editor;

namespace Engine.Tests;

public static class ScriptWorkflowTests
{
    private const string TypeName = "GameScripts.MoverSystem";
    private const string MethodName = "RunChunk";

    public static int Run()
    {
        var failures = 0;
        failures += Check(EditSaveReloadsNewBehaviour(), "script-workflow: edit-save rebuilds, reload serves new behaviour <2s");
        failures += Check(SyntaxErrorKeepsOldBehaviour(), "script-workflow: syntax error keeps old behaviour + logs Error");
        failures += Check(BindFailureRestoresLastGood(), "script-workflow: bind failure after unload restores last working assembly");
        failures += Check(WatcherDebouncesToOneRebuild(), "script-workflow: watcher debounces burst into one rebuild");
        failures += Check(OpenProjectValidatesPath(), "script-workflow: OpenCSharpProject rejects missing path, no process");
        Console.WriteLine(failures == 0 ? "ScriptWorkflowTests: all checks passed" : $"ScriptWorkflowTests: {failures} FAILED");
        return failures;
    }

    // --- Acceptance 1: edit a constant, observe new behaviour within 2s ----
    private static bool EditSaveReloadsNewBehaviour()
    {
        var root = TempAssetsDir();
        try
        {
            File.WriteAllText(Path.Combine(root, "Mover.cs"), Source(scale: 1.0f, broken: false));
            using var builder = new ScriptBuilder(systems: [(TypeName, MethodName)]);
            if (!builder.Rebuild(root))
            {
                Console.WriteLine("[script-workflow] FAIL: initial build failed");
                return false;
            }

            if (!RunTrampolineReads(expectDelta: 1.0f / 60.0f))
            {
                Console.WriteLine("[script-workflow] FAIL: gen0 behaviour wrong");
                return false;
            }

            // Edit the constant on disk, rebuild exactly as the watcher would.
            var sw = System.Diagnostics.Stopwatch.StartNew();
            File.WriteAllText(Path.Combine(root, "Mover.cs"), Source(scale: 7.0f, broken: false));
            if (!builder.Rebuild(root))
            {
                Console.WriteLine("[script-workflow] FAIL: rebuild after edit failed");
                return false;
            }

            sw.Stop();
            Console.WriteLine($"[script-workflow] edit-save reload: {sw.Elapsed.TotalMilliseconds:F1}ms (gate <2000ms)");
            if (sw.Elapsed.TotalSeconds >= 2.0)
            {
                return false;
            }

            return RunTrampolineReads(expectDelta: 7.0f / 60.0f);
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    // --- Acceptance 2: syntax error keeps old behaviour + Error log ---------
    private static bool SyntaxErrorKeepsOldBehaviour()
    {
        var root = TempAssetsDir();
        try
        {
            File.WriteAllText(Path.Combine(root, "Mover.cs"), Source(scale: 2.0f, broken: false));
            using var builder = new ScriptBuilder(systems: [(TypeName, MethodName)]);
            if (!builder.Rebuild(root))
            {
                return false;
            }

            File.WriteAllText(Path.Combine(root, "Mover.cs"), Source(scale: 2.0f, broken: true));
            if (builder.Rebuild(root))
            {
                Console.WriteLine("[script-workflow] FAIL: broken build reported success");
                return false;
            }

            var hasError = builder.Log.Any(e =>
                e.Level == ScriptLogLevel.Error && e.File != null && e.File.EndsWith("Mover.cs", StringComparison.Ordinal));
            if (!hasError)
            {
                Console.WriteLine("[script-workflow] FAIL: no Error entry naming Mover.cs");
                foreach (var e in builder.Log)
                {
                    Console.WriteLine($"  log: {e.Level} {e.File} {e.Text.Split('\n')[0]}");
                }

                return false;
            }

            // Old behaviour still served through the same trampoline.
            return RunTrampolineReads(expectDelta: 2.0f / 60.0f);
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    // --- Reload bind failure: compiles, but the runner is gone ------------
    private static bool BindFailureRestoresLastGood()
    {
        var root = TempAssetsDir();
        try
        {
            File.WriteAllText(Path.Combine(root, "Mover.cs"), Source(scale: 3.0f, broken: false));
            using var builder = new ScriptBuilder(systems: [(TypeName, MethodName)]);
            if (!builder.Rebuild(root))
            {
                return false;
            }

            // Valid C#, but the bound runner type no longer exists, so
            // Reload fails after the old generation has been unloaded.
            File.WriteAllText(
                Path.Combine(root, "Mover.cs"),
                Source(scale: 3.0f, broken: false).Replace("class MoverSystem", "class RenamedSystem", StringComparison.Ordinal));
            if (builder.Rebuild(root))
            {
                Console.WriteLine("[script-workflow] FAIL: bind failure reported success");
                return false;
            }

            if (builder.Generation < 0)
            {
                Console.WriteLine("[script-workflow] FAIL: nothing bound after failed reload");
                return false;
            }

            return RunTrampolineReads(expectDelta: 3.0f / 60.0f);
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    // --- Watcher: burst of saves -> one rebuild callback -------------------
    private static bool WatcherDebouncesToOneRebuild()
    {
        var root = TempAssetsDir();
        try
        {
            var path = Path.Combine(root, "Mover.cs");
            File.WriteAllText(path, Source(scale: 1.0f, broken: false));
            var fired = 0;
            using var watcher = new ScriptWatcher(root, () => Interlocked.Increment(ref fired));

            // Burst: 5 rapid saves (atomic-save rename pairs included).
            for (var i = 0; i < 5; i++)
            {
                File.WriteAllText(path, Source(scale: 1.0f + i, broken: false));
            }

            var deadline = DateTime.UtcNow.AddSeconds(2);
            while (Volatile.Read(ref fired) == 0 && DateTime.UtcNow < deadline)
            {
                Thread.Sleep(20);
            }

            Thread.Sleep(400); // let any extra debounce windows expire
            var count = Volatile.Read(ref fired);
            Console.WriteLine($"[script-workflow] watcher fired {count}x for 5-save burst");
            return count == 1;
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    // --- OpenCSharpProject: missing path fails closed, no process -----------
    private static bool OpenProjectValidatesPath()
    {
        using var builder = new ScriptBuilder();
        var missing = Path.Combine(Path.GetTempPath(), Guid.NewGuid() + ".csproj");
        if (builder.OpenCSharpProject(missing))
        {
            return false;
        }

        return builder.Log.Any(e => e.Level == ScriptLogLevel.Error);
    }

    // --- Helpers ------------------------------------------------------------

    private static string TempAssetsDir()
    {
        var root = Path.Combine(Path.GetTempPath(), "engine-p311-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        return root;
    }

    private static string Source(float scale, bool broken)
    {
        var literal = scale.ToString("R", System.Globalization.CultureInfo.InvariantCulture);
        var body = broken
            ? "float* px = ??? broken syntax here"
            : $"float* px = (float*)view->Local; for (int i = 0; i < view->Count; i++) {{ px[i * 16] += {literal} * dt; }}";
        return $$"""
            using System;
            using System.Runtime.CompilerServices;
            using System.Runtime.InteropServices;
            using Engine.Bindings;
            namespace GameScripts
            {
                public static unsafe class MoverSystem
                {
                    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
                    public static void RunChunk(NativeApi.EcsChunkView* view, float dt)
                    {
                        if (view->Count <= 0) { return; }
                        {{body}}
                    }
                }
            }
            """;
    }

    private static unsafe bool RunTrampolineReads(float expectDelta)
    {
        const int n = 8;
        var local = (float*)NativeMemory.AlignedAlloc((nuint)(n * 64), 64);
        if (local == null)
        {
            return false;
        }

        try
        {
            new Span<byte>(local, n * 64).Clear();
            var view = new NativeApi.EcsChunkView
            {
                Local = (NativeApi.LocalTransform*)local,
                World = null,
                Entities = null,
                Count = n,
                StrideLocal = 64,
                StrideWorld = 64,
                UserData = null,
            };
            var tramp = (delegate* unmanaged[Cdecl]<NativeApi.EcsChunkView*, float, void>)
                Engine.Runtime.ScriptSystemDispatcher.TrampolineAddress(0);
            tramp(&view, 1.0f / 60.0f);
            for (var i = 0; i < n; i++)
            {
                var px = local[i * 16];
                if (Math.Abs(px - expectDelta) > 1e-6f)
                {
                    Console.WriteLine($"[script-workflow] FAIL: row {i} px={px}, want {expectDelta}");
                    return false;
                }
            }

            return true;
        }
        finally
        {
            NativeMemory.AlignedFree(local);
        }
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }
}

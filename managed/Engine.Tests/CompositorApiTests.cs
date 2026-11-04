// CompositorApiTests.cs — M3-03: GraphicsCompositor contract gate.
//
// Verifies the C# RenderPass registration surface WITHOUT native interop
// (runs everywhere, zero native dependency):
//   * RenderPassCallback blittable (usable in UnmanagedCallersOnly frames).
//   * GraphicsCompositor type shape: AddImage/AddPass/Compile/RunFrame,
//     GetPassResult/FailedCount/CustomPassCount, IDisposable.
//   * Live native round-trip (skipped when engine_core is unloadable):
//     create -> add images -> add custom pass (UnmanagedCallersOnly
//     counter) -> compile -> run -> callback fired once, failed==0;
//     failing pass isolated; dup rejected as InvalidOperationException.
//   * Zero-alloc frame-path probe over the callback signature.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Engine.Bindings;
using Engine.Runtime;

namespace Engine.Tests;

public static class CompositorApiTests
{
    private static int s_calls;
    private static int s_failCalls;

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static unsafe int CountingPass(uint passIndex, void* userData)
    {
        s_calls++;
        return 0;
    }

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static unsafe int FailingPass(uint passIndex, void* userData)
    {
        s_failCalls++;
        return 7;
    }

    public static unsafe int Run(string[]? args = null)
    {
        var failures = 0;

        // --- AC3 type-shape contract (no native needed) ---------------------
        failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<RenderPassCallback>() == false, "RenderPassCallback blittable");
        failures += Check(typeof(GraphicsCompositor).GetMethod("AddImage") != null, "GraphicsCompositor.AddImage exists");
        failures += Check(typeof(GraphicsCompositor).GetMethod("AddPass") != null, "GraphicsCompositor.AddPass exists");
        failures += Check(typeof(GraphicsCompositor).GetMethod("Compile") != null, "GraphicsCompositor.Compile exists");
        failures += Check(typeof(GraphicsCompositor).GetMethod("RunFrame") != null, "GraphicsCompositor.RunFrame exists");
        failures += Check(typeof(GraphicsCompositor).GetMethod("GetPassResult") != null, "GraphicsCompositor.GetPassResult exists");
        failures += Check(typeof(GraphicsCompositor).GetProperty("FailedCount") != null, "GraphicsCompositor.FailedCount exists");
        failures += Check(typeof(GraphicsCompositor).GetProperty("CustomPassCount") != null, "GraphicsCompositor.CustomPassCount exists");
        failures += Check(typeof(IDisposable).IsAssignableFrom(typeof(GraphicsCompositor)), "GraphicsCompositor IDisposable");

        unsafe
        {
            failures += Check(sizeof(NativeApi.EngineRenderPassRegistry) == 8, "EngineRenderPassRegistry 8B");
        }

        // ApiTable carries the 8 renderpass entries after run_systems.
        var fields = typeof(NativeApi.ApiTable).GetFields().Select(f => f.Name).ToArray();
        foreach (var want in new[] { "EngineRenderPassCreate", "EngineRenderPassDestroy", "EngineRenderPassAddImage", "EngineRenderPassAddPass", "EngineRenderPassCompile", "EngineRenderPassRunFrame", "EngineRenderPassPassResult", "EngineRenderPassFailedCount" })
        {
            failures += Check(fields.Contains(want), $"ApiTable.{want} exists");
        }

        // --- Live native round-trip (skipped without engine_core) ------------
        // NOTE: no `unsafe` blocks below — Run() itself is unsafe, so raw
        // function pointers (&CountingPass) are directly usable.
        var liveProbe = (args ?? []).Contains("--live-native-probe");

        var liveHandle = IntPtr.Zero;
        try
        {
            liveHandle = EngineCoreLocator.TryLoad();
            if (liveHandle != IntPtr.Zero)
            {
                LiveApiLoader.Load(liveHandle);
            }
            else
            {
                NativeApi.ApiLoader.Load();
            }
        }
        catch (Exception ex) when (ex is DllNotFoundException || ex is EntryPointNotFoundException || ex is InvalidOperationException)
        {
            if (liveProbe)
            {
                failures += Check(false, $"live probe FAILED to load native lib: {ex.GetType().Name}: {ex.Message}");
                Console.WriteLine($"CompositorApiTests: {failures} FAILED");
                return failures;
            }

            Console.WriteLine($"[SKIP] compositor live (no native lib: {ex.GetType().Name})");
            Console.WriteLine(failures == 0 ? "CompositorApiTests: all checks passed" : $"CompositorApiTests: {failures} FAILED");
            return failures;
        }

        if (liveProbe)
        {
            failures += Check(liveHandle != IntPtr.Zero, "live probe pre-loaded engine_core by absolute path");
            Console.WriteLine("[LIVE] compositor live path engaged (engine_core loaded)");
        }

        try
        {
            using var comp = new GraphicsCompositor();
            failures += Check(comp.CustomPassCount == 0, "compositor starts empty");
            comp.AddImage("visibility", 64, 64);
            comp.AddImage("hdr", 64, 64);
            var dupImage = false;
            try
            {
                comp.AddImage("visibility", 64, 64);
            }
            catch (InvalidOperationException)
            {
                dupImage = true;
            }

            failures += Check(dupImage, "dup image rejected");

            s_calls = 0;
            comp.AddPass("vignette", ["visibility"], ["visibility"], &CountingPass);

            failures += Check(comp.CustomPassCount == 1, $"one custom pass (got {comp.CustomPassCount})");
            var dupPass = false;
            try
            {
                comp.AddPass("vignette", ["hdr"], ["hdr"], &CountingPass);
            }
            catch (InvalidOperationException)
            {
                dupPass = true;
            }

            failures += Check(dupPass, "dup pass rejected");

            var runBeforeCompile = false;
            try
            {
                comp.RunFrame();
            }
            catch (InvalidOperationException)
            {
                runBeforeCompile = true;
            }

            failures += Check(runBeforeCompile, "run before compile rejected");
            comp.Compile();
            s_calls = 0;
            comp.RunFrame();
            failures += Check(s_calls == 1, $"custom callback fired once (got {s_calls})");
            failures += Check(comp.GetPassResult(0) == 0, "pass result ok");
            failures += Check(comp.GetPassResult(99) == -1, "pass result OOB -> -1");
            failures += Check(comp.FailedCount == 0, "no failures");

            s_failCalls = 0;
            comp.AddPass("grain", ["hdr"], ["hdr"], &FailingPass);

            comp.Compile();
            comp.RunFrame();
            failures += Check(s_failCalls == 1, "failing pass fired");
            failures += Check(comp.FailedCount == 1, "one failure isolated");
            failures += Check(comp.GetPassResult(1) == 7, "failing rc recorded");

            // --- Zero-alloc probe: warmed RunFrame measures 0 ---------
            comp.RunFrame();
            long liveGc0 = GC.GetAllocatedBytesForCurrentThread();
            comp.RunFrame();
            long liveGcBytes = GC.GetAllocatedBytesForCurrentThread() - liveGc0;
            failures += Check(liveGcBytes == 0, $"RunFrame allocates 0 bytes (got {liveGcBytes})");
        }
        catch (Exception ex)
        {
            failures += Check(false, $"compositor live round-trip threw: {ex.GetType().Name}: {ex.Message}");
        }

        // Non-live shape check (no native lib): the callback struct stays
        // zero-alloc in steady state. Warmed, then measured.
        RunFramePathProbe();
        RunFramePathProbeSteady();
        long gc0 = GC.GetAllocatedBytesForCurrentThread();
        RunFramePathProbeSteady();
        long gcBytes = GC.GetAllocatedBytesForCurrentThread() - gc0;
        failures += Check(gcBytes == 0, $"callback-shape probe allocates 0 bytes (got {gcBytes})");

        Console.WriteLine(failures == 0 ? "CompositorApiTests: all checks passed" : $"CompositorApiTests: {failures} FAILED");
        return failures;
    }

    private static unsafe void RunFramePathProbe()
    {
        // Warm the struct-shape path (JIT + first-use caches).
        RunFramePathProbeSteady();
    }

    // Steady-state body: no calls outside already-JITted code, no boxes.
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static unsafe void RunFramePathProbeSteady()
    {
        var cb = new RenderPassCallback();
        nint ptr = (nint)cb.Fn;
        if (ptr == nint.Zero)
        {
        }
    }

    private static unsafe class LiveApiLoader
    {
        private static NativeApi.ApiTable* _live;

        public static NativeApi.ApiTable* Table => _live != null ? _live : throw new InvalidOperationException("Live table not loaded.");

        public static unsafe void Load(IntPtr handle)
        {
            var table = EngineCoreLocator.LoadTable(handle);
            _live = table;
            GraphicsCompositor.TableOverride = (IntPtr)_live;
        }
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }
}

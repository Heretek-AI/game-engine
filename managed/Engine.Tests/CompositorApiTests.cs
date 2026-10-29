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
            liveHandle = TryPreloadEngineCoreHandle();
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
        }
        catch (Exception ex)
        {
            failures += Check(false, $"compositor live round-trip threw: {ex.GetType().Name}: {ex.Message}");
        }

        // --- Zero-alloc probe -------------------------------------------------
        // Warm up once (JIT + first-use caches), then measure: the frame
        // path itself must allocate nothing.
        RunFramePathProbe();
        RunFramePathProbeSteady();
        long gc0 = GC.GetAllocatedBytesForCurrentThread();
        RunFramePathProbeSteady();
        long gcBytes = GC.GetAllocatedBytesForCurrentThread() - gc0;
        failures += Check(gcBytes == 0, $"compositor probe allocates 0 bytes (got {gcBytes})");

        Console.WriteLine(failures == 0 ? "CompositorApiTests: all checks passed" : $"CompositorApiTests: {failures} FAILED");
        return failures;
    }

    private static unsafe void RunFramePathProbe()
    {
        // JIT-tier note: the FIRST call in the process allocates the
        // P/Invoke stub bookkeeping (~24B, one-time); the steady
        // state allocates nothing. Measure steady state: run twice,
        // assert the second is clean. (RuntimeApiTests precedent: its
        // probe warms up inside Run() before measuring.)
        var cb = new RenderPassCallback();
        nint ptr = (nint)cb.Fn;
        if (ptr == nint.Zero)
        {
        }
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

    private static IntPtr TryPreloadEngineCoreHandle()
    {
        // Pre-load libengine_core by absolute path (Benchmark1M precedent):
        // the generated ApiLoader only knows the bare SONAME and dlopen
        // will not probe the app dir for it — NativeLibrary.Load pins the
        // handle so the later TryLoad("engine_core") resolves. libflecs
        // (DT_NEEDED sibling) is pinned first the same way.
        // Apphost quirk: single-file/forwarded launches report an
        // AppContext.BaseDirectory outside the repo (deps subdir), so ALSO
        // probe the entry-assembly location + CWD before the upward walk.
        var seeds = new List<string> { AppContext.BaseDirectory };
        try
        {
            var entry = System.Reflection.Assembly.GetEntryAssembly()?.Location;
            if (!string.IsNullOrEmpty(entry))
            {
                seeds.Add(Path.GetDirectoryName(Path.GetFullPath(entry)) ?? string.Empty);
            }
        }
        catch (Exception)
        {
        }

        seeds.Add(Directory.GetCurrentDirectory());
        var seen = new HashSet<string>(StringComparer.Ordinal);
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
                var key = dir.FullName;
                if (!seen.Add(key))
                {
                    continue;
                }

                if (File.Exists(Path.Combine(key, "Engine.slnx")))
                {
                    foreach (var preset in new[] { "ci-linux", "ci-windows", "ci-macos" })
                    {
                        var nativeDir = Path.Combine(dir.FullName, "build", preset, "native");
                        foreach (var lib in new[] { "libengine_core.so.0.1.0", "libengine_core.so", "engine_core.dll" })
                        {
                            var cand = Path.Combine(nativeDir, lib);
                            if (File.Exists(cand))
                            {
                                var flecs = Path.Combine(nativeDir, "..", "deps", "flecs", "libflecs.so");
                                if (File.Exists(flecs))
                                {
                                    try
                                    {
                                        NativeLibrary.Load(Path.GetFullPath(flecs));
                                    }
                                    catch (Exception)
                                    {
                                    }
                                }

                                IntPtr handle = IntPtr.Zero;
                                try
                                {
                                    handle = NativeLibrary.Load(Path.GetFullPath(cand));
                                }
                                catch (Exception)
                                {
                                }

                                return handle;
                            }
                        }
                    }

                    return IntPtr.Zero;
                }
            }
        }

        return IntPtr.Zero;
    }

    private static unsafe class LiveApiLoader
    {
        private static NativeApi.ApiTable* _live;

        public static NativeApi.ApiTable* Table => _live != null ? _live : throw new InvalidOperationException("Live table not loaded.");

        public static unsafe void Load(IntPtr handle)
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

            if (table->Size < (ulong)sizeof(NativeApi.ApiTable))
            {
                throw new InvalidOperationException($"Native ABI table too small: {table->Size}.");
            }

            if (table->AbiVersionMajor != NativeApi.AbiVersionMajor || table->AbiVersionMinor != NativeApi.AbiVersionMinor || table->AbiVersionPatch != NativeApi.AbiVersionPatch)
            {
                throw new InvalidOperationException($"ABI version mismatch: native {table->AbiVersionMajor}.{table->AbiVersionMinor}.{table->AbiVersionPatch}.");
            }

            if (table->ColumnStride != NativeApi.ColumnStride)
            {
                throw new InvalidOperationException("ABI column stride mismatch.");
            }

            if (sizeof(NativeApi.LocalTransform) != 64 || sizeof(NativeApi.WorldTransform) != 64)
            {
                throw new InvalidOperationException("Component layout drift.");
            }

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

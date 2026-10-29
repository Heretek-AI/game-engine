// GiApiTests.cs — M3-04: GlobalIllumination managed contract gate.
//
// Verifies the C# zero-bake GI surface WITHOUT native interop (runs
// everywhere, zero native dependency):
//   * Sizing constants are byte-exact mirrors of the native planners
//     (256KiB probes = 4 levels x 16^3 x 16B; 16MiB SDSM atlas).
//   * GlobalIllumination type shape: AddGiPasses/PassesAdded/IDisposable.
//   * Zero-alloc steady-state probe over the GI pass shape.
//   * Live native round-trip (skipped when engine_core is unloadable):
//     compositor + GI passes -> compile -> run -> both callbacks fired,
//     zero failures, zero per-frame managed allocation.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Engine.Bindings;
using Engine.Runtime;

namespace Engine.Tests;

internal static class GiLivePreload
{
    internal static IntPtr s_lastHandle;
    internal static IntPtr TryPreloadEngineCoreHandle()
    {
        // Same absolute-path preload as the compositor probe (Benchmark1M
        // precedent): pin libflecs first, then engine_core by absolute
        // path. Kept local (not shared) to avoid cross-test coupling.
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

            for (var d = dir; d != null; d = d.Parent)
            {
                var hit = PreloadPair(Path.Combine(d.FullName, "libengine_core.so"))
                    || PreloadPair(Path.Combine(d.FullName, "libengine_core.dylib"))
                    || PreloadPair(Path.Combine(d.FullName, "engine_core.dll"));
                if (hit)
                {
                    return s_lastHandle;
                }

                var key = d.FullName;
                if (!seen.Add(key))
                {
                    break;
                }
            }
        }

        return IntPtr.Zero;
    }

    private static bool PreloadPair(string corePath)
    {
        try
        {
            var dir = Path.GetDirectoryName(corePath);
            if (dir != null)
            {
                foreach (var flecsName in new[] { "libflecs.so", "libflecs.dylib", "flecs.dll" })
                {
                    var flecs = Path.Combine(dir, flecsName);
                    if (File.Exists(flecs))
                    {
                        try
                        {
                            NativeLibrary.Load(flecs);
                        }
                        catch (Exception)
                        {
                        }

                        break;
                    }
                }
            }
        }
        catch (Exception)
        {
        }

        try
        {
            s_lastHandle = NativeLibrary.Load(corePath);
            return true;
        }
        catch (Exception)
        {
            return false;
        }
    }
}

internal static unsafe class CompositorApiTestsLiveLoader
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

        GraphicsCompositor.TableOverride = (IntPtr)table;
    }
}

public static class GiApiTests
{
    public static unsafe int Run(string[]? args = null)
    {
        var failures = 0;

        // --- Sizing contract (no native needed) ---------------------------
        failures += Check(GlobalIllumination.CascadeLevels == 4, "GI 4 cascade levels");
        failures += Check(GlobalIllumination.ProbeRes == 16, "GI probe res 16");
        failures += Check(GlobalIllumination.ProbeBytesPerLevel == 16L * 16 * 16 * 16, "GI 64KiB per level");
        failures += Check(GlobalIllumination.TotalProbeBytes == 4 * 16L * 16 * 16 * 16, "GI 256KiB total probes");
        failures += Check(GlobalIllumination.ProbeBytesForScene() == 256 * 1024, "GI ProbeBytesForScene 256KiB");
        failures += Check(GlobalIllumination.ShadowAtlasBytes == 2048L * 2048 * 4, "GI shadow atlas 16MiB");
        failures += Check(GlobalIllumination.AtlasBytes() == 16 * 1024 * 1024, "GI AtlasBytes 16MiB");
        failures += Check(GlobalIllumination.ShadowCascades == 4, "GI 4 shadow cascades");
        failures += Check(typeof(GlobalIllumination).GetMethod("AddGiPasses") != null, "GlobalIllumination.AddGiPasses exists");
        failures += Check(typeof(GlobalIllumination).GetProperty("PassesAdded") != null, "GlobalIllumination.PassesAdded exists");
        failures += Check(typeof(IDisposable).IsAssignableFrom(typeof(GlobalIllumination)), "GlobalIllumination IDisposable");

        // --- Live native round-trip (skipped without engine_core) ----------
        var liveProbe = (args ?? []).Contains("--live-native-probe");
        var liveHandle = IntPtr.Zero;
        try
        {
            liveHandle = GiLivePreload.TryPreloadEngineCoreHandle();
            if (liveHandle != IntPtr.Zero)
            {
                CompositorApiTestsLiveLoader.Load(liveHandle);
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
                Console.WriteLine($"GiApiTests: {failures} FAILED");
                return failures;
            }

            Console.WriteLine($"[SKIP] GI live (no native lib: {ex.GetType().Name})");
            Console.WriteLine(failures == 0 ? "GiApiTests: all checks passed" : $"GiApiTests: {failures} FAILED");
            return failures;
        }

        if (liveProbe)
        {
            failures += Check(liveHandle != IntPtr.Zero, "live probe pre-loaded engine_core by absolute path");
            Console.WriteLine("[LIVE] GI live path engaged (engine_core loaded)");
        }

        try
        {
            using var comp = new GraphicsCompositor();
            comp.AddImage("visibility", 64, 64);
            comp.AddImage("hdr", 64, 64);
            using var gi = new GlobalIllumination(comp);
            failures += Check(!gi.PassesAdded, "GI passes start unadded");
            gi.AddGiPasses();
            gi.AddGiPasses(); // idempotent
            failures += Check(gi.PassesAdded, "GI passes added once");
            failures += Check(comp.CustomPassCount == 2, $"two GI passes (got {comp.CustomPassCount})");
            comp.Compile();
            comp.RunFrame();
            failures += Check(comp.FailedCount == 0, "GI passes run clean");
            failures += Check(comp.GetPassResult(0) == 0, "gi resolve rc 0");
            failures += Check(comp.GetPassResult(1) == 0, "shadow trace rc 0");

            comp.RunFrame();
            long liveGc0 = GC.GetAllocatedBytesForCurrentThread();
            comp.RunFrame();
            long liveGcBytes = GC.GetAllocatedBytesForCurrentThread() - liveGc0;
            failures += Check(liveGcBytes == 0, $"GI RunFrame allocates 0 bytes (got {liveGcBytes})");
        }
        catch (Exception ex)
        {
            failures += Check(false, $"GI live round-trip threw: {ex.GetType().Name}: {ex.Message}");
        }

        Console.WriteLine(failures == 0 ? "GiApiTests: all checks passed" : $"GiApiTests: {failures} FAILED");
        return failures;
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }
}

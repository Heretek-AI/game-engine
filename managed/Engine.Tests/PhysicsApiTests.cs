// PhysicsApiTests.cs — M4-03: Physics managed contract gate.
//
// Verifies the C# spatial-query surface:
//   * Type shape (no native needed): Ray/RaycastHit blittable + byte-exact
//     sizes, Physics type shape (Raycast/SphereCast/OverlapSphere/
//     RaycastBatch, IDisposable).
//   * Live native round-trip (skipped when engine_core is unloadable):
//     preload engine_core by absolute path, drive the ABI physics_*
//     entries through the Physics wrapper. NOTE: the ABI queries need a
//     live JPH::PhysicsSystem (caller-owned M4-01 world); the managed test
//     obtains one from the native test helper binary... — instead the live
//     path here only validates the ABI function pointers resolve + reject
//     nulls. Full query correctness is proven by engine_physics_queries
//     (CTest) which owns a real PhysicsSystem.
//   * Zero-alloc steady-state probe over the Ray/RaycastHit shapes.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Engine.Bindings;
using Engine.Runtime;

namespace Engine.Tests;

public static class PhysicsApiTests
{
    public static unsafe int Run(string[]? args = null)
    {
        var failures = 0;

        // --- AC1/AC2 type-shape contract (no native needed) -----------------
        failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<Ray>() == false, "Ray blittable");
        failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<RaycastHit>() == false, "RaycastHit blittable");
        failures += Check(typeof(Physics).GetMethod("Raycast") != null, "Physics.Raycast exists");
        failures += Check(typeof(Physics).GetMethod("SphereCast") != null, "Physics.SphereCast exists");
        failures += Check(typeof(Physics).GetMethod("OverlapSphere") != null, "Physics.OverlapSphere exists");
        failures += Check(typeof(Physics).GetMethod("RaycastBatch") != null, "Physics.RaycastBatch exists");
        failures += Check(typeof(IDisposable).IsAssignableFrom(typeof(Physics)), "Physics IDisposable");

        unsafe
        {
            failures += Check(sizeof(NativeApi.Ray) == 24, "NativeApi.Ray 24B");
            failures += Check(sizeof(NativeApi.RaycastHit) == 48, "NativeApi.RaycastHit 48B");
            failures += Check(sizeof(Ray) == 24, "Ray 24B");
            failures += Check(sizeof(RaycastHit) == 48, "RaycastHit 48B");
        }

        // ApiTable carries the 4 physics entries after the renderpass tail.
        var fields = typeof(NativeApi.ApiTable).GetFields().Select(f => f.Name).ToArray();
        foreach (var want in new[] { "EnginePhysicsRaycast", "EnginePhysicsSphereCast", "EnginePhysicsOverlapSphere", "EnginePhysicsRaycastBatch" })
        {
            failures += Check(fields.Contains(want), $"ApiTable.{want} exists");
        }

        // Constructor guards (no native needed).
        var threwNull = false;
        try
        {
            using var _ = new Physics(IntPtr.Zero);
        }
        catch (ArgumentNullException)
        {
            threwNull = true;
        }

        failures += Check(threwNull, "Physics(IntPtr.Zero) throws ArgumentNullException");

        // Arg validation without a world: disposed + length checks.
        using (var p = new Physics(new IntPtr(0x1000)))
        {
            var badRadius = false;
            try
            {
                p.SphereCast(new Ray(0, 0, 0, 0, 1, 0), 0.0f, out _);
            }
            catch (ArgumentOutOfRangeException)
            {
                badRadius = true;
            }

            failures += Check(badRadius, "SphereCast radius<=0 rejected");

            var badBatch = false;
            try
            {
                Span<Ray> rays = stackalloc Ray[2];
                Span<RaycastHit> hits = stackalloc RaycastHit[1];
                p.RaycastBatch(rays, hits);
            }
            catch (ArgumentException)
            {
                badBatch = true;
            }

            failures += Check(badBatch, "RaycastBatch length mismatch rejected");

            p.Dispose();
            var threwDisposed = false;
            try
            {
                p.Raycast(new Ray(0, 0, 0, 0, 1, 0), out _);
            }
            catch (ObjectDisposedException)
            {
                threwDisposed = true;
            }

            failures += Check(threwDisposed, "disposed Physics throws ObjectDisposedException");
        }

        // --- Live native round-trip (skipped without engine_core) -----------
        // The ABI entries need a live JPH::PhysicsSystem; this process has
        // none (the native engine_physics_queries binary proves query
        // correctness against a real world). Here: validate the table
        // resolves with ABI 4 + the four entries are non-null, and that a
        // null world is rejected (returns 0, no crash). Preload the staged
        // lib first (same absolute-path precedent as the compositor probe):
        // ApiLoader only knows the bare SONAME.
        var liveProbe = (args ?? []).Contains("--live-native-probe");
        try
        {
            var preloaded = PhysicsLivePreload.TryPreloadEngineCoreHandle();
            NativeApi.ApiTable* table;
            if (preloaded != IntPtr.Zero)
            {
                PhysicsLivePreload.LoadIntoApiLoader(preloaded);
                table = (NativeApi.ApiTable*)Physics.TableOverride;
            }
            else
            {
                table = NativeApi.ApiLoader.Load();
            }
            failures += Check(table->EnginePhysicsRaycast != null, "live: physics_raycast non-null");
            failures += Check(table->EnginePhysicsSphereCast != null, "live: physics_sphere_cast non-null");
            failures += Check(table->EnginePhysicsOverlapSphere != null, "live: physics_overlap_sphere non-null");
            failures += Check(table->EnginePhysicsRaycastBatch != null, "live: physics_raycast_batch non-null");

            var ray = new NativeApi.Ray { Ox = 0, Oy = 0, Oz = 0, Dx = 0, Dy = -1, Dz = 0 };
            var hit = default(NativeApi.RaycastHit);
            var rc = table->EnginePhysicsRaycast(null, &ray, &hit);
            failures += Check(rc == 0, "live: null world raycast -> 0 (no crash)");
            uint n = table->EnginePhysicsOverlapSphere(null, 0, 0, 0, 1, null, null, 0);
            failures += Check(n == 0, "live: null world overlap -> 0 (no crash)");
            uint b = table->EnginePhysicsRaycastBatch(null, &ray, &hit, 1);
            failures += Check(b == 0, "live: null world batch -> 0 (no crash)");
        }
        catch (Exception ex) when (ex is DllNotFoundException || ex is EntryPointNotFoundException || ex is InvalidOperationException)
        {
            if (liveProbe)
            {
                failures += Check(false, $"live probe FAILED to load native lib: {ex.GetType().Name}: {ex.Message}");
                Console.WriteLine($"PhysicsApiTests: {failures} FAILED");
                return failures;
            }

            Console.WriteLine($"[SKIP] physics live (no native lib: {ex.GetType().Name})");
            Console.WriteLine(failures == 0 ? "PhysicsApiTests: all checks passed" : $"PhysicsApiTests: {failures} FAILED");
            return failures;
        }

        // --- Zero-alloc probe over the query shapes --------------------------
        SteadyProbe();
        long gc0 = GC.GetAllocatedBytesForCurrentThread();
        SteadyProbe();
        long gcBytes = GC.GetAllocatedBytesForCurrentThread() - gc0;
        failures += Check(gcBytes == 0, $"query-shape probe allocates 0 bytes (got {gcBytes})");

        Console.WriteLine(failures == 0 ? "PhysicsApiTests: all checks passed" : $"PhysicsApiTests: {failures} FAILED");
        return failures;
    }

    [MethodImpl(MethodImplOptions.NoInlining)]
    private static unsafe void SteadyProbe()
    {
        var ray = new Ray(0, 0, 5, 0, 0, -10);
        var hit = new RaycastHit();
        nint a = (nint)(&ray.Ox);
        nint b = (nint)(&hit.Px);
        if (a == nint.Zero || b == nint.Zero)
        {
        }
    }

    // Staged-lib preload (same absolute-path precedent as the compositor
    // probe): pins libflecs + engine_core, then points Physics.TableOverride
    // at the live table so the wrapper drives the real ABI entries.
    private static class PhysicsLivePreload
    {
        public static unsafe IntPtr TryPreloadEngineCoreHandle()
        {
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
                        foreach (var seedDir in new[] { Directory.GetCurrentDirectory(), AppContext.BaseDirectory })
                        {
                            foreach (var stagedName in new[] { "libengine_core.dylib", "engine_core.dll", "libengine_core.so", "libengine_core.so.0.1.0", "libengine_core.so.0" })
                            {
                                var staged = Path.Combine(seedDir, stagedName);
                                if (File.Exists(staged))
                                {
                                    var h = PreloadPair(Path.GetFullPath(staged));
                                    if (h != IntPtr.Zero)
                                    {
                                        return h;
                                    }
                                }
                            }
                        }

                        return IntPtr.Zero;
                    }
                }
            }

            return IntPtr.Zero;
        }

        public static unsafe void LoadIntoApiLoader(IntPtr handle)
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

            // Point the Physics wrapper at the live table (the managed
            // ApiLoader only knows the bare SONAME, which the staged dir
            // does not satisfy — the preloaded handle is authoritative).
            Physics.TableOverride = (IntPtr)table;
        }

        private static IntPtr PreloadPair(string corePath)
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
                return NativeLibrary.Load(corePath);
            }
            catch (Exception)
            {
                return IntPtr.Zero;
            }
        }
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }
}

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

            failures += Check(badRadius, "SphereCast radius==0 rejected");

            var badNegativeRadius = false;
            try
            {
                p.SphereCast(new Ray(0, 0, 0, 0, 1, 0), -1.0f, out _);
            }
            catch (ArgumentOutOfRangeException)
            {
                badNegativeRadius = true;
            }

            failures += Check(badNegativeRadius, "SphereCast negative radius rejected");

            var badNaNRadius = false;
            try
            {
                p.SphereCast(new Ray(0, 0, 0, 0, 1, 0), float.NaN, out _);
            }
            catch (ArgumentOutOfRangeException)
            {
                badNaNRadius = true;
            }

            failures += Check(badNaNRadius, "SphereCast NaN radius rejected");

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

            var badOverlapRadius = false;
            try
            {
                Span<uint> ids = stackalloc uint[1];
                p.OverlapSphere(0, 0, 0, float.NaN, ids);
            }
            catch (ArgumentOutOfRangeException)
            {
                badOverlapRadius = true;
            }

            failures += Check(badOverlapRadius, "OverlapSphere NaN radius rejected");

            var badOverlapZero = false;
            try
            {
                Span<uint> ids = stackalloc uint[1];
                p.OverlapSphere(0, 0, 0, 0.0f, ids);
            }
            catch (ArgumentOutOfRangeException)
            {
                badOverlapZero = true;
            }

            failures += Check(badOverlapZero, "OverlapSphere radius==0 rejected");

            // NOTE: the short-userData guard needs a table pointer (it throws
            // after TablePtr is read), so it is proven in the managed-stub
            // block below, not here; every check in this block throws before
            // any table touch and needs no native library.
            Span<uint> emptyIds = Span<uint>.Empty;
            var emptyOverlap = p.OverlapSphere(0, 0, 0, 1.0f, emptyIds);
            failures += Check(emptyOverlap == 0, "OverlapSphere empty/empty returns 0");

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

        // --- Managed-stub functional + zero-alloc probe (no native needed) --
        // Drive the Physics wrapper through a managed ApiTable built from
        // UnmanagedCallersOnly stubs, so the zero-alloc claim covers the real
        // wrapper path (not just struct construction).
        var priorOverride = Physics.TableOverride;
        try
        {
            unsafe
            {
                var stub = default(NativeApi.ApiTable);
                stub.EnginePhysicsRaycast = &FakePhysics.StubRaycast;
                stub.EnginePhysicsSphereCast = &FakePhysics.StubSphereCast;
                stub.EnginePhysicsOverlapSphere = &FakePhysics.StubOverlapSphere;
                stub.EnginePhysicsRaycastBatch = &FakePhysics.StubRaycastBatch;
                var handle = GCHandle.Alloc(stub, GCHandleType.Pinned);
                try
                {
                    Physics.TableOverride = (IntPtr)handle.AddrOfPinnedObject();
                    using var fp = new Physics(new IntPtr(0x1000));

                    var ray = new Ray(0, 2, 0, 0, -2, 0);
                    var gotHit = fp.Raycast(ray, out var rhit);
                    failures += Check(gotHit && FakePhysics.LastRaycastCount == 1, "stub Raycast reaches native entry (rc=1, count=1)");

                    gotHit = fp.SphereCast(ray, 0.5f, out _);
                    failures += Check(gotHit && FakePhysics.LastSphereRadius > 0.0f, "stub SphereCast reaches native entry with radius");

                    Span<uint> sids = stackalloc uint[2];
                    Span<ulong> sud = stackalloc ulong[2];
                    var nw = fp.OverlapSphere(0, 1, 0, 1.0f, sids, sud);
                    var nullUdPassed = FakePhysics.LastOverlapUserData == null;
                    failures += Check(nw == 1 && sids[0] == 42u && sud[0] == 7u, "stub OverlapSphere writes ids+userData");
                    failures += Check(nullUdPassed == false, "stub OverlapSphere receives non-null userData when provided");

                    Span<uint> sids2 = stackalloc uint[2];
                    var nw2 = fp.OverlapSphere(0, 1, 0, 1.0f, sids2);
                    failures += Check(nw2 == 1 && FakePhysics.LastOverlapUserData == null, "stub OverlapSphere passes null userData when empty");

                    var badUserData = false;
                    try
                    {
                        Span<uint> ids = stackalloc uint[2];
                        Span<ulong> ud = stackalloc ulong[1];
                        fp.OverlapSphere(0, 0, 0, 1.0f, ids, ud);
                    }
                    catch (ArgumentException)
                    {
                        badUserData = true;
                    }

                    failures += Check(badUserData, "stub OverlapSphere short userData rejected");

                    Span<Ray> rays2 = stackalloc Ray[2];
                    Span<RaycastHit> hits2 = stackalloc RaycastHit[2];
                    rays2[0] = ray;
                    rays2[1] = ray;
                    var nb = fp.RaycastBatch(rays2, hits2);
                    failures += Check(nb == 2 && FakePhysics.LastBatchCount == 2, "stub RaycastBatch receives count=2");

                    // Warm up once (JIT + first-call caches), then measure.
                    FakePhysics.SteadyDrive(fp);
                    long fa0 = GC.GetAllocatedBytesForCurrentThread();
                    FakePhysics.SteadyDrive(fp);
                    long fakeBytes = GC.GetAllocatedBytesForCurrentThread() - fa0;
                    failures += Check(fakeBytes == 0, $"Physics wrapper steady-state allocates 0 bytes (got {fakeBytes})");
                }
                finally
                {
                    Physics.TableOverride = priorOverride;
                    handle.Free();
                }
            }
        }
        catch (Exception ex)
        {
            failures += Check(false, $"managed-stub probe threw: {ex.GetType().Name}: {ex.Message}");
            Physics.TableOverride = priorOverride;
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
        catch (Exception ex)
        {
            // A found library with an ABI mismatch must fail, and an
            // explicit --live-native-probe must fail too: only a missing
            // library (DllNotFound / EntryPointNotFound) without the flag
            // InvalidOperationException for table-too-small, ABI-version, or
            // stride drift — swallowing those would hide real ABI 4 drift.
            var preloadedMissing = false;
            if ((ex is DllNotFoundException || ex is EntryPointNotFoundException) && !liveProbe)
            {
                preloadedMissing = true;
            }

            if (preloadedMissing)
            {
                Console.WriteLine($"[SKIP] physics live (no native lib: {ex.GetType().Name})");
                Console.WriteLine(failures == 0 ? "PhysicsApiTests: all checks passed" : $"PhysicsApiTests: {failures} FAILED");
                return failures;
            }

            failures += Check(false, $"live probe FAILED: {ex.GetType().Name}: {ex.Message}");
            Console.WriteLine($"PhysicsApiTests: {failures} FAILED");
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

    // Managed stub table (no native needed): UnmanagedCallersOnly entries
    // that record call args and return canned hits, so the zero-alloc probe
    // exercises the real Physics wrapper path.
    private static class FakePhysics
    {
        public static int LastRaycastCount;
        public static float LastSphereRadius;
        public static unsafe ulong* LastOverlapUserData;
        public static uint LastBatchCount;

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static void SteadyDrive(Physics p)
        {
            var ray = new Ray(0, 2, 0, 0, -2, 0);
            p.Raycast(ray, out _);
            p.SphereCast(ray, 0.5f, out _);
            Span<uint> ids = stackalloc uint[2];
            p.OverlapSphere(0, 1, 0, 1.0f, ids);
            Span<Ray> rays = stackalloc Ray[2];
            Span<RaycastHit> hits = stackalloc RaycastHit[2];
            rays[0] = ray;
            rays[1] = ray;
            p.RaycastBatch(rays, hits);
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static unsafe int StubRaycast(NativeApi.EnginePhysicsWorld* world, NativeApi.Ray* ray, NativeApi.RaycastHit* hit)
        {
            LastRaycastCount = 1;
            if (hit != null)
            {
                hit->Hit = 1;
                hit->Fraction = 0.5f;
            }

            return 1;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static unsafe int StubSphereCast(NativeApi.EnginePhysicsWorld* world, NativeApi.Ray* ray, float radius, NativeApi.RaycastHit* hit)
        {
            LastSphereRadius = radius;
            if (hit != null)
            {
                hit->Hit = 1;
                hit->Fraction = 0.25f;
            }

            return 1;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static unsafe uint StubOverlapSphere(NativeApi.EnginePhysicsWorld* world, float cx, float cy, float cz, float radius, uint* bodyIds, ulong* userData, uint capacity)
        {
            LastOverlapUserData = userData;
            if (bodyIds == null || capacity == 0)
            {
                return 0;
            }

            bodyIds[0] = 42u;
            if (userData != null)
            {
                userData[0] = 7u;
            }

            return 1;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static unsafe uint StubRaycastBatch(NativeApi.EnginePhysicsWorld* world, NativeApi.Ray* rays, NativeApi.RaycastHit* hits, uint count)
        {
            LastBatchCount = count;
            for (uint i = 0; i < count; i++)
            {
                hits[i].Hit = 1;
                hits[i].Fraction = 0.5f;
            }

            return count;
        }
    }
}

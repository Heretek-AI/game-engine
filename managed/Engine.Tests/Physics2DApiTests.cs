// Physics2DApiTests.cs — P6-07: managed 2D physics contract gate.
//
// Verifies the C# 2D physics surface:
//   * Type shape (no native needed): Rigidbody2D/BoxCollider2D/
//     CircleCollider2D/PolygonCollider2D/CollisionEvent2D blittable +
//     byte-exact sizes; Physics2D type shape (BodyBox/BodyCircle/
//     BodyPolygon/BodyDestroy/Step/Drain, IDisposable).
//   * Live native round-trip (skipped when engine_core is unloadable):
//     builds a small 2-box drop through the real ABI entries and
//     asserts >= 1 kEnter event drains (same shape as the P2-05 3D path).
//   * Zero-alloc steady-state probe over a stub table.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Engine.Bindings;
using Engine.Runtime;

namespace Engine.Tests;

public static class Physics2DApiTests
{
    public static unsafe int Run(string[]? args = null)
    {
        var failures = 0;

        // --- Type-shape contract (no native needed) ----------------------
        failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<NativeApi.Rigidbody2D>() == false, "Rigidbody2D blittable");
        failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<NativeApi.CollisionEvent2D>() == false, "CollisionEvent2D blittable");
        failures += Check(typeof(Physics2D).GetMethod("BodyBox") != null, "Physics2D.BodyBox exists");
        failures += Check(typeof(Physics2D).GetMethod("BodyCircle") != null, "Physics2D.BodyCircle exists");
        failures += Check(typeof(Physics2D).GetMethod("BodyPolygon") != null, "Physics2D.BodyPolygon exists");
        failures += Check(typeof(Physics2D).GetMethod("BodyDestroy") != null, "Physics2D.BodyDestroy exists");
        failures += Check(typeof(Physics2D).GetMethod("Step") != null, "Physics2D.Step exists");
        failures += Check(typeof(Physics2D).GetMethod("Drain") != null, "Physics2D.Drain exists");
        failures += Check(typeof(IDisposable).IsAssignableFrom(typeof(Physics2D)), "Physics2D IDisposable");

        unsafe
        {
            failures += Check(sizeof(NativeApi.CollisionEvent2D) == 32, "NativeApi.CollisionEvent2D 32B");
            failures += Check(sizeof(NativeApi.Rigidbody2D) == 24, "NativeApi.Rigidbody2D 24B");
            failures += Check(sizeof(NativeApi.BoxCollider2D) == 24, "NativeApi.BoxCollider2D 24B");
            failures += Check(sizeof(NativeApi.CircleCollider2D) == 28, "NativeApi.CircleCollider2D 28B");
            failures += Check(sizeof(NativeApi.PolygonCollider2D) == 64, "NativeApi.PolygonCollider2D 64B");
            failures += Check(Marshal.OffsetOf<NativeApi.CollisionEvent2D>("PointX").ToInt64() == 16, "CollisionEvent2D.PointX @ 16");
            failures += Check(Marshal.OffsetOf<NativeApi.CollisionEvent2D>("Kind").ToInt64() == 28, "CollisionEvent2D.Kind @ 28");
        }

        var fields = typeof(NativeApi.ApiTable).GetFields().Select(f => f.Name).ToArray();
        foreach (var want in new[] { "EnginePhysics2DCreate", "EnginePhysics2DDestroy", "EnginePhysics2DBodyBox", "EnginePhysics2DBodyCircle", "EnginePhysics2DBodyPolygon", "EnginePhysics2DBodyDestroy", "EnginePhysics2DStep", "EnginePhysics2DDrain" })
        {
            failures += Check(fields.Contains(want), $"ApiTable.{want} exists");
        }

        // Constructor + arg guards (no native needed).
        var threwNull = false;
        try
        {
            using var _ = new Physics2D(IntPtr.Zero);
        }
        catch (ArgumentNullException)
        {
            threwNull = true;
        }

        failures += Check(threwNull, "Physics2D(IntPtr.Zero) throws ArgumentNullException");

        using (var p = new Physics2D(new IntPtr(0x1000)))
        {
            var badEntity = false;
            try
            {
                p.BodyBox(0, 0, 0, 0, default, default);
            }
            catch (ArgumentOutOfRangeException)
            {
                badEntity = true;
            }

            failures += Check(badEntity, "BodyBox entity==0 rejected");

            var badDt = false;
            try
            {
                p.Step(0.0f);
            }
            catch (ArgumentOutOfRangeException)
            {
                badDt = true;
            }

            failures += Check(badDt, "Step dt==0 rejected");

            Span<NativeApi.CollisionEvent2D> empty = Span<NativeApi.CollisionEvent2D>.Empty;
            failures += Check(p.Drain(empty) == 0, "Drain empty span returns 0");

            p.BodyDestroy(0); // ignored, no throw
            failures += Check(true, "BodyDestroy(0) ignored");

            p.Dispose();
            var threwDisposed = false;
            try
            {
                p.Step(1.0f / 60.0f);
            }
            catch (ObjectDisposedException)
            {
                threwDisposed = true;
            }

            failures += Check(threwDisposed, "disposed Physics2D throws ObjectDisposedException");
        }

        // --- Managed-stub zero-alloc probe (no native needed) ------------
        var priorOverride = Physics2D.TableOverride;
        try
        {
            unsafe
            {
                var stub = default(NativeApi.ApiTable);
                stub.EnginePhysics2DCreate = &FakePhysics2D.StubCreate;
                stub.EnginePhysics2DDestroy = &FakePhysics2D.StubDestroy;
                stub.EnginePhysics2DBodyBox = &FakePhysics2D.StubBodyBox;
                stub.EnginePhysics2DBodyCircle = &FakePhysics2D.StubBodyCircle;
                stub.EnginePhysics2DBodyPolygon = &FakePhysics2D.StubBodyPolygon;
                stub.EnginePhysics2DBodyDestroy = &FakePhysics2D.StubBodyDestroy;
                stub.EnginePhysics2DStep = &FakePhysics2D.StubStep;
                stub.EnginePhysics2DDrain = &FakePhysics2D.StubDrain;
                var handle = GCHandle.Alloc(stub, GCHandleType.Pinned);
                try
                {
                    Physics2D.TableOverride = (IntPtr)handle.AddrOfPinnedObject();
                    using var fp = new Physics2D(0.0f, -10.0f);
                    failures += Check(FakePhysics2D.CreateCount == 1, "stub create reached (count=1)");

                    var rb = new NativeApi.Rigidbody2D { GravityScale = 1.0f, Type = 2, Active = 1, AllowSleeping = 1 };
                    var box = new NativeApi.BoxCollider2D { Hx = 0.5f, Hy = 0.5f, Density = 1.0f, Friction = 0.6f };
                    var token = fp.BodyBox(7, 0, 1, 0, rb, box);
                    failures += Check(token == 77 && FakePhysics2D.LastEntity == 7, "stub BodyBox reaches native entry (token=77, entity=7)");

                    Span<NativeApi.CollisionEvent2D> evts = stackalloc NativeApi.CollisionEvent2D[4];
                    fp.Step(1.0f / 60.0f);
                    var n = fp.Drain(evts);
                    failures += Check(n == 1 && evts[0].EntityA == 7 && evts[0].Kind == 0, "stub Drain returns 1 kEnter event");

                    // Warm up once (JIT + first-call caches), then measure.
                    FakePhysics2D.SteadyDrive(fp);
                    long fa0 = GC.GetAllocatedBytesForCurrentThread();
                    FakePhysics2D.SteadyDrive(fp);
                    long fakeBytes = GC.GetAllocatedBytesForCurrentThread() - fa0;
                    failures += Check(fakeBytes == 0, $"Physics2D wrapper steady-state allocates 0 bytes (got {fakeBytes})");
                }
                finally
                {
                    Physics2D.TableOverride = priorOverride;
                    handle.Free();
                }
            }
        }
        catch (Exception ex)
        {
            failures += Check(false, $"managed-stub probe threw: {ex.GetType().Name}: {ex.Message}");
            Physics2D.TableOverride = priorOverride;
        }

        // --- Live native round-trip (skipped without engine_core) --------
        // 2-box drop through the REAL ABI entries: a static ground slab +
        // one dynamic box dropped 2 m; 4 s at 60 Hz must drain >= 1
        // kEnter event (same assertion shape as the P2-05 3D gate).
        var liveProbe = (args ?? []).Contains("--live-native-probe");
        try
        {
            var preloaded = PhysicsLivePreload.TryPreloadEngineCoreHandle();
            NativeApi.ApiTable* table;
            var savedOverride = Physics2D.TableOverride;
            try
            {
                if (preloaded != IntPtr.Zero)
                {
                    PhysicsLivePreload.LoadIntoApiLoader(preloaded);
                    table = (NativeApi.ApiTable*)Physics2D.TableOverride;
                    // Keep the override: Physics2D.TablePtr reads the
                    // override first (same pattern as Physics.TableOverride
                    // in the 3D gate) — clearing it would send the wrapper
                    // back to the unloaded ApiLoader.
                }
                else if (liveProbe)
                {
                    table = NativeApi.ApiLoader.Load();
                    if (table == null)
                    {
                        Console.WriteLine("Physics2DApiTests: live probe requested but engine_core unloadable — FAIL");
                        return failures + 1;
                    }
                }
                else
                {
                    Console.WriteLine("Physics2DApiTests: shape + stub checks passed (live native skipped, pass --live-native-probe with staged engine_core)");
                    Console.WriteLine(failures == 0 ? "Physics2DApiTests: all checks passed" : $"Physics2DApiTests: {failures} FAILED");
                    return failures;
                }

                failures += Check(table->EnginePhysics2DCreate != null, "live: physics2d_create non-null");
                failures += Check(table->EnginePhysics2DDrain != null, "live: physics2d_drain non-null");
                failures += Check(table->EnginePhysics2DStep(null, 1.0f / 60.0f) == 0, "live: null world step -> 0 (no crash)");
                failures += Check(table->EnginePhysics2DDrain(null, null, 0) == 0, "live: null world drain -> 0 (no crash)");

                if (table->EnginePhysics2DCreate == null || table->EnginePhysics2DDrain == null)
                {
                    Console.WriteLine(failures == 0 ? "Physics2DApiTests: all checks passed" : $"Physics2DApiTests: {failures} FAILED");
                    return failures;
                }

                using var world = new Physics2D(0.0f, -10.0f);
                var grb = new NativeApi.Rigidbody2D { GravityScale = 1.0f, Type = 0, Active = 1, AllowSleeping = 1 };
                var gbox = new NativeApi.BoxCollider2D { Hx = 50.0f, Hy = 10.0f, Density = 1.0f, Friction = 0.6f };
                var g = world.BodyBox(1, 0.0f, -10.0f, 0.0f, grb, gbox);
                failures += Check(g != 0, "live: ground created");

                var drb = new NativeApi.Rigidbody2D { GravityScale = 1.0f, Type = 2, Active = 1, AllowSleeping = 1 };
                var dbox = new NativeApi.BoxCollider2D { Hx = 0.5f, Hy = 0.5f, Density = 1.0f, Friction = 0.6f };
                var d = world.BodyBox(2, 0.0f, 2.0f, 0.0f, drb, dbox);
                failures += Check(d != 0, "live: dynamic box created");

                Span<NativeApi.CollisionEvent2D> buf = stackalloc NativeApi.CollisionEvent2D[64];
                var enters = 0u;
                for (var i = 0; i < 240; i++)
                {
                    world.Step(1.0f / 60.0f);
                    var n = world.Drain(buf);
                    for (var k = 0; k < n; k++)
                    {
                        if (buf[k].Kind == 0)
                        {
                            enters++;
                        }
                    }
                }

                var tail = world.Drain(buf);
                for (var k = 0; k < tail; k++)
                {
                    if (buf[k].Kind == 0)
                    {
                        enters++;
                    }
                }

                failures += Check(enters >= 1, $"live: >=1 kEnter drained (got {enters})");
            }
            finally
            {
                Physics2D.TableOverride = savedOverride;
            }
        }
        catch (Exception ex)
        {
            // Live path only: missing native lib skips (shape + stub above
            // already proved the contract); an explicit probe fails closed.
            if (liveProbe)
            {
                failures += Check(false, $"live native probe threw: {ex.GetType().Name}: {ex.Message}");
            }
            else
            {
                Console.WriteLine($"Physics2DApiTests: live native skipped ({ex.GetType().Name})");
            }
        }

        Console.WriteLine(failures == 0 ? "Physics2DApiTests: all checks passed" : $"Physics2DApiTests: {failures} FAILED");
        return failures;
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }

    private static class FakePhysics2D
    {
        public static int CreateCount;
        public static ulong LastEntity;

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static void SteadyDrive(Physics2D p)
        {
            var rb = new NativeApi.Rigidbody2D { GravityScale = 1.0f, Type = 2, Active = 1 };
            var box = new NativeApi.BoxCollider2D { Hx = 0.5f, Hy = 0.5f, Density = 1.0f, Friction = 0.6f };
            p.BodyBox(7, 0, 1, 0, rb, box);
            p.Step(1.0f / 60.0f);
            Span<NativeApi.CollisionEvent2D> evts = stackalloc NativeApi.CollisionEvent2D[4];
            p.Drain(evts);
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
        public static unsafe NativeApi.EnginePhysics2DWorld* StubCreate(float gx, float gy, uint subSteps)
        {
            CreateCount++;
            return (NativeApi.EnginePhysics2DWorld*)0x1000;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
        public static unsafe void StubDestroy(NativeApi.EnginePhysics2DWorld* world)
        {
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
        public static unsafe ulong StubBodyBox(NativeApi.EnginePhysics2DWorld* world, ulong entity, float px, float py, float angle, NativeApi.Rigidbody2D* rb, NativeApi.BoxCollider2D* box)
        {
            LastEntity = entity;
            return 77;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
        public static unsafe ulong StubBodyCircle(NativeApi.EnginePhysics2DWorld* world, ulong entity, float px, float py, float angle, NativeApi.Rigidbody2D* rb, NativeApi.CircleCollider2D* circle)
        {
            return 78;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
        public static unsafe ulong StubBodyPolygon(NativeApi.EnginePhysics2DWorld* world, ulong entity, float px, float py, float angle, NativeApi.Rigidbody2D* rb, NativeApi.PolygonCollider2D* poly)
        {
            return 79;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
        public static unsafe void StubBodyDestroy(NativeApi.EnginePhysics2DWorld* world, ulong body)
        {
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
        public static unsafe uint StubStep(NativeApi.EnginePhysics2DWorld* world, float dt)
        {
            return 2;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
        public static unsafe uint StubDrain(NativeApi.EnginePhysics2DWorld* world, NativeApi.CollisionEvent2D* events, uint capacity)
        {
            if (events == null || capacity == 0)
            {
                return 0;
            }

            events[0].EntityA = 7;
            events[0].EntityB = 1;
            events[0].Kind = 0;
            return 1;
        }
    }

    // Live-preload helper: same staged-lib precedent as the Physics
    // (3D) and compositor probes — ApiLoader only knows the bare SONAME.
    private static class PhysicsLivePreload
    {
        public static IntPtr TryPreloadEngineCoreHandle()
        {
            var seeds = new List<string> { AppContext.BaseDirectory };
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

            Physics2D.TableOverride = (IntPtr)table;
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
}

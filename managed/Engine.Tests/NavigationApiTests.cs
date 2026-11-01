// NavigationApiTests.cs — P6-03: Navigation managed contract gate.
//
// Verifies the C# navigation surface:
//   * Type shape (no native needed): NavAgent blittable + 64B,
//     NavBox/NavBakeConfig/NavStats blittable + byte-exact sizes,
//     Navigation type shape (Bake/AddAgent/TrySetDestination/Update/
//     TryGetPosition/HasReached, IDisposable), SetDestination semantics.
//   * Managed-stub functional + zero-alloc probe (no native needed):
//     drive the Navigation wrapper through a managed ApiTable built from
//     UnmanagedCallersOnly stubs, so the zero-alloc claim covers the real
//     wrapper path.
//   * Live native round-trip (skipped when engine_core is unloadable):
//     preload engine_core by absolute path, drive the ABI nav_* entries
//     through the Navigation wrapper (bake a 20x20 floor + center box,
//     agent routes around it within 600 frames — mirrors CTest).
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Engine.Bindings;
using Engine.Runtime;

namespace Engine.Tests;

public static class NavigationApiTests
{
    public static unsafe int Run(string[]? args = null)
    {
        var failures = 0;

        // --- Type-shape contract (no native needed) ----------------------
        failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<NavAgent>() == false, "NavAgent blittable");
        failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<NativeApi.NavBox>() == false, "NavBox blittable");
        failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<NativeApi.NavBakeConfig>() == false, "NavBakeConfig blittable");
        failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<NativeApi.NavStats>() == false, "NavStats blittable");
        failures += Check(typeof(Navigation).GetMethod("Bake") != null, "Navigation.Bake exists");
        failures += Check(typeof(Navigation).GetMethod("AddAgent") != null, "Navigation.AddAgent exists");
        failures += Check(typeof(Navigation).GetMethod("TrySetDestination") != null, "Navigation.TrySetDestination exists");
        failures += Check(typeof(Navigation).GetMethod("Update") != null, "Navigation.Update exists");
        failures += Check(typeof(Navigation).GetMethod("TryGetPosition") != null, "Navigation.TryGetPosition exists");
        failures += Check(typeof(Navigation).GetMethod("HasReached") != null, "Navigation.HasReached exists");
        failures += Check(typeof(IDisposable).IsAssignableFrom(typeof(Navigation)), "Navigation IDisposable");
        failures += Check(typeof(NavAgent).GetMethod("SetDestination") != null, "NavAgent.SetDestination exists");

        unsafe
        {
            failures += Check(sizeof(NativeApi.NavBox) == 24, "NativeApi.NavBox 24B");
            failures += Check(sizeof(NativeApi.NavBakeConfig) == 48, "NativeApi.NavBakeConfig 48B");
            failures += Check(sizeof(NativeApi.NavStats) == 8, "NativeApi.NavStats 8B");
            failures += Check(sizeof(NativeApi.NavAgentData) == 64, "NativeApi.NavAgentData 64B");
            failures += Check(sizeof(NavAgent) == 64, "NavAgent 64B");
        }

        var fields = typeof(NativeApi.ApiTable).GetFields().Select(f => f.Name).ToArray();
        foreach (var want in new[] { "EngineNavCreate", "EngineNavDestroy", "EngineNavBake", "EngineNavAgentAdd", "EngineNavAgentGoto", "EngineNavAgentUpdate", "EngineNavAgentPos", "EngineNavAgentReached" })
        {
            failures += Check(fields.Contains(want), $"ApiTable.{want} exists");
        }

        // SetDestination semantics (no native, no table touch).
        {
            var agent = new NavAgent { Idle = 1, Arrived = 1, MaxSpeed = 3.5f, StopRadius = 0.5f };
            agent.SetDestination(6.0f, 0.5f, 0.0f);
            failures += Check(agent.Gx == 6.0f && agent.Gy == 0.5f && agent.Gz == 0.0f, "NavAgent.SetDestination writes goal");
            failures += Check(agent.Arrived == 0 && agent.Idle == 0, "NavAgent.SetDestination clears arrival/idle");
            failures += Check(agent.MaxSpeed == 3.5f && agent.StopRadius == 0.5f, "NavAgent.SetDestination keeps speeds");
        }

        // --- Managed-stub functional + zero-alloc probe (no native) -----
        var priorOverride = Navigation.TableOverride;
        try
        {
            unsafe
            {
                var stub = default(NativeApi.ApiTable);
                stub.EngineNavCreate = &FakeNav.StubCreate;
                stub.EngineNavDestroy = &FakeNav.StubDestroy;
                stub.EngineNavBake = &FakeNav.StubBake;
                stub.EngineNavAgentAdd = &FakeNav.StubAgentAdd;
                stub.EngineNavAgentGoto = &FakeNav.StubAgentGoto;
                stub.EngineNavAgentUpdate = &FakeNav.StubAgentUpdate;
                stub.EngineNavAgentPos = &FakeNav.StubAgentPos;
                stub.EngineNavAgentReached = &FakeNav.StubAgentReached;
                var handle = GCHandle.Alloc(stub, GCHandleType.Pinned);
                try
                {
                    Navigation.TableOverride = (IntPtr)handle.AddrOfPinnedObject();
                    using var nav = new Navigation();
                    failures += Check(FakeNav.CreateCount == 1, "stub nav_create reached (count=1)");

                    var config = new NativeApi.NavBakeConfig
                    {
                        FloorMinX = -10,
                        FloorMinY = 0,
                        FloorMinZ = -10,
                        FloorMaxX = 10,
                        FloorMaxY = 2,
                        FloorMaxZ = 10,
                        CellSize = 0.3f,
                        CellHeight = 0.2f,
                        AgentHeight = 2.0f,
                        AgentRadius = 0.6f,
                        AgentMaxClimb = 0.9f,
                        AgentMaxSlopeDeg = 45.0f,
                    };
                    var boxes = new NativeApi.NavBox[1];
                    boxes[0] = new NativeApi.NavBox { Cx = 0, Cy = 1, Cz = 0, Hx = 1, Hy = 1, Hz = 1 };
                    var stats = nav.Bake(boxes, config);
                    failures += Check(FakeNav.LastBakeObstacles == 1 && stats.Polys == 20, "stub nav_bake receives 1 obstacle, returns polys=20");

                    var id = nav.AddAgent(-6, 0.5f, 0, 3.5f);
                    failures += Check(id == 0 && FakeNav.LastMaxSpeed > 3.0f, "stub nav_agent_add returns id=0 with speed");

                    var accepted = nav.TrySetDestination(6, 0.5f, 0);
                    failures += Check(accepted && FakeNav.LastGotoX == 6.0f, "stub nav_agent_goto accepted, goal x=6");

                    var stepped = nav.Update(1.0f / 60.0f);
                    failures += Check(stepped && FakeNav.LastDt > 0.0f, "stub nav_agent_update steps with dt");

                    var hasPos = nav.TryGetPosition(out var px, out var py, out var pz);
                    failures += Check(hasPos && px == 1.0f && py == 0.5f && pz == 2.0f, "stub nav_agent_pos returns (1,0.5,2)");

                    failures += Check(nav.HasReached(0.5f) == false, "stub nav_agent_reached false before arrival");
                    FakeNav.ReachedValue = 1;
                    failures += Check(nav.HasReached(0.5f), "stub nav_agent_reached true when stub says so");

                    // Warm up once, then measure.
                    FakeNav.SteadyDrive(nav);
                    long fa0 = GC.GetAllocatedBytesForCurrentThread();
                    FakeNav.SteadyDrive(nav);
                    long fakeBytes = GC.GetAllocatedBytesForCurrentThread() - fa0;
                    failures += Check(fakeBytes == 0, $"Navigation wrapper steady-state allocates 0 bytes (got {fakeBytes})");
                }
                finally
                {
                    Navigation.TableOverride = priorOverride;
                    handle.Free();
                }
            }
        }
        catch (Exception ex)
        {
            failures += Check(false, $"managed-stub probe threw: {ex.GetType().Name}: {ex.Message}");
            Navigation.TableOverride = priorOverride;
        }

        // --- Live native round-trip (skipped without engine_core) --------
        // Bake the acceptance scene and step the crowd to the goal within
        // 600 frames at 60 Hz (mirrors engine_navigation_p6_03).
        var liveProbe = (args ?? []).Contains("--live-native-probe");
        try
        {
            var preloaded = NavigationLivePreload.TryPreloadEngineCoreHandle();
            NativeApi.ApiTable* table;
            if (preloaded != IntPtr.Zero)
            {
                NavigationLivePreload.LoadIntoApiLoader(preloaded);
                table = (NativeApi.ApiTable*)Navigation.TableOverride;
            }
            else
            {
                table = NativeApi.ApiLoader.Load();
            }
            failures += Check(table->EngineNavCreate != null, "live: nav_create non-null");
            failures += Check(table->EngineNavBake != null, "live: nav_bake non-null");
            failures += Check(table->EngineNavAgentAdd != null, "live: nav_agent_add non-null");
            failures += Check(table->EngineNavAgentGoto != null, "live: nav_agent_goto non-null");
            failures += Check(table->EngineNavAgentUpdate != null, "live: nav_agent_update non-null");
            failures += Check(table->EngineNavAgentPos != null, "live: nav_agent_pos non-null");
            failures += Check(table->EngineNavAgentReached != null, "live: nav_agent_reached non-null");

            var rcNull = table->EngineNavBake(null, null, 0, null, null);
            failures += Check(rcNull == 0, "live: null nav_bake -> 0 (no crash)");
            var idNull = table->EngineNavAgentAdd(null, 0, 0, 0, 1);
            failures += Check(idNull < 0, "live: null nav_agent_add -> negative (no crash)");
            var updNull = table->EngineNavAgentUpdate(null, 1.0f / 60.0f);
            failures += Check(updNull == 0, "live: null nav_agent_update -> 0 (no crash)");

            // Full acceptance: bake + crowd through the real Navigation
            // wrapper (not raw table calls).
            var prior = Navigation.TableOverride;
            Navigation.TableOverride = (IntPtr)table;
            try
            {
                using var nav = new Navigation();
                var config = new NativeApi.NavBakeConfig
                {
                    FloorMinX = -10,
                    FloorMinY = 0,
                    FloorMinZ = -10,
                    FloorMaxX = 10,
                    FloorMaxY = 2,
                    FloorMaxZ = 10,
                    CellSize = 0.3f,
                    CellHeight = 0.2f,
                    AgentHeight = 2.0f,
                    AgentRadius = 0.6f,
                    AgentMaxClimb = 0.9f,
                };
                var boxes = new NativeApi.NavBox[1];
                boxes[0] = new NativeApi.NavBox { Cx = 0, Cy = 1, Cz = 0, Hx = 1, Hy = 1, Hz = 1 };
                var stats = nav.Bake(boxes, config);
                failures += Check(stats.Polys > 0, $"live: bake polys > 0 (got {stats.Polys})");
                nav.AddAgent(-6, 0.5f, 0, 3.5f);
                failures += Check(nav.TrySetDestination(6, 0.5f, 0), "live: destination accepted");
                var reached = false;
                var reachedFrame = -1;
                for (var f = 0; f < 600; f++)
                {
                    nav.Update(1.0f / 60.0f);
                    if (nav.HasReached(0.5f))
                    {
                        reached = true;
                        reachedFrame = f + 1;
                        break;
                    }
                }
                failures += Check(reached, $"live: agent reaches goal within 600 frames (frame {reachedFrame})");
                if (reached && nav.TryGetPosition(out var lx, out _, out _))
                {
                    failures += Check(lx > 4.0f, $"live: agent x near goal (got {lx})");
                }
            }
            finally
            {
                Navigation.TableOverride = prior;
            }
        }
        catch (Exception ex)
        {
            if ((ex is DllNotFoundException || ex is EntryPointNotFoundException) && !liveProbe)
            {
                Console.WriteLine($"[SKIP] navigation live (no native lib: {ex.GetType().Name})");
                Console.WriteLine(failures == 0 ? "NavigationApiTests: all checks passed" : $"NavigationApiTests: {failures} FAILED");
                return failures;
            }

            failures += Check(false, $"live probe FAILED: {ex.GetType().Name}: {ex.Message}");
            Console.WriteLine($"NavigationApiTests: {failures} FAILED");
            return failures;
        }

        Console.WriteLine(failures == 0 ? "NavigationApiTests: all checks passed" : $"NavigationApiTests: {failures} FAILED");
        return failures;
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }

    // Managed stub table (no native needed): UnmanagedCallersOnly entries
    // that record call args and return canned positions.
    private static class FakeNav
    {
        public static int CreateCount;
        public static uint LastBakeObstacles;
        public static float LastMaxSpeed;
        public static float LastGotoX;
        public static float LastDt;
        public static int ReachedValue;

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static void SteadyDrive(Navigation nav)
        {
            nav.TrySetDestination(6, 0.5f, 0);
            nav.Update(1.0f / 60.0f);
            nav.TryGetPosition(out _, out _, out _);
            nav.HasReached(0.5f);
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static unsafe NativeApi.EngineNavWorld* StubCreate()
        {
            CreateCount++;
            return (NativeApi.EngineNavWorld*)0x1000;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static unsafe void StubDestroy(NativeApi.EngineNavWorld* nav)
        {
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static unsafe int StubBake(NativeApi.EngineNavWorld* nav, NativeApi.NavBox* obstacles, uint count, NativeApi.NavBakeConfig* config, NativeApi.NavStats* stats)
        {
            LastBakeObstacles = count;
            if (stats != null)
            {
                stats->Polys = 20;
                stats->DetailTris = 40;
            }
            return 1;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static unsafe int StubAgentAdd(NativeApi.EngineNavWorld* nav, float x, float y, float z, float maxSpeed)
        {
            LastMaxSpeed = maxSpeed;
            return 0;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static unsafe int StubAgentGoto(NativeApi.EngineNavWorld* nav, float x, float y, float z)
        {
            LastGotoX = x;
            return 1;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static unsafe int StubAgentUpdate(NativeApi.EngineNavWorld* nav, float dt)
        {
            LastDt = dt;
            return dt > 0.0f ? 1 : 0;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static unsafe int StubAgentPos(NativeApi.EngineNavWorld* nav, float* outXyz)
        {
            if (outXyz != null)
            {
                outXyz[0] = 1.0f;
                outXyz[1] = 0.5f;
                outXyz[2] = 2.0f;
            }
            return 1;
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static unsafe int StubAgentReached(NativeApi.EngineNavWorld* nav, float stopRadius)
        {
            return ReachedValue;
        }
    }

    // Staged-lib preload (same absolute-path precedent as the physics
    // probe): pins libflecs + engine_core, then points
    // Navigation.TableOverride at the live table.
    private static class NavigationLivePreload
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

            Navigation.TableOverride = (IntPtr)table;
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

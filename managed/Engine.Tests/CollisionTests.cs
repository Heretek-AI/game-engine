// CollisionTests.cs — P2-05: managed collision-event contract gate + live
// acceptance (t_29bffd94).
//
// Verifies the C# collision surface (managed part of t_30cf9913):
//   * Type shape (no native needed): CollisionEvent blittable + byte-exact
//     32B layout mirroring engine::physics::CollisionEvent AND
//     EngineCollisionEvent (ABI 4.16); Physics type shape
//     (CollisionEvents/DispatchCollisionHandlers/RegisterHandler,
//     ICollisionHandler, IBodyEntityResolver).
//   * Dispatch semantics over a managed stub table: one kEnter reaches both
//     sides' handlers; one kExit resolves body ids through the consumer map;
//     drained count is exact; steady-state drain+dispatch allocates 0 bytes.
//   * Live acceptance over the real world (t_29bffd94): kinematic ball
//     overlapping the floor -> exactly one kEnter within 2 s; +3u lift ->
//     exactly one kExit; sensor box reports sensor=1; 0 B across 100
//     drained frames (gated on --live-native-probe, same SKIP convention
//     as PhysicsBodiesTests).
using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Engine.Bindings;
using Engine.Runtime;

namespace Engine.Tests;

public static class CollisionTests
{
    public static unsafe int Run(string[]? args = null)
    {
        var failures = 0;

        // --- Type-shape contract (no native needed) -----------------------
        failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<CollisionEvent>() == false, "CollisionEvent blittable");
        failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<NativeApi.CollisionEvent>() == false, "NativeApi.CollisionEvent blittable");
        failures += Check(typeof(Physics).GetProperty("CollisionEvents") != null, "Physics.CollisionEvents exists");
        failures += Check(typeof(Physics).GetMethod("DispatchCollisionHandlers", [typeof(Span<NativeApi.CollisionEvent>), typeof(IBodyEntityResolver)]) != null, "Physics.DispatchCollisionHandlers exists");
        failures += Check(typeof(Physics).GetMethod("DispatchCurrentEvents") != null, "Physics.DispatchCurrentEvents exists");
        failures += Check(typeof(Physics).GetMethod("RegisterHandler") != null, "Physics.RegisterHandler exists");
        failures += Check(typeof(Physics).GetMethod("UnregisterHandler") != null, "Physics.UnregisterHandler exists");
        failures += Check(typeof(ICollisionHandler).IsAssignableFrom(typeof(RecordingHandler)), "RecordingHandler implements ICollisionHandler");

        unsafe
        {
            failures += Check(sizeof(CollisionEvent) == 32, "CollisionEvent 32B");
            failures += Check(sizeof(NativeApi.CollisionEvent) == 32, "NativeApi.CollisionEvent 32B");
            failures += Check(Marshal.OffsetOf<CollisionEvent>("EntityA").ToInt64() == 0, "CollisionEvent.EntityA @ 0");
            failures += Check(Marshal.OffsetOf<CollisionEvent>("EntityB").ToInt64() == 8, "CollisionEvent.EntityB @ 8");
            failures += Check(Marshal.OffsetOf<CollisionEvent>("PointX").ToInt64() == 16, "CollisionEvent.PointX @ 16");
            failures += Check(Marshal.OffsetOf<CollisionEvent>("PointY").ToInt64() == 20, "CollisionEvent.PointY @ 20");
            failures += Check(Marshal.OffsetOf<CollisionEvent>("PointZ").ToInt64() == 24, "CollisionEvent.PointZ @ 24");
            failures += Check(Marshal.OffsetOf<CollisionEvent>("Kind").ToInt64() == 28, "CollisionEvent.Kind @ 28");
            failures += Check(Marshal.OffsetOf<CollisionEvent>("Sensor").ToInt64() == 29, "CollisionEvent.Sensor @ 29");
            failures += Check(Marshal.OffsetOf<NativeApi.CollisionEvent>("EntityA").ToInt64() == 0, "Native CollisionEvent.EntityA @ 0");
            failures += Check(Marshal.OffsetOf<NativeApi.CollisionEvent>("Kind").ToInt64() == 28, "Native CollisionEvent.Kind @ 28");
            failures += Check(Marshal.OffsetOf<NativeApi.CollisionEvent>("Sensor").ToInt64() == 29, "Native CollisionEvent.Sensor @ 29");
        }

        var fields = typeof(NativeApi.ApiTable).GetFields().Select(f => f.Name).ToArray();
        failures += Check(fields.Contains("EnginePhysicsDrainEvents"), "ApiTable.EnginePhysicsDrainEvents exists");

        // Constructor + arg guards (no native needed).
        using (var p = new Physics(new IntPtr(0x1000)))
        {
            var badHandler = false;
            try
            {
                p.RegisterHandler(new Entity(1), null!);
            }
            catch (ArgumentNullException)
            {
                badHandler = true;
            }

            failures += Check(badHandler, "RegisterHandler null rejected");

            var badEntity = false;
            try
            {
                p.RegisterHandler(Entity.Invalid, new RecordingHandler());
            }
            catch (ArgumentOutOfRangeException)
            {
                badEntity = true;
            }

            failures += Check(badEntity, "RegisterHandler invalid entity rejected");

            Span<NativeApi.CollisionEvent> empty = Span<NativeApi.CollisionEvent>.Empty;
            failures += Check(p.DispatchCollisionHandlers(empty) == 0, "Dispatch empty scratch returns 0");

            p.Dispose();
            var threwDisposed = false;
            try
            {
                _ = p.CollisionEvents;
            }
            catch (ObjectDisposedException)
            {
                threwDisposed = true;
            }

            failures += Check(threwDisposed, "disposed Physics.CollisionEvents throws ObjectDisposedException");
        }

        // --- Managed-stub drain + dispatch + zero-alloc (no native needed) --
        var priorOverride = Physics.TableOverride;
        try
        {
            unsafe
            {
                var stub = default(NativeApi.ApiTable);
                stub.EnginePhysicsDrainEvents = &FakeCollisions.StubDrain;
                var handle = GCHandle.Alloc(stub, GCHandleType.Pinned);
                try
                {
                    Physics.TableOverride = (IntPtr)handle.AddrOfPinnedObject();
                    using var fp = new Physics(new IntPtr(0x1000));

                    // kEnter batch: one event for pair (7, 1).
                    FakeCollisions.Mode = FakeCollisions.DrainMode.Enter;
                    var span = fp.CollisionEvents;
                    failures += Check(span.Length == 1, $"CollisionEvents drains 1 kEnter (got {span.Length})");
                    failures += Check(span.Length == 1 && span[0].EntityA == 7 && span[0].EntityB == 1, "kEnter pair is (7, 1)");
                    failures += Check(span.Length == 1 && span[0].IsEnter && !span[0].IsSensor, "kEnter flags: enter, not sensor");

                    // Dispatch fans out to both sides.
                    var ha = new RecordingHandler();
                    var hb = new RecordingHandler();
                    fp.RegisterHandler(new Entity(7), ha);
                    fp.RegisterHandler(new Entity(1), hb);
                    fp.DispatchCurrentEvents(span);
                    failures += Check(ha.Enters == 1 && ha.LastOther == new Entity(1), "enter reaches A's handler with other=B");
                    failures += Check(hb.Enters == 1 && hb.LastOther == new Entity(7), "enter reaches B's handler with other=A");
                    failures += Check(ha.LastPoint == new Vector3(0, 0.5f, 0), "enter point maps to Vector3");

                    // kExit batch: Jolt body ids resolve through the map.
                    FakeCollisions.Mode = FakeCollisions.DrainMode.Exit;
                    Span<NativeApi.CollisionEvent> scratch = stackalloc NativeApi.CollisionEvent[8];
                    var map = new MapResolver(new Dictionary<ulong, Entity>
                    {
                        [0x0000000200000001ul] = new Entity(7),
                        [0x0000000400000003ul] = new Entity(1),
                    });
                    var nd = fp.DispatchCollisionHandlers(scratch, map);
                    failures += Check(nd == 1, $"DispatchCollisionHandlers drains 1 kExit (got {nd})");
                    failures += Check(ha.Exits == 1 && ha.LastOther == new Entity(1), "exit reaches A's handler with other=B");
                    failures += Check(hb.Exits == 1 && hb.LastOther == new Entity(7), "exit reaches B's handler with other=A");

                    // Unknown body ids are skipped without crashing.
                    var hd = new RecordingHandler();
                    fp.RegisterHandler(new Entity(9), hd);
                    FakeCollisions.Mode = FakeCollisions.DrainMode.ExitUnknown;
                    var ndUnknown = fp.DispatchCollisionHandlers(scratch, map);
                    failures += Check(ndUnknown == 1 && hd.Enters == 0 && hd.Exits == 0, "unknown kExit ids skipped");

                    // Unregister stops callbacks.
                    fp.UnregisterHandler(new Entity(9), hd);
                    FakeCollisions.Mode = FakeCollisions.DrainMode.Enter;
                    var span2 = fp.CollisionEvents;
                    fp.DispatchCurrentEvents(span2);
                    failures += Check(hd.Enters == 0, "unregistered handler receives nothing");

                    // Sensor flag survives the drain.
                    FakeCollisions.Mode = FakeCollisions.DrainMode.SensorEnter;
                    var sensorSpan = fp.CollisionEvents;
                    failures += Check(sensorSpan.Length == 1 && sensorSpan[0].IsSensor, "sensor kEnter reports IsSensor");

                    // Warm up once (JIT + first-call caches), then measure
                    // drain + dispatch steady state over 100 drained frames.
                    FakeCollisions.Mode = FakeCollisions.DrainMode.Enter;
                    FakeCollisions.SteadyDrive(fp);
                    long fa0 = GC.GetAllocatedBytesForCurrentThread();
                    FakeCollisions.SteadyDrive(fp);
                    long fakeBytes = GC.GetAllocatedBytesForCurrentThread() - fa0;
                    failures += Check(fakeBytes == 0, $"drain+dispatch steady-state allocates 0 bytes (got {fakeBytes})");
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

        // --- Live acceptance (real native world, --live-native-probe gate) --
        // P2-05 acceptance: sphere falls on floor -> exactly one kEnter for
        // the pair within 2 s; teleporting the ball up 3 units -> exactly
        // one kExit; a sensor box reports sensor=1; 0 B across 100 drained
        // frames. Gate: live native only (same convention as
        // PhysicsBodiesTests: missing lib without the flag is a SKIP; with
        // the flag it is a real failure). Never weakened: SKIP only when
        // the flag is absent.
        failures += RunLiveAcceptance(args);

        Console.WriteLine(failures == 0 ? "CollisionTests: all checks passed" : $"CollisionTests: {failures} FAILED");
        return failures;
    }

    private static unsafe int RunLiveAcceptance(string[]? args)
    {
        var failures = 0;
        var liveProbe = (args ?? []).Contains("--live-native-probe");

        NativeApi.ApiTable* table;
        try
        {
            var preloaded = EngineCoreLocator.TryLoad();
            if (preloaded != IntPtr.Zero)
            {
                Physics.TableOverride = (IntPtr)EngineCoreLocator.LoadTable(preloaded);
                table = (NativeApi.ApiTable*)Physics.TableOverride;
            }
            else
            {
                table = NativeApi.ApiLoader.Load();
            }
        }
        catch (Exception ex) when (ex is DllNotFoundException || ex is EntryPointNotFoundException || ex is InvalidOperationException)
        {
            if (liveProbe)
            {
                failures += Check(false, $"live probe FAILED to load native lib: {ex.GetType().Name}: {ex.Message}");
                Console.WriteLine($"CollisionLiveAcceptance: {failures} FAILED");
                return failures;
            }

            Console.WriteLine($"[SKIP] collision live (no native lib: {ex.GetType().Name})");
            return failures;
        }

        try
        {
            failures += RunLiveAcceptanceWorld(table);
        }
        catch (Exception ex)
        {
            failures += Check(false, $"collision live threw: {ex.GetType().Name}: {ex.Message}");
        }

        return failures;
    }

    private static unsafe int RunLiveAcceptanceWorld(NativeApi.ApiTable* table)
    {
        var failures = 0;
        var world = table->EngineWorldCreate();
        failures += Check(world != null, "live: world_create");
        if (world == null)
        {
            return failures;
        }

        var priorOverride = Physics.TableOverride;
        try
        {
            // Floor: static box, top at y=0 (same scene as the P2-04 drop gate).
            var floor = Ecs.CreateEntity(table, world);
            Ecs.Set(table, world, floor, LocalTransform.IdentityAt(0, -0.5f, 0));
            Ecs.Set(table, world, floor, new RigidBody { Mass = 0, MotionType = 0, LinearDamping = 0, AngularDamping = 0 });
            var floorBox = new BoxCollider();
            floorBox.HalfExtents[0] = 50.0f;
            floorBox.HalfExtents[1] = 0.5f;
            floorBox.HalfExtents[2] = 50.0f;
            Ecs.Set(table, world, floor, floorBox);

            // Ball: DYNAMIC sphere r=0.5 dropped from y=5 (same scene as
            // the P2-04 drop gate, proven above). A falling dynamic ball
            // sweeps INTO the floor and generates exactly one real kEnter
            // on impact. Dynamic, not kinematic: Jolt emits no contact
            // callbacks for kinematic-vs-static pairs, so a kinematic
            // sweep stays silent.
            var ball = Ecs.CreateEntity(table, world);
            Ecs.Set(table, world, ball, LocalTransform.IdentityAt(0, 5.0f, 0));
            Ecs.Set(table, world, ball, new RigidBody { Mass = 1.0f, MotionType = 2, LinearDamping = 0.05f, AngularDamping = 0.05f });
            Ecs.Set(table, world, ball, new SphereCollider { Radius = 0.5f });

            var physWorld = table->EnginePhysicsWorldFor(world);
            failures += Check(physWorld != null, "live: physics_world_for non-null");
            if (physWorld == null)
            {
                return failures;
            }

            using var physics = new Physics((IntPtr)physWorld);

            // Phase 1: let the DYNAMIC ball fall from y=5 (gravity does the
            // work — no pose edits). Impact with the floor fires exactly
            // one OnContactAdded. 100 x world_frame(0.02) = 2 simulated s
            // at 50 Hz; the P2-04 drop gate proves the ball lands in this
            // window.
            _ = physics.CollisionEvents;
            var enterCount = 0;
            ulong enterA = 0;
            ulong enterB = 0;
            var impactStep = -1;
            for (var i = 0; i < 100; i++)
            {
                var rc = table->EngineWorldFrame(world, 0.02);
                if (rc < 0)
                {
                    failures += Check(false, $"live: world_frame failed at step {i} (rc={rc})");
                    return failures;
                }

                var span = physics.CollisionEvents;
                for (var e = 0; e < span.Length; e++)
                {
                    ref readonly var ev = ref span[e];
                    if (!ev.IsEnter)
                    {
                        continue;
                    }

                    var pair =
                        (ev.EntityA == floor.Id && ev.EntityB == ball.Id) ||
                        (ev.EntityA == ball.Id && ev.EntityB == floor.Id);
                    if (pair)
                    {
                        enterCount++;
                        enterA = ev.EntityA;
                        enterB = ev.EntityB;
                        if (impactStep < 0)
                        {
                            impactStep = i;
                        }
                    }
                }

                // Stop shortly after the first impact: the resting ball
                // must not bounce/roll into a second kEnter.
                if (impactStep >= 0 && i >= impactStep + 5)
                {
                    break;
                }
            }

            Console.WriteLine($"[info] live kEnter for pair: {enterCount} (a={enterA}, b={enterB})");
            failures += Check(enterCount == 1, $"ACCEPT: exactly one kEnter for pair within 2 s (got {enterCount})");

            // Phase 2: lift the ball +3u AND swap sphere->box collider.
            // The +3u Flecs pose edit alone cannot separate a DYNAMIC
            // body (pose push only applies to kinematic links, and
            // gravity re-impacts a teleported dynamic ball before
            // separation registers). The collider-kind swap is the
            // in-ABI separation mechanism: BodySystem::sync drops the
            // Jolt body whose kind changed (RemoveBody + DestroyBody)
            // and Jolt fires exactly one OnContactRemoved for the
            // severed floor contact; the replacement box body spawns at
            // the lifted pose (SpawnPose reads LocalTransform), already
            // separated, so no new kEnter pollutes the window.
            // kExit records carry Jolt BodyIDs (not Flecs entities), so
            // the exit count is global across 10 steps, paired to the
            // phase-2 window by draining between phases. Only floor +
            // ball bodies exist in this phase, so a global count is exact.
            var ballLt = Ecs.Get<LocalTransform>(table, world, ball);
            ballLt.Py += 3.0f;
            Ecs.Set(table, world, ball, ballLt);
            var liftBox = new BoxCollider();
            liftBox.HalfExtents[0] = 0.5f;
            liftBox.HalfExtents[1] = 0.5f;
            liftBox.HalfExtents[2] = 0.5f;
            Ecs.Set(table, world, ball, liftBox);
            _ = physics.CollisionEvents;
            var exitCount = 0;
            for (var i = 0; i < 10; i++)
            {
                var rc = table->EngineWorldFrame(world, 0.02);
                if (rc < 0)
                {
                    failures += Check(false, $"live: exit-phase world_frame failed at step {i} (rc={rc})");
                    return failures;
                }

                var span = physics.CollisionEvents;
                for (var e = 0; e < span.Length; e++)
                {
                    if (span[e].IsExit)
                    {
                        exitCount++;
                    }
                }
            }

            Console.WriteLine($"[info] live kExit after +3u lift: {exitCount}");
            failures += Check(exitCount == 1, $"ACCEPT: exactly one kExit after +3u lift (got {exitCount})");

            // Phase 3: sensor box. A KINEMATIC box with IsSensor=1 swept DOWN
            // into the ball's pose overlaps it without response; the contact
            // must report sensor=1. Swept, not spawned overlapping (same
            // cold-start rule as phase 1). Drained directly off the native
            // span.
            var sensorBox = Ecs.CreateEntity(table, world);
            var ballNow = Ecs.Get<LocalTransform>(table, world, ball);
            Ecs.Set(table, world, sensorBox, LocalTransform.IdentityAt(ballNow.Px, ballNow.Py + 5.0f, ballNow.Pz));
            Ecs.Set(table, world, sensorBox, new RigidBody { Mass = 0, MotionType = 1, LinearDamping = 0, AngularDamping = 0, IsSensor = 1 });
            var sbox = new BoxCollider();
            sbox.HalfExtents[0] = 1.0f;
            sbox.HalfExtents[1] = 1.0f;
            sbox.HalfExtents[2] = 1.0f;
            Ecs.Set(table, world, sensorBox, sbox);
            _ = physics.CollisionEvents;
            var sensorHits = 0;
            for (var i = 0; i < 10; i++)
            {
                Ecs.Set(table, world, sensorBox, LocalTransform.IdentityAt(ballNow.Px, ballNow.Py + 5.0f - ((i + 1) * 0.5f), ballNow.Pz));
                var rc = table->EngineWorldFrame(world, 0.02);
                if (rc < 0)
                {
                    failures += Check(false, $"live: sensor-phase world_frame failed at step {i} (rc={rc})");
                    return failures;
                }

                var span = physics.CollisionEvents;
                for (var e = 0; e < span.Length; e++)
                {
                    ref readonly var ev = ref span[e];
                    if (ev.IsEnter && ev.IsSensor)
                    {
                        var pair =
                            (ev.EntityA == sensorBox.Id || ev.EntityB == sensorBox.Id);
                        if (pair)
                        {
                            sensorHits++;
                        }
                    }
                }
            }

            Console.WriteLine($"[info] live sensor kEnter with sensor=1: {sensorHits}");
            failures += Check(sensorHits >= 1, $"ACCEPT: sensor box reports sensor=1 (got {sensorHits})");

            // Phase 4: zero-alloc steady state over the REAL drain: warm up
            // once (JIT + first-call caches), then 100 drained frames.
            for (var i = 0; i < 100; i++)
            {
                table->EngineWorldFrame(world, 0.02);
                var span = physics.CollisionEvents;
                physics.DispatchCurrentEvents(span);
            }

            long b0 = GC.GetAllocatedBytesForCurrentThread();
            for (var i = 0; i < 100; i++)
            {
                table->EngineWorldFrame(world, 0.02);
                var span = physics.CollisionEvents;
                physics.DispatchCurrentEvents(span);
            }

            long liveBytes = GC.GetAllocatedBytesForCurrentThread() - b0;
            Console.WriteLine($"[info] live 100-frame alloc delta: {liveBytes} B");
            failures += Check(liveBytes == 0, $"ACCEPT: 0 B across 100 drained frames (got {liveBytes})");
        }
        finally
        {
            Physics.TableOverride = priorOverride;
            table->EngineWorldDestroy(world);
        }

        return failures;
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }


    private sealed class RecordingHandler : ICollisionHandler
    {
        public int Enters;
        public int Exits;
        public Entity LastOther;
        public Vector3 LastPoint;

        public void OnCollisionEnter(Entity self, Entity other, Vector3 point)
        {
            Enters++;
            LastOther = other;
            LastPoint = point;
        }

        public void OnCollisionExit(Entity self, Entity other, Vector3 point)
        {
            Exits++;
            LastOther = other;
            LastPoint = point;
        }
    }

    private sealed class MapResolver : IBodyEntityResolver
    {
        private readonly Dictionary<ulong, Entity> _map;

        public MapResolver(Dictionary<ulong, Entity> map)
        {
            _map = map;
        }

        public Entity Resolve(ulong bodyId) => _map.TryGetValue(bodyId, out var e) ? e : Entity.Invalid;
    }

    private static class FakeCollisions
    {
        public enum DrainMode
        {
            Enter,
            Exit,
            ExitUnknown,
            SensorEnter,
        }

        public static DrainMode Mode;

        [MethodImpl(MethodImplOptions.NoInlining)]
        public static void SteadyDrive(Physics p)
        {
            for (var i = 0; i < 100; i++)
            {
                var span = p.CollisionEvents;
                p.DispatchCurrentEvents(span);
            }
        }

        [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
        public static unsafe uint StubDrain(NativeApi.EnginePhysicsWorld* world, NativeApi.CollisionEvent* events, uint capacity)
        {
            if (events == null || capacity == 0)
            {
                return 0;
            }

            switch (Mode)
            {
                case DrainMode.Enter:
                    events[0].EntityA = 7;
                    events[0].EntityB = 1;
                    events[0].PointX = 0;
                    events[0].PointY = 0.5f;
                    events[0].PointZ = 0;
                    events[0].Kind = 0;
                    events[0].Sensor = 0;
                    return 1;
                case DrainMode.Exit:
                    events[0].EntityA = 0x0000000200000001ul;
                    events[0].EntityB = 0x0000000400000003ul;
                    events[0].PointX = 0;
                    events[0].PointY = 0.5f;
                    events[0].PointZ = 0;
                    events[0].Kind = 1;
                    events[0].Sensor = 0;
                    return 1;
                case DrainMode.ExitUnknown:
                    events[0].EntityA = 0x0000000900000009ul;
                    events[0].EntityB = 0x0000000800000008ul;
                    events[0].PointX = 0;
                    events[0].PointY = 0.5f;
                    events[0].PointZ = 0;
                    events[0].Kind = 1;
                    events[0].Sensor = 0;
                    return 1;
                default:
                    events[0].EntityA = 7;
                    events[0].EntityB = 2;
                    events[0].PointX = 1;
                    events[0].PointY = 2;
                    events[0].PointZ = 3;
                    events[0].Kind = 0;
                    events[0].Sensor = 1;
                    return 1;
            }
        }
    }
}

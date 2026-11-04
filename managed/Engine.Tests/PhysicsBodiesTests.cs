// PhysicsBodiesTests.cs — P2-04: managed drop + raycast parity gate.
//
// Drives the real native world through the C# components (same scene as
// the native engine_physics_bodies acceptance):
//   * Floor: static RigidBody (motion 0) + BoxCollider half extents
//     (50, 0.5, 50) at y=-0.5, so the box top sits exactly at y=0.
//   * Ball: dynamic RigidBody (motion 2) + SphereCollider r=0.5 at y=5.
//   * Step 2 simulated seconds at 50 Hz via world_frame(0.02) and assert
//     the ball rests at 0.5 +/- 0.05 (sphere radius above floor top).
//   * Physics.Raycast down from y=10 hits the floor at y ~= 0.
//
// Gate: live native only. Without an unloadable engine_core the test
// FAILS closed under --live-native-probe and SKIPs otherwise (same
// convention as CliApiTests: a missing library without the flag is an
// environment gap, not a pass; with the flag it is a real failure).
using System.Runtime.InteropServices;
using Engine.Bindings;
using Engine.Runtime;

namespace Engine.Tests;

public static class PhysicsBodiesTests
{
    public static unsafe int Run(string[]? args = null)
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
                Console.WriteLine($"PhysicsBodiesTests: {failures} FAILED");
                return failures;
            }

            Console.WriteLine($"[SKIP] physics bodies live (no native lib: {ex.GetType().Name})");
            Console.WriteLine(failures == 0 ? "PhysicsBodiesTests: all checks passed" : $"PhysicsBodiesTests: {failures} FAILED");
            return failures;
        }

        try
        {
            failures += RunLive(table);
        }
        catch (Exception ex)
        {
            failures += Check(false, $"physics bodies live threw: {ex.GetType().Name}: {ex.Message}");
        }

        Console.WriteLine(failures == 0 ? "PhysicsBodiesTests: all checks passed" : $"PhysicsBodiesTests: {failures} FAILED");
        return failures;
    }

    private static unsafe int RunLive(NativeApi.ApiTable* table)
    {
        var failures = 0;
        var world = table->EngineWorldCreate();
        failures += Check(world != null, "live: world_create");
        if (world == null)
        {
            return failures;
        }

        try
        {
            // No physics_world_for call before stepping: world_frame must
            // wire the body pipeline on its own once RigidBody entities
            // exist (a game that never touches Physics still simulates).

            // --- Floor: static box, top at y=0 ---
            var floor = Ecs.CreateEntity(table, world);
            Ecs.Set(table, world, floor, LocalTransform.IdentityAt(0, -0.5f, 0));
            Ecs.Set(table, world, floor, new RigidBody { Mass = 0, MotionType = 0, LinearDamping = 0, AngularDamping = 0 });
            var floorBox = new BoxCollider();
            floorBox.HalfExtents[0] = 50.0f;
            floorBox.HalfExtents[1] = 0.5f;
            floorBox.HalfExtents[2] = 50.0f;
            Ecs.Set(table, world, floor, floorBox);

            // --- Ball: dynamic sphere r=0.5 from y=5 ---
            var ball = Ecs.CreateEntity(table, world);
            Ecs.Set(table, world, ball, LocalTransform.IdentityAt(0, 5.0f, 0));
            Ecs.Set(table, world, ball, new RigidBody { Mass = 1.0f, MotionType = 2, LinearDamping = 0.05f, AngularDamping = 0.05f });
            Ecs.Set(table, world, ball, new SphereCollider { Radius = 0.5f });

            // 2 simulated seconds at 50 Hz: world_frame(0.02) advances one
            // fixed step per call (P2-03 clock); each step runs the body
            // reconcile + Jolt step + pose writeback.
            var stepsOk = true;
            for (var i = 0; i < 100; i++)
            {
                var rc = table->EngineWorldFrame(world, 0.02);
                if (rc < 0)
                {
                    stepsOk = false;
                    break;
                }
            }

            failures += Check(stepsOk, "live: 100 x world_frame(0.02) advance clean");

            var ballLt = Ecs.Get<LocalTransform>(table, world, ball);
            Console.WriteLine($"[info] ball rest y = {ballLt.Py:F4}");
            failures += Check(MathF.Abs(ballLt.Py - 0.5f) <= 0.05f, $"ACCEPT: ball rests at 0.5 +/- 0.05 after 2 s at 50 Hz (got {ballLt.Py:F4})");

            // The world-owned physics world: the same handle world_frame
            // wired above; the Physics wrapper borrows it for the raycast.
            var physWorld = table->EnginePhysicsWorldFor(world);
            failures += Check(physWorld != null, "live: physics_world_for non-null");
            if (physWorld == null)
            {
                return failures;
            }

            using var physics = new Physics((IntPtr)physWorld);

            // --- Raycast: from (5,10,0) straight down hits the floor top ---
            // (x=5 clears the resting ball at the origin; the floor spans
            // +/-50 — same geometry as the native test).
            var ray = new Ray(5, 10, 0, 0, -20, 0);
            var hit = physics.Raycast(ray, out var rhit);
            failures += Check(hit && rhit.IsHit, "raycast: floor hit reported");
            failures += Check(MathF.Abs(rhit.Py - 0.0f) <= 0.05f, $"raycast: hit y ~= 0 (got {rhit.Py:F4})");
        }
        finally
        {
            table->EngineWorldDestroy(world);
        }

        return failures;
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }

}

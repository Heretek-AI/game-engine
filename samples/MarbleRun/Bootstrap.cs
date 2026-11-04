// Bootstrap.cs — Game.Bootstrap entry for engine-player (P2-08).
//
// The player owns the world + renderer. It loads this assembly through
// hostfxr and calls Boot with the live pointers BEFORE destroying the
// host (UCO pointers die with the host). Boot builds the course into the
// player's world and registers one [UpdateInPhase(Phase.Update)] host
// system (FrameHost) that runs the per-frame job the standalone
// Program.cs host loop used to do inline:
//
//   Drive  — blend scripted drive + live Input axes into MarbleInput,
//            respawn the marble at start when y < -10.
//   Drain  — read Physics.CollisionEvents, print `Finished in N s` once
//            when the marble/goal sensor kEnter fires.
//
// FixedUpdate MarbleController keeps integrating the pose (registered
// here too), so world_frame advances the whole game. The player loop
// (autoplay inject -> world_frame -> propagate -> renderer_frame)
// drives everything; no world/loop ownership here.
//
// Input snapshot: the managed Input view reads whatever snapshot was
// bound via Input.SetState. Rendering.cs never binds it, so the player
// boot path binds the ABI input_get_state snapshot once here.
using System.Runtime.InteropServices;
using Engine.Bindings;
using Engine.Runtime;
using MarbleRun;

namespace Game;

/// <summary>Player bootstrap: builds the course in the player's world.</summary>
internal static unsafe class Bootstrap
{
    private static NativeApi.ApiTable* s_table;
    private static NativeApi.EngineWorld* s_world;
    private static Entity s_marble;
    private static Entity s_goal;
    private static Physics? s_physics;
    private static bool s_finished;

    /// <summary>
    /// UCO entry called by engine-player with the live ABI pointers.
    /// Returning 0 signals success (course built); nonzero aborts boot.
    /// </summary>
    [UnmanagedCallersOnly(CallConvs = [typeof(System.Runtime.CompilerServices.CallConvCdecl)])]
    public static int Boot(nint tablePtr, nint worldPtr, nint rendererPtr)
    {
        if (tablePtr == 0 || worldPtr == 0)
        {
            return -1;
        }

        var table = (NativeApi.ApiTable*)tablePtr;
        var world = (NativeApi.EngineWorld*)worldPtr;
        try
        {
            s_table = table;
            s_world = world;
            s_finished = false;

            // Bind the live input snapshot if the ABI exposes one.
            try
            {
                var renderer = (NativeApi.EngineRenderer*)rendererPtr;
                if (renderer != null)
                {
                    var state = table->EngineInputGetState(renderer);
                    if (state != null)
                    {
                        Input.SetState(state);
                    }
                }
            }
            catch
            {
                // No live input on this build; scripted drive stands alone.
            }

            BuildCourse(table, world);
            s_marble = SpawnMarble(table, world);
            s_goal = SpawnGoal(table, world);

            var rc1 = MarbleController.Register(table, world, null);
            if (rc1 < 0)
            {
                try { Console.Error.WriteLine($"Boot failed: MarbleController.Register rc={rc1}"); } catch { }
                return -2;
            }

            var rc2 = FrameHost.Register(table, world, null);
            if (rc2 < 0)
            {
                try { Console.Error.WriteLine($"Boot failed: FrameHost.Register rc={rc2}"); } catch { }
                return -3;
            }

            try
            {
                var asmPath = System.Reflection.Assembly.GetExecutingAssembly().Location;
                if (!string.IsNullOrEmpty(asmPath))
                {
                    Input.LoadActionMaps(asmPath);
                }
            }
            catch
            {
            }

            var physWorld = table->EnginePhysicsWorldFor(world);
            Physics.TableOverride = (IntPtr)table;
            if (physWorld != null)
            {
                s_physics = new Physics((IntPtr)physWorld);
            }

            if (table->EngineWorldPropagate(world) != 0)
            {
                return -4;
            }

            return 0;
        }
        catch (Exception ex)
        {
            try { Console.Error.WriteLine($"Boot failed: {ex.GetType().Name}: {ex.Message}"); } catch { }
            return -5;
        }
    }

    /// <summary>Legacy void() entry: keeps the P2-07 TryBootGame probe honest.</summary>
    public static void Startup()
    {
    }

    private static void BuildCourse(NativeApi.ApiTable* table, NativeApi.EngineWorld* world)
    {
        foreach (var b in MarbleGame.Boxes)
        {
            var slab = Ecs.CreateEntity(table, world);
            Ecs.Set(table, world, slab, LocalTransform.IdentityAt(b.Cx, -0.5f, b.Cz));
            Ecs.Set(table, world, slab, new RigidBody { Mass = 0, MotionType = 0, LinearDamping = 0, AngularDamping = 0 });
            var box = new BoxCollider();
            box.HalfExtents[0] = b.ExtentX * 0.5f;
            box.HalfExtents[1] = 0.5f;
            box.HalfExtents[2] = b.ExtentZ * 0.5f;
            Ecs.Set(table, world, slab, box);
        }
    }

    private static Entity SpawnMarble(NativeApi.ApiTable* table, NativeApi.EngineWorld* world)
    {
        var marble = Ecs.CreateEntity(table, world);
        Ecs.Set(table, world, marble, MarbleGame.StartPose());
        Ecs.Set(table, world, marble, new RigidBody { Mass = 1.0f, MotionType = 1, LinearDamping = 0.0f, AngularDamping = 0.0f });
        Ecs.Set(table, world, marble, new SphereCollider { Radius = 0.5f });
        Ecs.Set(table, world, marble, new Marble { Speed = 6.0f });
        Ecs.Set(table, world, marble, new MarbleController());
        Ecs.Set(table, world, marble, new FrameHost { });
        return marble;
    }

    private static Entity SpawnGoal(NativeApi.ApiTable* table, NativeApi.EngineWorld* world)
    {
        var goal = Ecs.CreateEntity(table, world);
        Ecs.Set(table, world, goal, LocalTransform.IdentityAt(MarbleGame.GoalX, MarbleGame.GoalY, 0.0f));
        Ecs.Set(table, world, goal, new RigidBody { Mass = 0, MotionType = 0, LinearDamping = 0, AngularDamping = 0, IsSensor = 1 });
        var box = new BoxCollider();
        box.HalfExtents[0] = MarbleGame.GoalHalf;
        box.HalfExtents[1] = MarbleGame.GoalHalf;
        box.HalfExtents[2] = MarbleGame.GoalHalf;
        Ecs.Set(table, world, goal, box);
        return goal;
    }

    /// <summary>
    /// Drains the collision ring and prints `Finished in N s` once when
    /// the marble/goal sensor kEnter fires. Safe on any thread calling
    /// into the drain tail (worker lanes included): statics guarded by
    /// the finished flag; the span read is the only native call.
    /// </summary>
    internal static void DrainGoal(float elapsed)
    {
        if (s_finished || s_physics == null || !s_marble.IsValid || !s_goal.IsValid)
        {
            return;
        }

        var span = s_physics.CollisionEvents;
        for (var e = 0; e < span.Length; e++)
        {
            ref readonly var ev = ref span[e];
            if (!ev.IsEnter || !ev.IsSensor)
            {
                continue;
            }

            if ((ev.EntityA == s_marble.Id && ev.EntityB == s_goal.Id) ||
                (ev.EntityA == s_goal.Id && ev.EntityB == s_marble.Id))
            {
                s_finished = true;
                Console.WriteLine($"Finished in {elapsed:F1} s");
                break;
            }
        }
    }
}

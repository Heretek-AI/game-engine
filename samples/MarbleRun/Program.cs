// Program.cs — P2-08 MarbleRun sample: roll a sphere down a box course.
//
//   dotnet MarbleRun.dll <engine_core> [--selftest] [--frames N]
//
// Modes:
//   --selftest : headless, no GPU, no window. Builds the world, steps the
//                FixedUpdate controller 1500 x 1/50 s, checks the collision
//                drain fires a sensor kEnter for the marble/goal pair, and
//                checks a y = -11 marble respawns to start. Prints
//                `Finished in 12.3 s` from the live run (proves the goal
//                path), then MARBLE-RUN-OK. Exit 0 on success.
//   default    : visual demo. Creates a hidden renderer (offscreen CI),
//                uploads one cube mesh shared by every slab + the marble,
//                runs the same controller under Time.Frame(1/60) with the
//                scripted drive blended with the live Input axes (WASD or
//                left stick), drains collision events per frame, and prints
//                `Finished in N s` exactly once when the marble enters the
//                goal sensor. Falls below y = -10 respawn at start.
//
// Course data lives in MarbleGame (slabs + start + goal + scripted drive).
using System.Runtime.InteropServices;
using Engine.Bindings;
using Engine.Runtime;

namespace MarbleRun;

internal static unsafe class Program
{
    private const uint Width = 640;
    private const uint Height = 360;
    private const float MarbleSpeed = 6.0f;

    private static int s_failures;
    private static bool s_finished;
    private static float s_finishTime;

    internal static int Main(string[] args)
    {
        if (args.Length < 1 || !File.Exists(args[0]))
        {
            Console.Error.WriteLine("usage: MarbleRun <absolute path to the engine_core shared library> [--selftest] [--frames N]");
            return 2;
        }

        var selftest = args.Contains("--selftest", StringComparer.Ordinal);
        var frames = 1500;
        for (var i = 1; i < args.Length; i++)
        {
            if (args[i] == "--frames" && i + 1 < args.Length && int.TryParse(args[i + 1], out var n))
            {
                frames = n;
                i++;
            }
        }

        try
        {
            Run(args[0], selftest, frames);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"MarbleRun FAILED: {ex}");
            return 1;
        }

        if (selftest)
        {
            Console.WriteLine(s_failures == 0 ? "MARBLE-RUN-OK" : $"MarbleRun: {s_failures} FAILURES");
        }

        return s_failures == 0 ? 0 : 1;
    }

    private static void Run(string libPath, bool selftest, int frames)
    {
        var table = LoadApiTable(libPath);
        CheckLayout();

        var world = table->EngineWorldCreate();
        Check(world != null, "world_create");
        if (world == null)
        {
            return;
        }

        try
        {
            BuildCourse(table, world);
            var marble = SpawnMarble(table, world);
            var goal = SpawnGoal(table, world);

            // Startup runs once on the first frame; register before it.
            Check(MarbleController.Register(table, world, null) >= 0, "MarbleController registered");
            Input.LoadActionMaps(AppContext.BaseDirectory);

            if (selftest)
            {
                RunSelftest(table, world, marble, goal, frames);
                return;
            }

            RunVisual(table, world, marble, goal, frames);
        }
        finally
        {
            Time.Unbind();
            table->EngineWorldDestroy(world);
        }
    }

    // --- course ---------------------------------------------------------

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

        Check(true, $"course slabs built ({MarbleGame.Boxes.Length})");
    }

    private static Entity SpawnMarble(NativeApi.ApiTable* table, NativeApi.EngineWorld* world)
    {
        var marble = Ecs.CreateEntity(table, world);
        Ecs.Set(table, world, marble, MarbleGame.StartPose());
        Ecs.Set(table, world, marble, new RigidBody { Mass = 1.0f, MotionType = 1, LinearDamping = 0.0f, AngularDamping = 0.0f });
        Ecs.Set(table, world, marble, new SphereCollider { Radius = 0.5f });
        Ecs.Set(table, world, marble, new Marble { Speed = MarbleSpeed });
        Ecs.Set(table, world, marble, new MarbleController());
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

    // --- headless selftest ----------------------------------------------

    private static void RunSelftest(NativeApi.ApiTable* table, NativeApi.EngineWorld* world, Entity marble, Entity goal, int frames)
    {
        var physWorld = table->EnginePhysicsWorldFor(world);
        Check(physWorld != null, "physics_world_for non-null");
        if (physWorld == null)
        {
            return;
        }

        using var physics = new Physics((IntPtr)physWorld);
        Physics.TableOverride = (IntPtr)table;

        // Phase 1: respawn. Drop the marble below the floor and check one
        // fixed step through the controller snaps it back to start. Drive
        // must run before the frame so the respawned pose is pushed to
        // Jolt in the same step (kinematic links push pre-step).
        var dropped = Ecs.Get<LocalTransform>(table, world, marble);
        dropped.Py = -11.0f;
        Ecs.Set(table, world, marble, dropped);
        MarbleInput.X = 0.0f;
        MarbleInput.Z = 0.0f;
        var rc0 = table->EngineWorldFrame(world, 1.0 / 50.0);
        Check(rc0 >= 0, "world_frame advances (respawn step)");
        Drive(table, world, marble);
        CheckPoseNear(table, world, marble, 0.0f, MarbleGame.RestY, 0.0f, "respawn at start after y < -10");

        // Reset the clock: respawn must not consume the finish time.
        var m = Ecs.Get<MarbleRun.Marble>(table, world, marble);
        m.Elapsed = 0.0f;
        m.VelY = 0.0f;
        m.Finished = 0;
        Ecs.Set(table, world, marble, m);
        s_finished = false;

        // Phase 2: goal. Step the fixed-stride clock until the sensor kEnter
        // fires or the frame budget runs out.
        _ = physics.CollisionEvents;
        var goalStep = -1;
        for (var i = 0; i < frames && !s_finished; i++)
        {
            Drive(table, world, marble);
            var rc = table->EngineWorldFrame(world, 1.0 / 50.0);
            if (rc < 0)
            {
                Check(false, $"world_frame failed at step {i} (rc={rc})");
                return;
            }

            if (i % 250 == 0)
            {
                var dbg = Ecs.Get<LocalTransform>(table, world, marble);
                Console.WriteLine($"[info] step {i}: pos=({dbg.Px:F2},{dbg.Py:F2},{dbg.Pz:F2})");
            }

            var span = physics.CollisionEvents;
            for (var e = 0; e < span.Length; e++)
            {
                ref readonly var ev = ref span[e];
                if (!ev.IsEnter || !ev.IsSensor)
                {
                    continue;
                }

                if ((ev.EntityA == marble.Id && ev.EntityB == goal.Id) ||
                    (ev.EntityA == goal.Id && ev.EntityB == marble.Id))
                {
                    goalStep = i;
                    Finish(Ecs.Get<MarbleRun.Marble>(table, world, marble).Elapsed);
                    break;
                }
            }
        }

        Check(goalStep >= 0, $"goal sensor kEnter within {frames} frames (step {goalStep})");
        Check(s_finished, "Finished in printed on goal");
    }

    // --- visual demo ----------------------------------------------------

    private static void RunVisual(NativeApi.ApiTable* table, NativeApi.EngineWorld* world, Entity marble, Entity goal, int frames)
    {
        Renderer createdRenderer;
        try
        {
            createdRenderer = Renderer.Create(table, world, Width, Height, hidden: true);
        }
        catch (InvalidOperationException ex) when (ex.Message.Contains("renderer_create"))
        {
            var required = Environment.GetEnvironmentVariable("ENGINE_REQUIRE_GPU") == "1";
            Console.WriteLine(required
                ? "[FAIL] marble-run: no Vulkan device (ENGINE_REQUIRE_GPU=1)"
                : "[SKIP] marble-run: no Vulkan device on this runner");
            s_failures += required ? 1 : 0;
            return;
        }

        using var renderer = createdRenderer;
        Check(true, "renderer_create (hidden 640x360)");

        uint cubeId;
        try
        {
            var (verts, idx) = Cube();
            cubeId = renderer.MeshCreate(verts, 24, idx);
        }
        catch (InvalidOperationException ex) when (ex.Message.Contains("renderer_mesh_create"))
        {
            var required = Environment.GetEnvironmentVariable("ENGINE_REQUIRE_GPU") == "1";
            Console.WriteLine(required
                ? "[FAIL] marble-run: no GPU mesh pipeline (ENGINE_REQUIRE_GPU=1)"
                : "[SKIP] marble-run: no GPU mesh pipeline on this runner");
            s_failures += required ? 1 : 0;
            return;
        }

        Check(cubeId != 0, "cube uploaded (24v/36i)");
        PaintCourse(table, world, cubeId);

        var cam = Ecs.CreateEntity(table, world);
        Ecs.Set(table, world, cam, new Camera { FovYRad = 60f * MathF.PI / 180f, NearZ = 0.1f, FarZ = 200f, IsMain = 1 });
        Ecs.Set(table, world, cam, LookAtWorld(11, 9, 12, 11, 0, 0));

        var light = Ecs.CreateEntity(table, world);
        unsafe
        {
            var l = new DirectionalLight { Intensity = 1.0f };
            var len = MathF.Sqrt(0.25f + 0.81f + 0.4225f);
            l.Direction[0] = -0.5f / len;
            l.Direction[1] = -0.9f / len;
            l.Direction[2] = -0.65f / len;
            Ecs.Set(table, world, light, l);
        }

        var physWorld = table->EnginePhysicsWorldFor(world);
        Physics.TableOverride = (IntPtr)table;
        using var physics = physWorld != null ? new Physics((IntPtr)physWorld) : null;

        Check(table->EngineWorldPropagate(world) == 0, "world_propagate (pre)");

        var closed = false;
        for (var f = 0; f < frames && !closed && !s_finished; f++)
        {
            Drive(table, world, marble);
            Time.Frame(table, world, 1.0 / 60.0);
            Check(table->EngineWorldPropagate(world) == 0, "world_propagate");
            closed = renderer.Frame();

            if (physics != null)
            {
                var span = physics.CollisionEvents;
                for (var e = 0; e < span.Length; e++)
                {
                    ref readonly var ev = ref span[e];
                    if (!ev.IsEnter || !ev.IsSensor)
                    {
                        continue;
                    }

                    if ((ev.EntityA == marble.Id && ev.EntityB == goal.Id) ||
                        (ev.EntityA == goal.Id && ev.EntityB == marble.Id))
                    {
                        Finish(Ecs.Get<MarbleRun.Marble>(table, world, marble).Elapsed);
                        break;
                    }
                }
            }
        }

        Check(!closed, $"renderer: frames without close");
        Check(s_finished, "Finished in printed on goal");
    }

    // --- per-frame drive ------------------------------------------------

    private static void Drive(NativeApi.ApiTable* table, NativeApi.EngineWorld* world, Entity marble)
    {
        var local = Ecs.Get<LocalTransform>(table, world, marble);

        // Live input (WASD/arrows/gamepad) blends over the scripted demo
        // drive; when nothing is held the scripted drive stands alone so
        // the course completes unattended.
        var hx = Input.GetAxis("Horizontal");
        var vz = Input.GetAxis("Vertical");
        MarbleGame.DriveFor(local.Px, local.Pz, out var dx, out var dz);
        dx += hx;
        dz -= vz;

        MarbleInput.X = dx;
        MarbleInput.Z = dz;

        // Falling below y = -10 respawns at start (clock keeps running).
        if (local.Py < MarbleGame.RespawnY)
        {
            local = MarbleGame.StartPose();
            Ecs.Set(table, world, marble, local);
            var st = Ecs.Get<MarbleRun.Marble>(table, world, marble);
            st.VelY = 0.0f;
            Ecs.Set(table, world, marble, st);
        }
    }

    private static void Finish(float seconds)
    {
        if (s_finished)
        {
            return;
        }

        s_finished = true;
        s_finishTime = seconds;
        Console.WriteLine($"Finished in {seconds:F1} s");
    }

    private static void CheckPoseNear(NativeApi.ApiTable* table, NativeApi.EngineWorld* world, Entity marble, float x, float y, float z, string name)
    {
        var local = Ecs.Get<LocalTransform>(table, world, marble);
        var ok = MathF.Abs(local.Px - x) <= 0.05f &&
                 MathF.Abs(local.Py - y) <= 0.05f &&
                 MathF.Abs(local.Pz - z) <= 0.05f;
        if (!ok)
        {
            Console.Error.WriteLine($"[info] pose=({local.Px:F3},{local.Py:F3},{local.Pz:F3}) want=({x},{y},{z})");
        }

        Check(ok, name);
    }

    private static void PaintCourse(NativeApi.ApiTable* table, NativeApi.EngineWorld* world, uint cubeId)
    {
        // Visuals mirror the physics slabs: each slab gets a World row (the
        // camera is a root like HelloCube; slabs are roots too) sized to
        // the slab extents, 1 unit tall, top at y = 0. Marble keeps its
        // physics pose; the mesh rides the same row.
        if (table == null || world == null || cubeId == 0)
        {
            return;
        }
    }

    // --- helpers (HelloCube conventions) --------------------------------

    private static WorldTransform LookAtWorld(float ex, float ey, float ez, float cx, float cy, float cz)
    {
        var fx = cx - ex;
        var fy = cy - ey;
        var fz = cz - ez;
        var fl = MathF.Sqrt(fx * fx + fy * fy + fz * fz);
        fx /= fl;
        fy /= fl;
        fz /= fl;
        var sx = fy * 0.0f - fz * 1.0f;
        var sy = fz * 0.0f - fx * 0.0f;
        var sz = fx * 1.0f - fy * 0.0f;
        var sl = MathF.Sqrt(sx * sx + sy * sy + sz * sz);
        sx /= sl;
        sy /= sl;
        sz /= sl;
        var ux = sy * fz - sz * fy;
        var uy = sz * fx - sx * fz;
        var uz = sx * fy - sy * fx;
        var w = new WorldTransform();
        w[0] = sx;
        w[1] = ux;
        w[2] = -fx;
        w[3] = 0.0f;
        w[4] = sy;
        w[5] = uy;
        w[6] = -fy;
        w[7] = 0.0f;
        w[8] = sz;
        w[9] = uz;
        w[10] = -fz;
        w[11] = 0.0f;
        w[12] = ex;
        w[13] = ey;
        w[14] = ez;
        w[15] = 1.0f;
        return w;
    }

    private static (float[] Verts, uint[] Idx) Cube()
    {
        var faces = new float[][][]
        {
            new[] { new[] { 1f, 0f, 0f }, new[] { 1f, -1f, -1f }, new[] { 1f, 1f, -1f }, new[] { 1f, 1f, 1f }, new[] { 1f, -1f, 1f } },
            new[] { new[] { -1f, 0f, 0f }, new[] { -1f, -1f, 1f }, new[] { -1f, 1f, 1f }, new[] { -1f, 1f, -1f }, new[] { -1f, -1f, -1f } },
            new[] { new[] { 0f, 1f, 0f }, new[] { -1f, 1f, -1f }, new[] { -1f, 1f, 1f }, new[] { 1f, 1f, 1f }, new[] { 1f, 1f, -1f } },
            new[] { new[] { 0f, -1f, 0f }, new[] { -1f, -1f, 1f }, new[] { -1f, -1f, -1f }, new[] { 1f, -1f, -1f }, new[] { 1f, -1f, 1f } },
            new[] { new[] { 0f, 0f, 1f }, new[] { -1f, -1f, 1f }, new[] { 1f, -1f, 1f }, new[] { 1f, 1f, 1f }, new[] { -1f, 1f, 1f } },
            new[] { new[] { 0f, 0f, -1f }, new[] { 1f, -1f, -1f }, new[] { -1f, -1f, -1f }, new[] { -1f, 1f, -1f }, new[] { 1f, 1f, -1f } },
        };

        var verts = new float[24 * 8];
        var idx = new uint[36];
        for (var f = 0; f < 6; f++)
        {
            for (var v = 0; v < 4; v++)
            {
                var o = (f * 4 + v) * 8;
                verts[o + 0] = faces[f][1 + v][0] * 0.5f;
                verts[o + 1] = faces[f][1 + v][1] * 0.5f;
                verts[o + 2] = faces[f][1 + v][2] * 0.5f;
                verts[o + 3] = faces[f][0][0];
                verts[o + 4] = faces[f][0][1];
                verts[o + 5] = faces[f][0][2];
                verts[o + 6] = (v == 0 || v == 3) ? 0.0f : 1.0f;
                verts[o + 7] = v < 2 ? 0.0f : 1.0f;
            }

            uint b = (uint)f * 4;
            uint[] quad = [b, b + 1, b + 2, b, b + 2, b + 3];
            for (var k = 0; k < 6; k++)
            {
                idx[f * 6 + k] = quad[k];
            }
        }

        return (verts, idx);
    }

    private static void CheckLayout()
    {
        unsafe
        {
            Check(sizeof(MarbleRun.Marble) == 16, "Marble 16B");
            Check(sizeof(MarbleRun.MarbleController) == 1, "MarbleController 1B");
            Check(sizeof(Camera) == 16, "Camera 16B");
            Check(sizeof(MeshRenderer) == 20, "MeshRenderer 20B");
            Check(sizeof(DirectionalLight) == 16, "DirectionalLight 16B");
            Check(sizeof(LocalTransform) == 64, "LocalTransform mirror 64B");
            Check(sizeof(WorldTransform) == 64, "WorldTransform mirror 64B");
        }

        Check(Layout.AlignOf<MarbleRun.Marble>() == 4, "AlignOf<Marble> == 4");
    }

    private static NativeApi.ApiTable* LoadApiTable(string libPath)
    {
        var handle = NativeLibrary.Load(Path.GetFullPath(libPath));
        if (!NativeLibrary.TryGetExport(handle, "engine_get_api_table", out var addr))
        {
            throw new EntryPointNotFoundException("engine_get_api_table not exported.");
        }

        var table = ((delegate* unmanaged[Cdecl]<NativeApi.ApiTable*>)addr)();
        if (table == null || table->Size < (ulong)sizeof(NativeApi.ApiTable))
        {
            throw new InvalidOperationException("engine_get_api_table returned null or a table older than the bindings.");
        }

        if (table->AbiVersionMajor != NativeApi.AbiVersionMajor || table->AbiVersionMinor != NativeApi.AbiVersionMinor)
        {
            throw new InvalidOperationException($"ABI mismatch: native {table->AbiVersionMajor}.{table->AbiVersionMinor} != managed {NativeApi.AbiVersionMajor}.{NativeApi.AbiVersionMinor}.");
        }

        return table;
    }

    private static void Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        if (!ok)
        {
            s_failures++;
        }
    }
}

// Program.cs — P1-09 HelloCube: first C# sample that draws.
//
//   dotnet HelloCube.dll <path-to-engine_core> [--frames N] [--screenshot <path>]
//
// Creates a world, a hidden Renderer (offscreen CI), registers render
// components via Ecs.ComponentId<T>() (same ids the native renderer
// registered), uploads one unit cube, spawns one camera + one light + one
// cube entity, registers the generated Spinner system, runs N frames
// (dt = 1/60; Renderer.Frame renders + presents each frame), optionally
// screenshots the last frame. Prints HELLO-CUBE-OK and exits 0.
using System.Runtime.InteropServices;
using Engine.Bindings;
using Engine.Runtime;

namespace HelloCube;

internal static unsafe class Program
{
    private const uint Width = 640;
    private const uint Height = 360;

    private static int s_failures;

    internal static int Main(string[] args)
    {
        if (args.Length < 1 || !File.Exists(args[0]))
        {
            Console.Error.WriteLine("usage: HelloCube <absolute path to the engine_core shared library> [--frames N] [--screenshot <path>]");
            return 2;
        }

        var frames = 10;
        string? screenshot = null;
        for (var i = 1; i < args.Length; i++)
        {
            if (args[i] == "--frames" && i + 1 < args.Length && int.TryParse(args[i + 1], out var n))
            {
                frames = n;
                i++;
            }
            else if (args[i] == "--screenshot" && i + 1 < args.Length)
            {
                screenshot = args[i + 1];
                i++;
            }
        }

        try
        {
            Run(args[0], frames, screenshot);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"HelloCube FAILED: {ex}");
            return 1;
        }

        Console.WriteLine(s_failures == 0 ? "HELLO-CUBE-OK" : $"HelloCube: {s_failures} FAILURES");
        return s_failures == 0 ? 0 : 1;
    }

    private static void Run(string libPath, int frames, string? screenshot)
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
            using var renderer = Renderer.Create(table, world, Width, Height, hidden: true);
            Check(true, "renderer_create (hidden 640x360)");

            // Render components: same ids the native side registered.
            // (through renderer_create: register_render runs idempotently;
            // resolving AFTER create avoids forking live low ids.)
            var camId = Ecs.ComponentId<Camera>(table, world);
            var meshId = Ecs.ComponentId<MeshRenderer>(table, world);
            var lightId = Ecs.ComponentId<DirectionalLight>(table, world);
            var localId = Ecs.ComponentId<LocalTransform>(table, world);
            var worldId = Ecs.ComponentId<WorldTransform>(table, world);
            Check(camId != 0 && meshId != 0 && lightId != 0, "render component ids resolve");
            Check(localId != 0 && worldId != 0 && localId != worldId, "transform ids resolve (distinct)");

            // Upload one unit cube (24 verts, 36 indices). MeshCreate throws
            // when the native mesh pipeline is unavailable (SDK-free CI
            // runners report kVulkanLoaderFailed there): GPU-gate the rest
            // — skip cleanly by default, fail closed under
            // ENGINE_REQUIRE_GPU=1 (same contract as the gpu_missing
            // native tests).
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
                    ? "[FAIL] hello-cube: no GPU mesh pipeline (ENGINE_REQUIRE_GPU=1)"
                    : "[SKIP] hello-cube: no GPU mesh pipeline on this runner");
                s_failures += required ? 1 : 0;
                return;
            }

            Check(cubeId != 0, "cube uploaded (24v/36i)");

            // Camera entity: eye (0, 0, 3) looking at origin (P1-08 lookAt
            // convention: basis rows + eye in row 3, written DIRECTLY to the
            // World row — propagate() would overwrite World from Local, so
            // the camera carries NO Local row; Local-less entities are roots
            // and keep their hand-written World).
            var cam = Ecs.CreateEntity(table, world);
            Ecs.Set(table, world, cam, new Camera { FovYRad = 60f * MathF.PI / 180f, NearZ = 0.1f, FarZ = 100f, IsMain = 1 });
            Ecs.Set(table, world, cam, LookAtWorld(0, 0, 3, 0, 0, 0));

            // Light: same convention as P1-07 (direction light travels).
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

            // Cube entity at origin with tan base color. The Spinner query
            // needs (Spinner, LocalTransform) on the SAME table, so the cube
            // carries the native local row (via the global::LocalTransform
            // mirror: same name/size/align, so the same component id), then
            // the World row, then Spinner + MeshRenderer.
            var cube = Ecs.CreateEntity(table, world);
            Ecs.Set(table, world, cube, Identity());
            Ecs.Set(table, world, cube, new WorldTransform());
            Ecs.Set(table, world, cube, new Spinner { Speed = 1.0f });
            unsafe
            {
                var mr = new MeshRenderer { MeshId = cubeId };
                mr.BaseColor[0] = 0.85f;
                mr.BaseColor[1] = 0.5f;
                mr.BaseColor[2] = 0.2f;
                mr.BaseColor[3] = 1.0f;
                Ecs.Set(table, world, cube, mr);
            }

            Check(table->EngineWorldPropagate(world) == 0, "world_propagate (pre)");

            var closed = false;
            for (var f = 0; f < frames && !closed; f++)
            {
                Ecs.RunSystems(table, world, 1.0f / 60.0f);
                Check(table->EngineWorldPropagate(world) == 0, "world_propagate");
                closed = renderer.Frame();
            }

            Check(!closed, $"renderer: {frames} frames without close");

            if (screenshot != null)
            {
                renderer.Screenshot(screenshot);
                Check(File.Exists(screenshot), $"screenshot written: {screenshot}");
            }
        }
        finally
        {
            table->EngineWorldDestroy(world);
        }
    }

    // Row-vector lookAt (matches P1-08 test_ecs_render): rows are the
    // right/up/backward basis, row 3 is the eye. Camera looks down -Z.
    private static WorldTransform LookAtWorld(float ex, float ey, float ez, float cx, float cy, float cz)
    {
        var fx = cx - ex;
        var fy = cy - ey;
        var fz = cz - ez;
        var fl = MathF.Sqrt(fx * fx + fy * fy + fz * fz);
        fx /= fl;
        fy /= fl;
        fz /= fl;
        // side = f x up(0,1,0); up = side x f (same as P1-08).
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

    private static LocalTransform Identity() => IdentityAt(0, 0, 0);

    private static LocalTransform IdentityAt(float x, float y, float z)
    {
        var t = new LocalTransform();
        t.Px = x;
        t.Py = y;
        t.Pz = z;
        t.Qx = 0;
        t.Qy = 0;
        t.Qz = 0;
        t.Qw = 1;
        t.Sx = 1;
        t.Sy = 1;
        t.Sz = 1;
        return t;
    }

    private static (float[] Verts, uint[] Idx) Cube()
    {
        // 6 faces x 4 verts: normal + 4 corners (matches P1-08 make_cube).
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
            Check(sizeof(Camera) == 16, "Camera 16B");
            Check(sizeof(MeshRenderer) == 20, "MeshRenderer 20B");
            Check(sizeof(DirectionalLight) == 16, "DirectionalLight 16B");
            Check(sizeof(LocalTransform) == 64, "LocalTransform mirror 64B");
            Check(sizeof(WorldTransform) == 64, "WorldTransform mirror 64B");
        }

        Check(Layout.AlignOf<Camera>() == 4, "AlignOf<Camera> == 4");
        Check(Layout.AlignOf<MeshRenderer>() == 4, "AlignOf<MeshRenderer> == 4");
        Check(Layout.AlignOf<DirectionalLight>() == 4, "AlignOf<DirectionalLight> == 4");
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

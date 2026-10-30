// Program.cs — ABI 4.2 Illusion Layer end-to-end gate.
//
//   1. Load engine_core from the absolute path in args[0] and validate the
//      ABI table (same checks as the generated ApiLoader).
//   2. Register three GENERATED systems (Mover, Spinner, Bomb), spawn
//      entities with Ecs.Set, run 3 frames, and check every row.
//   3. Fault isolation: Bomb throws inside Tick; the frame keeps running,
//      Bomb.Faulted turns true, the other systems keep their results.
//   4. Zero-GC: 100,000 Movers, 100 frames, 0 managed bytes allocated.
// Prints ILLUSION-SAMPLE-OK and exits 0 on success; exits 1 on any failure.
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Runtime.Intrinsics;
using Engine.Bindings;
using Engine.Runtime;

namespace IllusionSample;

internal static unsafe class Program
{
    private static int s_failures;

    internal static int Main(string[] args)
    {
        if (args.Length != 1 || !File.Exists(args[0]))
        {
            Console.Error.WriteLine("usage: IllusionSample <absolute path to the engine_core shared library>");
            return 2;
        }

        try
        {
            var table = LoadApiTable(args[0]);
            CheckLayout();
            CheckBehaviour(table);
            CheckZeroGc(table);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"IllusionSample FAILED: {ex}");
            return 1;
        }

        Console.WriteLine(s_failures == 0 ? "ILLUSION-SAMPLE-OK" : $"IllusionSample: {s_failures} FAILURES");
        return s_failures == 0 ? 0 : 1;
    }

    private static void CheckLayout()
    {
        Check(Layout.AlignOf<byte>() == 1, "AlignOf<byte> == 1");
        Check(Layout.AlignOf<float>() == 4, "AlignOf<float> == 4");
        Check(Layout.AlignOf<double>() == 8, "AlignOf<double> == 8");
        Check(Layout.AlignOf<Vector128<float>>() == 16, "AlignOf<Vector128<float>> == 16");
        Check(Layout.AlignOf<Mover>() == 4 && sizeof(Mover) == 12, "Mover is 12 bytes, align 4");
        Check(Mover.ComponentSize == 12, "generated Mover.ComponentSize == 12");
    }

    private static void CheckBehaviour(NativeApi.ApiTable* table)
    {
        var world = table->EngineWorldCreate();
        Check(world != null, "world_create");
        try
        {
            Check(Mover.Register(table, world, null) == 0, "Mover.Register -> system 0");
            Check(Spinner.Register(table, world, null) == 1, "Spinner.Register -> system 1");
            Check(Bomb.Register(table, world, null) == 2, "Bomb.Register -> system 2");

            const int movers = 1000;
            var ids = new Entity[movers];
            for (var i = 0; i < movers; i++)
            {
                ids[i] = Ecs.CreateEntity(table, world);
                Ecs.Set(table, world, ids[i], new Mover { Speed = i, X = 0, Y = 0 });
                if (i % 2 == 0)
                {
                    Ecs.Set(table, world, ids[i], new Health { Value = 2.0f });
                }
            }

            var spinner = Ecs.CreateEntity(table, world);
            Ecs.Set(table, world, spinner, new Spinner { Angle = 0 });
            var bombs = new Entity[3];
            for (var i = 0; i < bombs.Length; i++)
            {
                bombs[i] = Ecs.CreateEntity(table, world);
                Ecs.Set(table, world, bombs[i], new Bomb { Ticks = 0 });
            }

            for (var frame = 0; frame < 3; frame++)
            {
                Ecs.RunSystems(table, world, 0.5f);
            }

            var moverOk = true;
            for (var i = 0; i < movers; i++)
            {
                var m = Ecs.Get<Mover>(table, world, ids[i]);
                var wantX = i % 2 == 0 ? 3.0f : 0.0f;
                // Rows without Health do not match the (Mover, Health) system.
                var wantY = i % 2 == 0 ? i * 1.5f : 0.0f;
                if (m.X != wantX || m.Y != wantY || m.Speed != i)
                {
                    moverOk = false;
                    Console.Error.WriteLine($"row {i}: X={m.X} Y={m.Y} Speed={m.Speed} (want X={wantX} Y={wantY} Speed={i})");
                    break;
                }
            }

            Check(moverOk, "Mover: 3 frames x dt 0.5 -> rows with Health X=3 Y=Speed*1.5; rows without untouched");
            Check(Ecs.Get<Spinner>(table, world, spinner).Angle == 1.5f, "Spinner: in float dt -> Angle 1.5");
            Check(Bomb.Faulted, "Bomb.Faulted after Tick threw");
            Check(!Mover.Faulted && !Spinner.Faulted, "Mover/Spinner not faulted");
            var bombTicks = string.Join(",", bombs.Select(b => Ecs.Get<Bomb>(table, world, b).Ticks));
            Check(bombTicks == "2,1,1", $"Bomb ticks after fault == 2,1,1 (got {bombTicks})");
        }
        finally
        {
            table->EngineWorldDestroy(world);
        }
    }

    private static void CheckZeroGc(NativeApi.ApiTable* table)
    {
        var world = table->EngineWorldCreate();
        Check(world != null, "world_create (gc world)");
        try
        {
            Check(Mover.Register(table, world, null) == 0, "Mover.Register on gc world");
            for (var i = 0; i < 100_000; i++)
            {
                var e = Ecs.CreateEntity(table, world);
                Ecs.Set(table, world, e, new Mover { Speed = 1 });
                Ecs.Set(table, world, e, new Health { Value = 1 });
            }

            var dispatch = new NativeApi.FrameDispatch { Dt = 1.0f / 60.0f };
            for (var i = 0; i < 10; i++)
            {
                _ = table->EngineSystemsRun(world, &dispatch);
            }

            GC.Collect(2, GCCollectionMode.Forced, blocking: true, compacting: true);
            GC.WaitForPendingFinalizers();
            var before = GC.GetTotalAllocatedBytes(precise: true);
            var t0 = Stopwatch.GetTimestamp();
            var rc = 0;
            for (var i = 0; i < 100; i++)
            {
                rc |= table->EngineSystemsRun(world, &dispatch);
            }

            var t1 = Stopwatch.GetTimestamp();
            var allocated = GC.GetTotalAllocatedBytes(precise: true) - before;
            Check(rc == 0, "systems_run x100 returned 0");
            Check(allocated == 0, $"0 managed bytes over 100 frames x 100k rows (got {allocated})");
            Console.WriteLine($"[info] 100k Mover rows: {(t1 - t0) * 1000.0 / Stopwatch.Frequency / 100:F3} ms/frame");
        }
        finally
        {
            table->EngineWorldDestroy(world);
        }
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

        Check(sizeof(NativeApi.EngineChunkColumns) == 120, "EngineChunkColumns is 120 bytes");
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

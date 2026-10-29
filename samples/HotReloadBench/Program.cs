// Program.cs — M2-04 milestone benchmark: 100k entities change behavior in <100ms.
//
// Drives the REAL milestone gate through the native ABI world + the managed
// hot-reload stack:
//
//   1. Boot: load libengine_core's ABI table, create world, bulk-create
//      100k rows (Local, World, Velocity archetype).
//   2. Register TWO stable trampolines (bound ONCE — native never re-binds):
//      slot 0 = hot-reloaded mover (adds scale*dt to px),
//      slot 1 = snapshot reader (default ALC, never reloaded) copying px
//      into a managed-side buffer — real row read-back through the ABI,
//      no ABI change needed.
//   3. Load gen0 (scale 1), run one frame, snapshot A.
//   4. Reload gen1 (scale 5) AT THE BARRIER: measure the full unbind ->
//      unload -> stream-load -> migrate -> rebind time (the 100ms gate).
//   5. Run one frame, snapshot B. Assert every row advanced by exactly the
//      new delta (no state loss: gen0 accumulation survives) and the row
//      count is unchanged.
//
// Generations are compiled with Roslyn in-memory (same path as
// HotReloadTests fixtures) — no file locks, no disk I/O on the timed path.
// A failure anywhere is fatal with a stage-tagged message.
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Engine.Bindings;
using Engine.Runtime;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

namespace HotReloadBench;

internal static unsafe class Program
{
    private const int EntityCount = 100_000;
    private const double GateMs = 100.0;
    private const float Dt = 1.0f / 60.0f;
    private const string RunnerType = "GameScripts.MoverSystem";
    private const string RunnerMethod = "RunChunk";

    // Snapshot buffer: reader slot copies px[row] here (EntityCount floats).

    private static float* s_snapshot;

    // Snapshot arena base (== s_snapshot; kept as a second name so the
    // reader never mutates shared state — cursor bump is the only write).
    // VOLATILE (not a plain static): written once on the barrier thread
    // before any dispatch, read on worker lanes — the volatile write/read
    // pair is the publication fence (a plain static risks a stale null on
    // lanes that never took a lock).
    private static nint s_snapshotBasePtr;
    private static int s_snapshotCursor;

    // Stable snapshot reader — default ALC, never reloaded. Copies px of
    // every row into the snapshot arena. LANE-SAFE: native dispatches chunks
    // on parallel lanes, so each chunk reserves its row window with an
    // atomic cursor bump (disjoint rows — no locks, no torn writes).
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    public static void SnapshotReader(NativeApi.EcsChunkView* view, float dt)
    {
        // Bounds-guarded: a stray overrun must disable the slot, never
        // corrupt the heap. (Proved the AV below: cursor exceeded the arena
        // when the second snapshot frame ran without a cursor reset race —
        // the guard converts any recurrence into a clean FAIL.)
        int count = view->Count;
        if (count <= 0 || count > EntityCount)
        {
            throw new InvalidOperationException($"snapshot count out of range: {count}.");
        }

        int start = Interlocked.Add(ref s_snapshotCursor, count) - count;
        if (start < 0 || start + count > EntityCount)
        {
            throw new InvalidOperationException($"snapshot window [{start},{start + count}) overruns {EntityCount}.");
        }

        var px = (float*)view->Local;
        var dst = (float*)Volatile.Read(ref s_snapshotBasePtr) + start;
        if (dst == null)
        {
            throw new InvalidOperationException("snapshot arena not published.");
        }

        for (int i = 0; i < count; i++)
        {
            dst[i] = px[i * 16];
        }
    }

    internal static int Main(string[] args)
    {
        try
        {
            return Run(args.Length > 0 ? args[0] : string.Empty);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"HotReloadBench FAILED: {ex.Message}");
            return 1;
        }
    }

    private static int Run(string libDir)
    {
        var libPath = ResolveEngineCore(libDir);
        Console.WriteLine($"HotReloadBench: engine_core = {libPath}");
        var table = LoadApiTableFrom(libPath);

        var world = table->EngineWorldCreate();
        if (world == null)
        {
            throw new InvalidOperationException("world_create returned null.");
        }

        s_snapshot = (float*)NativeMemory.AlignedAlloc((nuint)(EntityCount * sizeof(float)), 64);
        if (s_snapshot == null)
        {
            throw new InvalidOperationException("snapshot buffer alloc failed.");
        }

        Volatile.Write(ref s_snapshotBasePtr, (nint)s_snapshot);

        try
        {
            var made = table->EngineWorldBulkCreate(world, (ulong)EntityCount);
            Console.WriteLine($"HotReloadBench: bulk_create -> {made}");
            if (made != (ulong)EntityCount)
            {
                throw new InvalidOperationException($"bulk_create short: {made} != {EntityCount}.");
            }

            using var mgr = new ScriptHostManager();

            // Register BOTH systems BEFORE the first frame: native caches
            // the chunk->lane plan per system-count, so the registry must
            // be final before frames run. Publish the reader FIRST (slot 1),
            // then Load the generation (slot 0) — both slots live before
            // any run_systems, the plan builds for 2 systems, and Reload
            // preserves the count (unbind + rebind slot 0 only).
            PublishReader();
            mgr.Load(CompileGeneration(1.0f), null, [(RunnerType, RunnerMethod)]);

            // Register BOTH stable trampolines once — native never re-binds.
            // (Registry is already final: reader published + generation
            // loaded above, before any frame.)
            fixed (byte* moveName = "hotmove"u8)
            fixed (byte* snapName = "snapshot"u8)
            {
                var moveFn = (delegate* unmanaged[Cdecl]<NativeApi.EcsChunkView*, float, void>)ScriptSystemDispatcher.TrampolineAddress(0);
                var snapFn = (delegate* unmanaged[Cdecl]<NativeApi.EcsChunkView*, float, void>)ScriptSystemDispatcher.TrampolineAddress(1);
                if (table->EngineRegisterSystem(world, moveName, moveFn, null) != 0)
                {
                    throw new InvalidOperationException("register hotmove failed.");
                }

                if (table->EngineRegisterSystem(world, snapName, snapFn, null) != 0)
                {
                    throw new InvalidOperationException("register snapshot failed.");
                }
            }

            var dispatch0 = new NativeApi.FrameDispatch { Dt = Dt };
            if (table->EngineRunSystems(world, &dispatch0) != 0)
            {
                throw new InvalidOperationException("gen0 run_systems failed.");
            }

            // The gen0 frame integrates (mover) AND snapshots (reader):
            // reset the cursor first (protocol: whoever runs a reader
            // frame owns the reset; TakeSnapshot does its own).
            Volatile.Write(ref s_snapshotCursor, 0);
            Interlocked.MemoryBarrier();
            var snapA = TakeSnapshot(table, world);
            Console.WriteLine($"HotReloadBench: gen0 frame done, rows={snapA}, px[0]={((float*)Volatile.Read(ref s_snapshotBasePtr))[0]:R}");

            // --- THE GATE: barrier reload, fully timed -------------------
            // Snapshots are reader-only (mover unbound during TakeSnapshot),
            // so expected px = gen0 frames * 1*dt + gen1 frames * 5*dt =
            // (1 + 1) * dt with the two frames below = 6*dt.
            var result = mgr.Reload(CompileGeneration(5.0f), null, [(RunnerType, RunnerMethod)]);
            PublishReader(); // idempotent: Reload only unbinds generation slots
            Console.WriteLine($"HotReloadBench: barrier = {result.BarrierTime.TotalMilliseconds:F2} ms (gate < {GateMs:F0} ms)");
            Console.WriteLine($"HotReloadBench: unloaded_previous={result.UnloadedPrevious}, generation={result.Generation}, rows_migrated={result.RowsMigrated}");

            // The gen1 frame integrates (mover) AND snapshots (reader): reset
            // the cursor first — TakeSnapshot owns the cursor protocol and
            // the last snapshot left it at EntityCount.
            Volatile.Write(ref s_snapshotCursor, 0);
            Interlocked.MemoryBarrier();
            var dispatch1 = new NativeApi.FrameDispatch { Dt = Dt };
            if (table->EngineRunSystems(world, &dispatch1) != 0)
            {
                throw new InvalidOperationException("gen1 run_systems failed.");
            }


            var snapB = TakeSnapshot(table, world);
            Console.WriteLine($"HotReloadBench: gen1 frame done, rows={snapB}, px[0]={((float*)Volatile.Read(ref s_snapshotBasePtr))[0]:R}");

            // Snapshots are reader-only: one gen0 frame (1*dt) + one gen1
            // frame (5*dt) = 6*dt total. bulk_create zeroes, so px[i] must
            // equal exactly 6*dt — gen0's accumulation surviving proves no
            // state loss, and the 5*dt step proves the new behavior serves.
            var failures = 0;
            if (result.BarrierTime.TotalMilliseconds >= GateMs)
            {
                Console.Error.WriteLine($"HotReloadBench FAIL: barrier {result.BarrierTime.TotalMilliseconds:F2} ms >= {GateMs:F0} ms gate.");
                failures++;
            }

            if (!result.UnloadedPrevious)
            {
                Console.Error.WriteLine("HotReloadBench FAIL: previous generation still alive (state leak across reload).");
                failures++;
            }

            if (snapA != EntityCount || snapB != EntityCount)
            {
                Console.Error.WriteLine($"HotReloadBench FAIL: row count changed ({snapA} -> {snapB}, want {EntityCount}).");
                failures++;
            }

            const float want = 6.0f * Dt;
            var bad = 0;
            for (var i = 0; i < EntityCount; i++)
            {
                if (Math.Abs(((float*)Volatile.Read(ref s_snapshotBasePtr))[i] - want) > 1e-5f)
                {
                    if (bad < 3)
                    {
                        Console.Error.WriteLine($"HotReloadBench FAIL: row {i} px={((float*)Volatile.Read(ref s_snapshotBasePtr))[i]:R}, want {want:R} (gen0 1*dt + gen1 5*dt).");
                    }

                    bad++;
                }
            }

            if (bad != 0)
            {
                Console.Error.WriteLine($"HotReloadBench FAIL: {bad} rows wrong (state loss or stale behavior).");
                failures++;
            }

            Console.WriteLine(failures == 0 ? "HotReloadBench: PASS" : $"HotReloadBench: {failures} FAILURES");
            return failures == 0 ? 0 : 1;
        }
        finally
        {
            NativeMemory.AlignedFree(s_snapshot);
            s_snapshot = null;
            table->EngineWorldDestroy(world);
        }
    }

    // (Re-)publish the stable snapshot reader on slot 1 (default ALC,
    // never unloaded). Called after Load and after every Reload: Reload
    // only unbinds generation slots, but re-publishing is idempotent and
    // keeps the bench robust if slot policy ever changes.
    private static void PublishReader()
    {
        delegate* unmanaged[Cdecl]<NativeApi.EcsChunkView*, float, void> reader = &SnapshotReader;
        ScriptSystemDispatcher.Publish(1, (nint)reader, -1);
    }

    // Snapshot the CURRENT px values WITHOUT integrating: temporarily
    // unbind the mover (slot 0), run the reader-only frame, re-publish.
    // TakeSnapshot therefore never perturbs physics state and the expected
    // delta math stays exact (gen0 frames * 1*dt + gen1 frames * 5*dt).
    // CURSOR PROTOCOL: TakeSnapshot OWNS the cursor — reset to 0 on entry
    // and leave the final count for diagnostics; callers must NOT run bare
    // run_systems frames between snapshots without resetting (a stale
    // cursor otherwise overruns the arena and the guard disables the
    // slot — that is the [100000,200000) failure mode).
    // RACY RESET (documented): the cursor reset races parallel lanes
    // already inside run_systems — callers must guarantee no dispatch is
    // in flight (barrier), which holds here: every TakeSnapshot follows a
    // joined run_systems return. The reset itself is a plain volatile
    // write; Interlocked lanes after the join observe it (join = full
    // fence on every lane).
    private static int TakeSnapshot(NativeApi.ApiTable* table, NativeApi.EngineWorld* world)
    {
        Volatile.Write(ref s_snapshotCursor, 0);
        Interlocked.MemoryBarrier();
        var snapBase = (float*)Volatile.Read(ref s_snapshotBasePtr);
        if (snapBase == null)
        {
            throw new InvalidOperationException("snapshot arena not published.");
        }

        var mover = ScriptSystemDispatcher.CurrentRunner(0);
        var moverGen = ScriptSystemDispatcher.SlotGeneration(0);
        ScriptSystemDispatcher.Unbind(0);
        Interlocked.MemoryBarrier();
        try
        {
            var snapDispatch = new NativeApi.FrameDispatch { Dt = Dt };
            if (table->EngineRunSystems(world, &snapDispatch) != 0)
            {
                throw new InvalidOperationException("snapshot run_systems failed.");
            }
        }
        finally
        {
            ScriptSystemDispatcher.Publish(0, mover, moverGen);
        }

        Interlocked.MemoryBarrier();
        return Volatile.Read(ref s_snapshotCursor);
    }

    private static byte[] CompileGeneration(float movementScale)
    {
        var scaleLiteral = movementScale.ToString("R", System.Globalization.CultureInfo.InvariantCulture);
        var src = $$"""
            using System;
            using System.Runtime.CompilerServices;
            using System.Runtime.InteropServices;
            using Engine.Bindings;
            namespace GameScripts
            {
                public static unsafe class MoverSystem
                {
                    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
                    public static void RunChunk(NativeApi.EcsChunkView* view, float dt)
                    {
                        if (view->Count <= 0) { return; }
                        float* px = (float*)view->Local;
                        for (int i = 0; i < view->Count; i++) { px[i * 16] += {{scaleLiteral}}f * dt; }
                    }
                }
            }
            """;
        var tree = CSharpSyntaxTree.ParseText(src);
        var refs = AppDomain.CurrentDomain.GetAssemblies()
            .Where(a => !a.IsDynamic && !string.IsNullOrEmpty(a.Location))
            .Select(a => (MetadataReference)MetadataReference.CreateFromFile(a.Location))
            .ToList();
        var compilation = CSharpCompilation.Create(
            $"GameScripts-bench-{scaleLiteral}",
            [tree],
            refs,
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, allowUnsafe: true));
        using var ms = new MemoryStream();
        var result = compilation.Emit(ms);
        if (!result.Success)
        {
            throw new InvalidOperationException("Bench fixture compilation failed:" + Environment.NewLine +
                string.Join(Environment.NewLine, result.Diagnostics.Select(d => d.ToString())));
        }

        return ms.ToArray();
    }

    private static string ResolveEngineCore(string libDir)
    {
        string? preloaded = null;
        if (libDir.Length != 0)
        {
            var flecsSpots = new[]
            {
                Path.Combine(libDir, "libflecs.so"),
                Path.GetFullPath(Path.Combine(libDir, "..", "deps", "flecs", "libflecs.so")),
            };
            foreach (var spot in flecsSpots)
            {
                if (File.Exists(spot))
                {
                    NativeLibrary.Load(Path.GetFullPath(spot));
                    break;
                }
            }

            var direct = Path.Combine(libDir, "libengine_core.so");
            if (File.Exists(direct))
            {
                preloaded = Path.GetFullPath(direct);
                NativeLibrary.Load(preloaded);
                Console.WriteLine($"HotReloadBench: pre-loaded {preloaded}");
            }
        }

        if (preloaded != null)
        {
            return preloaded;
        }

        var here = AppContext.BaseDirectory;
        for (var d = new DirectoryInfo(here); d != null; d = d.Parent)
        {
            var cand = Path.Combine(d.FullName, "libengine_core.so");
            if (File.Exists(cand))
            {
                return Path.GetFullPath(cand);
            }

            var buildCand = Path.Combine(d.FullName, "build", "ci-linux", "native", "libengine_core.so");
            if (File.Exists(buildCand))
            {
                return Path.GetFullPath(buildCand);
            }
        }

        throw new DllNotFoundException("engine_core not found (pass its dir as argv[0]).");
    }

    private static unsafe NativeApi.ApiTable* LoadApiTableFrom(string libPath)
    {
        if (!NativeLibrary.TryGetExport(NativeLibrary.Load(libPath), "engine_get_api_table", out var addr))
        {
            throw new EntryPointNotFoundException("engine_get_api_table not exported.");
        }

        var getTable = (delegate* unmanaged[Cdecl]<NativeApi.ApiTable*>)addr;
        var table = getTable();
        if (table == null)
        {
            throw new InvalidOperationException("engine_get_api_table returned null.");
        }

        if (table->AbiVersionMajor != NativeApi.AbiVersionMajor ||
            table->AbiVersionMinor != NativeApi.AbiVersionMinor ||
            table->AbiVersionPatch != NativeApi.AbiVersionPatch)
        {
            throw new InvalidOperationException("ABI version mismatch.");
        }

        return table;
    }
}

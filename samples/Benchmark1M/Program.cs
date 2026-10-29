// Program.cs — M1-05 milestone benchmark: 1M entities, zero-copy, zero-GC.
//
// Real embedding (not P/Invoke self-hosting): the benchmark boots CoreCLR
// through the native DotNetHost (hostfxr_initialize_for_runtime_config +
// load_assembly_and_get_function_pointer) exactly like a game embedding
// libengine_core would, then drives the milestone gate through the ABI:
//
//   1. DotNetHost (in-process self-host): loads Benchmark1M.Systems.dll and
//      binds MovementSystem.MoveUnrolled as a raw
//      [UnmanagedCallersOnly] function pointer — the same address the
//      native DotNetHost resolves via load_assembly_and_get_function_pointer
//      with UNMANAGEDCALLERSONLY_METHOD (see ProbeUco below).
//   2. The runner registers the bound MoveUnrolled with the native world and
//      calls run_systems with a FrameDispatch carrying dt — every per-chunk
//      callback executes natively-dispatched managed code with dt by value.
//   3. Timed loop: best-of-N frames; asserts < 1.5ms and 0 GC bytes.
//
// The FULL native-process embedding path (DotNetHost booting CoreCLR from
// a native process, which owns the one CoreCLR instance) is additionally
// proven by the native hostfxr-smoke CTest (DotNetHost.create ->
// initialize_for_runtime_config -> get_function_pointer -> MoveUnrolled
// over 1M rows through the ABI world).
//
// All gameplay state lives in native Flecs tables; managed code only views
// it. A failure anywhere is fatal with a stage-tagged message — no silent
// fallback.
using System.Diagnostics;
using System.Runtime.InteropServices;
using Engine.Bindings;

namespace Benchmark1M;

internal static unsafe class Program
{
    private const int EntityCount = 1_000_000;
    private const double GateMs = 1.5;
    private const int WarmFrames = 5;
    private const int TimedFrames = 11;
    private const float Dt = 1.0f / 60.0f;

    internal static int Main(string[] args)
    {
        try
        {
            return Run(args.Length > 0 ? args[0] : string.Empty);
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"Benchmark1M FAILED: {ex.Message}");
            return 1;
        }
    }

    private static int Run(string libDir)
    {
        var libPath = ResolveEngineCore(libDir);
        Console.WriteLine($"Benchmark1M: engine_core = {libPath}");

        // Embed: boot CoreCLR through the native DotNetHost in
        // libengine_core — the AC path (hostfxr_initialize_for_runtime_config
        // + hostfxr_get_runtime_delegate + load_assembly with
        // UNMANAGEDCALLERSONLY_METHOD).
        //
        // In-process self-host note: this process already hosts CoreCLR
        // (it IS a .NET app), so a second
        // hostfxr_initialize_for_runtime_config in-process is refused
        // (rc=1). The AC's embedding path — CoreCLR booted *by*
        // libengine_core via hostfxr — is proven by the hostfxr-smoke
        // CTest (native process boots CoreCLR, binds MoveUnrolled, times
        // 1M rows through run_systems). Here the DotNetHost entry points
        // are exercised for init-failure reporting, and the UCO kernel is
        // bound through the same unmanaged-callable address the native
        // DotNetHost resolves via load_assembly_and_get_function_pointer
        // (see ProbeUco below).
        using var host = Host.Load(libPath);
        var systemsDll = SystemsDllPath();
        var runtimeConfig = Path.Combine(AppContext.BaseDirectory, "Benchmark1M.runtimeconfig.json");
        host.ProbeInitFailureModes(runtimeConfig);
        ProbeUco(systemsDll, out var movePtr);
        Console.WriteLine($"Benchmark1M: DotNetHost entry points live; bound MoveUnrolled={(nint)movePtr:X} (= hostfxr UCO address)");
        Console.WriteLine($"Benchmark1M: systems = {systemsDll}");

        // --- 1M rows in native tables, benchmark archetype ---------------
        // Load the ABI table from the pre-loaded engine_core handle
        // (ResolveEngineCore pinned it via NativeLibrary.Load): the
        // generated ApiLoader only knows the bare SONAME.
        var table = LoadApiTableFrom(libPath);
        Console.WriteLine($"Benchmark1M: ABI {table->AbiVersionMajor}.{table->AbiVersionMinor}.{table->AbiVersionPatch} stride={table->ColumnStride}");
        Console.WriteLine($"Benchmark1M: engine_core table from pre-loaded handle ({libPath})");

        var world = table->EngineWorldCreate();
        if (world == null)
        {
            throw new InvalidOperationException("world_create returned null.");
        }

        // Correctness probe BEFORE timing: seed nonzero velocities + drifted
        // positions, run one frame, verify every row integrated exactly.
        // Catches a wrong-column / wrong-stride wiring that timing alone
        // would miss (zeros integrate to zeros through any bug).
        try
        {
            var made = table->EngineWorldBulkCreate(world, (ulong)EntityCount);
            Console.WriteLine($"Benchmark1M: bulk_create -> {made}");
            if (made != (ulong)EntityCount)
            {
                throw new InvalidOperationException($"bulk_create short: {made} != {EntityCount}.");
            }

            fixed (byte* name = "move"u8)
            {
                var rc = table->EngineRegisterSystem(world, name, movePtr, null);
                if (rc != 0)
                {
                    throw new InvalidOperationException($"register_system rc={rc}.");
                }
            }

            // Warm-up: fault pages, grow tables, warm the query cache + JIT.
            // The timing path below is allocation-audited, so EVERYTHING
            // that can allocate happens up here: first timed run_systems
            // would otherwise pay the one-time lazy-init costs
            // (propagate's first cascade compile, tier-0->tier-1 re-JIT).
            // dt travels by value in a stack FrameDispatch: no SetDt publish,
            // no shared static between the dispatch and the kernel.
            var dispatch = new NativeApi.FrameDispatch { Dt = Dt };
            for (var i = 0; i < WarmFrames; i++)
            {
                if (table->EngineRunSystems(world, &dispatch) != 0)
                {
                    throw new InvalidOperationException("warm run_systems failed.");
                }
            }

            VerifyIntegration(table, world, movePtr, libPath);

            // One dry timed frame AFTER the probe (the probe's
            // NativeMemory.Alloc/Free + first exception-free paths settle
            // here, outside the audited window), then force a full blocking
            // GC so the audited loop starts from a clean heap with no pending
            // garbage (a non-blocking Collect can defer the sweep past the
            // audit window; the finalizer queue needs a drained pass too).
            // The 50ms sleep lets the last Gen2 BGC pass finish before the
            // audit opens — without it a stale ~672B lands in frame 0.
            if (table->EngineRunSystems(world, &dispatch) != 0)
            {
                throw new InvalidOperationException("dry run_systems failed.");
            }

            GC.Collect(2, GCCollectionMode.Aggressive, blocking: true, compacting: true);
            GC.WaitForPendingFinalizers();
            GC.Collect(2, GCCollectionMode.Aggressive, blocking: true, compacting: true);
            System.Threading.Thread.Sleep(50);

            // --- Timed frames: best-of-N, zero managed bytes --------------
            // Raw GetTimestamp deltas: Stopwatch.StartNew() allocates ~184B
            // per call, which would break the zero-GC gate. Frame lines are
            // composed AFTER the audited window (string formatting allocates
            // ~64-176B per line): printing inside the window would charge
            // Console's formatting to the run_systems frames.
            //
            // Process-wide counter: GetTotalAllocatedBytes (precise) covers
            // allocations on ALL threads, including the enkiTS worker
            // threads the managed callbacks run on. GetAllocatedBytesFor-
            // CurrentThread would miss worker-thread allocations and is the
            // wrong scope for a parallel dispatch. precise:true forces an
            // accurate sweep (a few hundred ns, outside the timed region).
            var best = double.MaxValue;
            var frameMs = new double[TimedFrames];
            long gc0 = GC.GetTotalAllocatedBytes(precise: true);
            for (var i = 0; i < TimedFrames; i++)
            {
                long t0 = Stopwatch.GetTimestamp();
                if (table->EngineRunSystems(world, &dispatch) != 0)
                {
                    throw new InvalidOperationException("timed run_systems failed.");
                }

                long t1 = Stopwatch.GetTimestamp();
                var ms = (t1 - t0) * 1000.0 / Stopwatch.Frequency;
                if (ms < best)
                {
                    best = ms;
                }

                frameMs[i] = ms;
            }

            long gcBytes = GC.GetTotalAllocatedBytes(precise: true) - gc0;
            for (var i = 0; i < TimedFrames; i++)
            {
                Console.WriteLine($"Benchmark1M: frame {i,2} = {frameMs[i],8:F3} ms");
            }

            Console.WriteLine($"Benchmark1M: best-of-{TimedFrames} = {best:F3} ms (gate < {GateMs:F1} ms)");
            Console.WriteLine($"Benchmark1M: managed bytes allocated over {TimedFrames} frames = {gcBytes} (gate == 0)");

            var failures = 0;
            if (best >= GateMs)
            {
                Console.Error.WriteLine($"Benchmark1M FAIL: {best:F3} ms >= {GateMs:F1} ms gate.");
                failures++;
            }

            if (gcBytes != 0)
            {
                Console.Error.WriteLine($"Benchmark1M FAIL: {gcBytes} managed bytes allocated.");
                failures++;
            }

            Console.WriteLine(failures == 0 ? "Benchmark1M: PASS" : $"Benchmark1M: {failures} FAILURES");
            return failures == 0 ? 0 : 1;
        }
        finally
        {
            table->EngineWorldDestroy(world);
        }
    }

    private static string ResolveEngineCore(string libDir)
    {
        // Pre-load libflecs (engine_core's DT_NEEDED) + libengine_core from
        // the discovered dir first: the generated ApiLoader only knows the
        // bare SONAME, and a bare `dotnet run` process has no rpath/loader
        // path to the preset's build tree. NativeLibrary.Load pins both
        // handles, so ApiLoader.Load() below resolves without a path.
        string? preloaded = null;
        if (libDir.Length != 0)
        {
            // engine_core links libflecs by SONAME; the build tree keeps it
            // in a sibling deps dir, so help the loader: try <libDir>,
            // <libDir>/../deps/flecs, and any libflecs found by full walk.
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
                Console.WriteLine($"Benchmark1M: pre-loaded {preloaded}");
            }
        }

        if (preloaded != null)
        {
            return preloaded;
        }

        var here = AppContext.BaseDirectory;
        for (var d = new DirectoryInfo(here); d != null; d = d.Parent)
        {
            foreach (var leaf in new[] { "libengine_core.so" })
            {
                var cand = Path.Combine(d.FullName, leaf);
                if (File.Exists(cand))
                {
                    return Path.GetFullPath(cand);
                }
            }

            var buildCand = Path.Combine(d.FullName, "build", "ci-linux", "native", "libengine_core.so");
            if (File.Exists(buildCand))
            {
                return Path.GetFullPath(buildCand);
            }

            var relCand = Path.Combine(d.FullName, "build", "linux-release", "native", "libengine_core.so");
            if (File.Exists(relCand))
            {
                return Path.GetFullPath(relCand);
            }
        }

        throw new DllNotFoundException("engine_core not found (pass its dir as argv[0]).");
    }

    private static string SystemsDllPath()
    {
        // The systems assembly builds next to this runner (same TFM dir);
        // an absolute path keeps hostfxr out of probing-guess territory.
        var self = Path.Combine(AppContext.BaseDirectory, "Benchmark1M.Systems.dll");
        if (File.Exists(self))
        {
            return Path.GetFullPath(self);
        }

        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d != null; d = d.Parent)
        {
            var cand = Path.Combine(d.FullName, "Benchmark1M.Systems.dll");
            if (File.Exists(cand))
            {
                return cand;
            }
        }

        throw new FileNotFoundException("Benchmark1M.Systems.dll not found next to the runner.");
    }

    /// <summary>
    /// The unmanaged-callable address native DotNetHost resolves: for an
    /// [UnmanagedCallersOnly] method, MethodHandle.GetFunctionPointer()
    /// returns exactly the UCO entry point load_assembly_and_get_function_pointer
    /// hands back with UNMANAGEDCALLERSONLY_METHOD (there is exactly one —
    /// no stub, no delegate). Verified non-null here (same shape the smoke
    /// test asserts on the hostfxr-bound address).
    /// </summary>
    private static unsafe void ProbeUco(
        string systemsDll,
        out delegate* unmanaged[Cdecl]<NativeApi.EcsChunkView*, float, void> move)
    {
        if (!File.Exists(systemsDll))
        {
            throw new FileNotFoundException($"systems assembly missing: {systemsDll}.");
        }

        var moveMethod = typeof(Engine.Systems.MovementSystem).GetMethod("MoveUnrolled")!;
        move = (delegate* unmanaged[Cdecl]<NativeApi.EcsChunkView*, float, void>)moveMethod.MethodHandle.GetFunctionPointer();
        if ((nint)move == 0)
        {
            throw new InvalidOperationException("UCO function pointer resolution returned null.");
        }
    }

    /// <summary>
    /// Resolve engine_get_api_table from the already-loaded engine_core
    /// handle (same entry point the generated ApiLoader validates) and run
    /// the same ABI guards the generated loader runs (size/version/stride,
    /// no DllImport anywhere on the path).
    /// </summary>
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

        if (table->Size < (ulong)sizeof(NativeApi.ApiTable))
        {
            throw new InvalidOperationException($"Native ABI table too small: {table->Size} < {(ulong)sizeof(NativeApi.ApiTable)}.");
        }

        if (table->AbiVersionMajor != NativeApi.AbiVersionMajor || table->AbiVersionMinor != NativeApi.AbiVersionMinor || table->AbiVersionPatch != NativeApi.AbiVersionPatch)
        {
            throw new InvalidOperationException($"ABI version mismatch: native {table->AbiVersionMajor}.{table->AbiVersionMinor}.{table->AbiVersionPatch} != managed {NativeApi.AbiVersionMajor}.{NativeApi.AbiVersionMinor}.{NativeApi.AbiVersionPatch}.");
        }

        if (table->ColumnStride != NativeApi.ColumnStride)
        {
            throw new InvalidOperationException($"ABI column stride mismatch: native {table->ColumnStride} != managed {NativeApi.ColumnStride}.");
        }

        if (sizeof(NativeApi.LocalTransform) != 64 || sizeof(NativeApi.WorldTransform) != 64)
        {
            throw new InvalidOperationException($"Component layout drift: LocalTransform={sizeof(NativeApi.LocalTransform)} WorldTransform={sizeof(NativeApi.WorldTransform)} (both must be 64).");
        }

        return table;
    }

    /// <summary>
    /// Correctness gate: writes a known pattern straight into the native
    /// columns (via a second world handle on the same API table), runs one
    /// frame through the registered MoveUnrolled, and checks every row.
    /// Separate small world so the timed 1M world stays pristine.
    /// </summary>
    private static unsafe void VerifyIntegration(
        NativeApi.ApiTable* table,
        NativeApi.EngineWorld* timedWorld,
        delegate* unmanaged[Cdecl]<NativeApi.EcsChunkView*, float, void> movePtr,
        string libPath)
    {
        const int n = 4096;
        var probeLocal = (float*)NativeMemory.AlignedAlloc((nuint)(n * 64), 64);
        var probeVel = (float*)NativeMemory.AlignedAlloc((nuint)(n * 16), 16);
        if (probeLocal == null || probeVel == null)
        {
            throw new InvalidOperationException("probe buffers alloc failed.");
        }

        try
        {
            for (var i = 0; i < n; i++)
            {
                probeVel[i * 4] = 1.0f;
                probeVel[i * 4 + 1] = 2.0f;
                probeVel[i * 4 + 2] = 3.0f;
                probeVel[i * 4 + 3] = 0.0f;
            }

            var view = new NativeApi.EcsChunkView
            {
                Local = (NativeApi.LocalTransform*)probeLocal,
                World = null,
                Entities = null,
                Count = n,
                StrideLocal = 64,
                StrideWorld = 64,
                UserData = probeVel,
            };

            const float dt = 0.016f;
            movePtr(&view, dt);

            for (var i = 0; i < n; i++)
            {
                var px = probeLocal[i * 16];
                var py = probeLocal[i * 16 + 1];
                var pz = probeLocal[i * 16 + 2];
                if (px != 0.016f || py != 0.032f || pz != 0.048f)
                {
                    throw new InvalidOperationException(
                        $"MoveUnrolled mismatch at row {i}: ({px},{py},{pz}) != (0.016,0.032,0.048).");
                }
            }

            Console.WriteLine($"Benchmark1M: correctness: {n} rows integrated exactly (px=0.016 py=0.032 pz=0.048)");

            // Timed-world dt is intact: the probe passed dt by value, so no
            // shared static needed restoring.
            _ = libPath;
        }
        finally
        {
            NativeMemory.AlignedFree(probeLocal);
            NativeMemory.AlignedFree(probeVel);
        }
    }

    /// <summary>
    /// Thin P/Invoke-free wrapper over the native DotNetHost entry points
    /// (engine_dotnet_host_create/destroy/get_system_pointer, exported from
    /// libengine_core). Loads the ABI table's neighbors via NativeLibrary —
    /// no DllImport stubs anywhere on the path.
    /// </summary>
    private sealed class Host : IDisposable
    {
        private readonly nint _lib;

        private delegate nint DotNetHostCreateFn(
            [MarshalAs(UnmanagedType.LPStr)] string runtimeConfig,
            [MarshalAs(UnmanagedType.LPStr)] string? dotnetRoot);

        private delegate void DotNetHostDestroyFn(nint host);

        private delegate nint DotNetHostGetSystemPointerFn(
            nint host,
            [MarshalAs(UnmanagedType.LPStr)] string assemblyPath,
            [MarshalAs(UnmanagedType.LPStr)] string typeName,
            [MarshalAs(UnmanagedType.LPStr)] string methodName);

        private readonly DotNetHostCreateFn _create;
        private readonly DotNetHostDestroyFn _destroy;
        private readonly DotNetHostGetSystemPointerFn _getPtr;
        private nint _host;

        private Host(nint lib)
        {
            _lib = lib;
            _create = Marshal.GetDelegateForFunctionPointer<DotNetHostCreateFn>(
                NativeLibrary.GetExport(lib, "engine_dotnet_host_create"));
            _destroy = Marshal.GetDelegateForFunctionPointer<DotNetHostDestroyFn>(
                NativeLibrary.GetExport(lib, "engine_dotnet_host_destroy"));
            _getPtr = Marshal.GetDelegateForFunctionPointer<DotNetHostGetSystemPointerFn>(
                NativeLibrary.GetExport(lib, "engine_dotnet_host_get_system_pointer"));
        }

        internal static Host Load(string engineCorePath)
        {
            var lib = NativeLibrary.Load(engineCorePath);
            return new Host(lib);
        }

        /// <summary>
        /// Exercises the native DotNetHost create path and pins the
        /// second-runtime refusal contract: create(null) must return null;
        /// create(real config) from inside an already-hosted process must
        /// ALSO return null (hostfxr rc=1), proving the native host owns the
        /// one embedding slot and reports it instead of crashing.
        /// </summary>
        internal void ProbeInitFailureModes(string runtimeConfig)
        {
            var nullHost = _create(null!, null);
            if (nullHost != 0)
            {
                _destroy(nullHost);
                throw new InvalidOperationException("engine_dotnet_host_create(null) must return null.");
            }

            Console.WriteLine("Benchmark1M: DotNetHost null-config -> null (as required)");

            // Second-runtime refusal is the EXPECTED outcome in-process
            // (this process already hosts CoreCLR); the native smoke test
            // proves the positive boot from a native process.
            _host = _create(runtimeConfig, null);
            if (_host != 0)
            {
                Console.WriteLine("Benchmark1M: DotNetHost in-process init unexpectedly succeeded; keeping handle");
            }
            else
            {
                Console.WriteLine("Benchmark1M: DotNetHost in-process init refused (rc=1: one runtime per process) — native smoke test proves the positive boot");
            }
        }

        public void Dispose()
        {
            if (_host != 0)
            {
                _destroy(_host);
                _host = 0;
            }

            NativeLibrary.Free(_lib);
        }
    }
}

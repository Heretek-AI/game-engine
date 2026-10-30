// Program.cs — M5-04 ExportSample: minimal NativeAOT game entry.
//
// Cold-boot proof: preload native libs by absolute sibling path, load the
// ABI table, create a world, bulk-create a scene, run one frame through
// the real MovementSystem kernel, print timings, exit. The exporter
// measures wall time of THIS binary as the <1.0s cold-boot gate.
using System.Diagnostics;
using System.Runtime.InteropServices;
using Engine.Bindings;
using Engine.Systems;

internal static class Program
{
    internal static unsafe int Main(string[] args)
    {
        var total = Stopwatch.StartNew();

        // Sibling-preload: the exporter stages libengine_core + libflecs
        // next to this binary. NativeLibrary.Load by absolute path pins
        // the handle, but bare-SONAME NativeLibrary.TryLoad("engine_core")
        // still dlopens by SONAME — so register a DllImportResolver on the
        // Bindings assembly that maps "engine_core" to the staged file.
        // (NativeAOT-compatible: no DllImport stub needed; the resolver
        // runs for NativeLibrary loads from the target assembly.)
        var dir = AppContext.BaseDirectory;
        string? corePath = null;
        foreach (var f in new[] { "libengine_core.so.0.1.0", "libengine_core.so", "libengine_core.dylib", "engine_core.dll", })
        {
            var p = Path.Combine(dir, f);
            if (File.Exists(p))
            {
                corePath = p;
                break;
            }
        }

        if (corePath == null)
        {
            Console.Error.WriteLine("export-sample: no staged engine_core next to binary.");
            return 1;
        }

        string? flecsPath = null;
        foreach (var f in new[] { "libflecs.so", "libflecs.dylib", "flecs.dll" })
        {
            var p = Path.Combine(dir, f);
            if (File.Exists(p))
            {
                flecsPath = p;
                break;
            }
        }

        if (flecsPath != null)
        {
            try
            {
                NativeLibrary.Load(flecsPath);
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"export-sample: flecs preload failed: {ex.Message}");
                return 1;
            }
        }



        NativeApi.ApiTable* table;
        {
            // Direct-from-handle load (Benchmark1M LiveApiLoader precedent):
            // bare-SONAME TryLoad does not probe the app dir, so resolve the
            // export off the staged handle we just pinned by absolute path.
            nint coreHandle;
            try
            {
                coreHandle = NativeLibrary.Load(corePath);
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"export-sample: core handle failed: {ex.GetType().Name}: {ex.Message}");
                return 1;
            }

            if (!NativeLibrary.TryGetExport(coreHandle, "engine_get_api_table", out var addr))
            {
                Console.Error.WriteLine("export-sample: engine_get_api_table not exported.");
                return 1;
            }

            var getTable = (delegate* unmanaged[Cdecl]<NativeApi.ApiTable*>)addr;
            table = getTable();
            if (table == null)
            {
                Console.Error.WriteLine("export-sample: engine_get_api_table returned null.");
                return 1;
            }

            if (table->AbiVersionMajor != NativeApi.AbiVersionMajor
                || table->AbiVersionMinor != NativeApi.AbiVersionMinor
                || table->AbiVersionPatch != NativeApi.AbiVersionPatch)
            {
                Console.Error.WriteLine(
                    $"export-sample: ABI mismatch native {table->AbiVersionMajor}.{table->AbiVersionMinor}.{table->AbiVersionPatch} " +
                    $"!= managed {NativeApi.AbiVersionMajor}.{NativeApi.AbiVersionMinor}.{NativeApi.AbiVersionPatch}.");
                return 1;
            }

            Console.WriteLine($"export-sample: ABI {table->AbiVersionMajor}.{table->AbiVersionMinor}.{table->AbiVersionPatch} table={table->Size}B");
        }

        {
            var world = table->EngineWorldCreate();
            if (world == null)
            {
                Console.Error.WriteLine("export-sample: world_create returned null.");
                return 1;
            }

            try
            {
                // Scene: 10k entities, one registered script system.
                var n = table->EngineWorldBulkCreate(world, 10_000);
                Console.WriteLine($"export-sample: bulk_created={n}");

                var rc = table->EngineRegisterSystem(
                    world,
                    null,
                    &MovementSystem.MoveUnrolled,
                    null);
                if (rc != 0)
                {
                    Console.Error.WriteLine($"export-sample: register_system failed: {rc}.");
                    return 1;
                }

                var frame = new NativeApi.FrameDispatch
                {
                    Dt = 1.0f / 60.0f,
                };
                var t0 = Stopwatch.GetTimestamp();
                var runRc = table->EngineRunSystems(world, &frame);
                var frameUs = (Stopwatch.GetTimestamp() - t0) * 1_000_000.0 / Stopwatch.Frequency;
                if (runRc != 0)
                {
                    Console.Error.WriteLine($"export-sample: run_systems failed: {runRc}.");
                    return 1;
                }

                Console.WriteLine($"export-sample: frame_10k_1sys_us={frameUs:F1}");
            }
            finally
            {
                table->EngineWorldDestroy(world);
            }
        }

        total.Stop();
        Console.WriteLine($"export-sample: cold_boot_ms={total.Elapsed.TotalMilliseconds:F1}");
        Console.WriteLine("EXPORT-SAMPLE-OK");
        return 0;
    }
}

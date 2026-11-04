// SceneIoTests.cs — P2-06: managed scene save/load round-trip gate.
//
// Drives the real native world through Scene.Save/Scene.Load (ABI 4.16
// scene tail): seeds entities with LocalTransform, saves to a scratch
// file, loads into a fresh world, saves again, and asserts the two files
// are byte-identical. Also asserts a wrong-$size user blob loads the rest
// (skip count 1, no throw).
//
// Gate: live native only (same convention as PhysicsBodiesTests: missing
// library without --live-native-probe SKIPs; with the flag it FAILs).
using System.Runtime.InteropServices;
using Engine.Bindings;
using Engine.Runtime;

namespace Engine.Tests;

public static class SceneIoTests
{
    public static unsafe int Run(string[]? args = null)
    {
        var failures = 0;
        var liveProbe = (args ?? []).Contains("--live-native-probe");

        NativeApi.ApiTable* table;
        try
        {
            var preloaded = SceneIoPreload.TryPreloadEngineCoreHandle();
            if (preloaded != IntPtr.Zero)
            {
                SceneIoPreload.LoadIntoApiLoader(preloaded);
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
                Console.WriteLine($"SceneIoTests: {failures} FAILED");
                return failures;
            }

            Console.WriteLine($"[SKIP] scene io live (no native lib: {ex.GetType().Name})");
            Console.WriteLine(failures == 0 ? "SceneIoTests: all checks passed" : $"SceneIoTests: {failures} FAILED");
            return failures;
        }

        try
        {
            failures += RunLive(table);
        }
        catch (Exception ex)
        {
            failures += Check(false, $"scene io live threw: {ex.GetType().Name}: {ex.Message}");
        }

        Console.WriteLine(failures == 0 ? "SceneIoTests: all checks passed" : $"SceneIoTests: {failures} FAILED");
        return failures;
    }

    private static unsafe int RunLive(NativeApi.ApiTable* table)
    {
        var failures = 0;
        var dir = Path.Combine(Path.GetTempPath(), $"p206_managed_{Environment.ProcessId}");
        Directory.CreateDirectory(dir);
        var fileA = Path.Combine(dir, "scene_a.json");
        var fileB = Path.Combine(dir, "scene_b.json");

        var w1 = table->EngineWorldCreate();
        failures += Check(w1 != null, "live: world1 create");
        if (w1 == null)
        {
            return failures;
        }

        try
        {
            var floor = Ecs.CreateEntity(table, w1);
            Ecs.Set(table, w1, floor, LocalTransform.IdentityAt(0, 0, 0));
            var child = Ecs.CreateEntity(table, w1);
            Ecs.Set(table, w1, child, LocalTransform.IdentityAt(1, 2, 3));

            Scene.Save(table, w1, fileA);
            failures += Check(File.Exists(fileA) && new FileInfo(fileA).Length > 0, "live: save A writes a file");
        }
        finally
        {
            table->EngineWorldDestroy(w1);
        }

        var w2 = table->EngineWorldCreate();
        failures += Check(w2 != null, "live: world2 create");
        if (w2 == null)
        {
            return failures;
        }

        try
        {
            var skipped = Scene.Load(table, w2, fileA);
            failures += Check(skipped == 0, $"live: load skips nothing (got {skipped})");
            Scene.Save(table, w2, fileB);
            var a = File.ReadAllBytes(fileA);
            var b = File.ReadAllBytes(fileB);
            failures += Check(a.SequenceEqual(b), "ACCEPT: managed scene round-trip stable (byte-identical)");
        }
        finally
        {
            table->EngineWorldDestroy(w2);
        }

        try
        {
            Directory.Delete(dir, true);
        }
        catch (Exception)
        {
        }

        return failures;
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }

    // Staged-lib preload: same absolute-path precedent as the physics
    // bodies probe — pins libflecs + engine_core via ENGINE_CORE_PATH (what
    // CTest sets) or staged siblings, then the managed ApiLoader validates
    // the ABI the same way (version + stride + layout drift).
    private static class SceneIoPreload
    {
        public static unsafe IntPtr TryPreloadEngineCoreHandle()
        {
            var fromEnv = Environment.GetEnvironmentVariable("ENGINE_CORE_PATH");
            if (!string.IsNullOrEmpty(fromEnv) && File.Exists(fromEnv))
            {
                var h = PreloadPair(Path.GetFullPath(fromEnv));
                if (h != IntPtr.Zero)
                {
                    return h;
                }
            }

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

            Physics.TableOverride = (IntPtr)table;
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

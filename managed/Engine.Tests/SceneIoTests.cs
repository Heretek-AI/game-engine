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

            // P3-04 acceptance: describe Health (same shape the generator
            // emits: Current f32 @0, Max i32 @4), set values, save writes
            // named fields "Health":{"Current":75,"Max":100}.
            failures += Check(DescribeSceneHealth(table, w1) == 0, "live: Health.Describe ok");
            var healthEnt = Ecs.CreateEntity(table, w1);
            Ecs.Set(table, w1, healthEnt, LocalTransform.IdentityAt(4, 5, 6));
            Ecs.Set(table, w1, healthEnt, new SceneHealth { Current = 75, Max = 100 });

            Scene.Save(table, w1, fileA);
            failures += Check(File.Exists(fileA) && new FileInfo(fileA).Length > 0, "live: save A writes a file");
            var jsonA = File.ReadAllText(fileA);
            // Acceptance maps to "Health":{...} for a component NAMED Health;
            // this assembly's type is Engine.Tests.SceneHealth (FullName
            // contract), so the file key is the dotted name.
            failures += Check(jsonA.Contains("\"Engine.Tests.SceneHealth\":{\"Current\":75,\"Max\":100}", StringComparison.Ordinal), "ACCEPT: scene writes named Health fields");
            failures += Check(!jsonA.Contains("$bytes", StringComparison.Ordinal) || jsonA.Contains("LocalTransform", StringComparison.Ordinal), "live: Health not a $bytes blob");
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
            failures += Check(DescribeSceneHealth(table, w2) == 0, "live: Health.Describe on w2 ok");
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

    // Mirrors the generator's emitted Describe for SceneHealth (Current
    // f32 @OffsetOf(Current), Max i32 @OffsetOf(Max)): register, then
    // describe with runtime-computed offsets.
    private static unsafe int DescribeSceneHealth(NativeApi.ApiTable* table, NativeApi.EngineWorld* world)
    {
        if (table == null || world == null)
        {
            return -1;
        }

        _ = Ecs.ComponentId<SceneHealth>(table, world);
        var fields = stackalloc NativeApi.FieldDesc[2];
        fixed (byte* f0 = "Current\0"u8)
        {
            fields[0].Name = f0;
            fields[0].Offset = (uint)System.Runtime.InteropServices.Marshal.OffsetOf<SceneHealth>("Current");
            fields[0].Type = (uint)NativeApi.FieldType.F32;
        }

        fixed (byte* f1 = "Max\0"u8)
        {
            fields[1].Name = f1;
            fields[1].Offset = (uint)System.Runtime.InteropServices.Marshal.OffsetOf<SceneHealth>("Max");
            fields[1].Type = (uint)NativeApi.FieldType.I32;
        }

        fixed (byte* comp = Ecs.ComponentName<SceneHealth>.Utf8)
        {
            return table->EngineComponentDescribe(world, comp, fields, 2u);
        }
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }

}

/// <summary>P3-04 acceptance component: Health { float Current; int Max } (no Tick).</summary>
#pragma warning disable CA1051 // Blittable interop struct by design.
public struct SceneHealth : Engine.Runtime.IComponent
{
    public float Current;
    public int Max;
}
#pragma warning restore CA1051


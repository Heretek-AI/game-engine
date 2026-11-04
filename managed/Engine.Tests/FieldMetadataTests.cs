// FieldMetadataTests.cs — P3-04: component field-metadata contract gate.
//
// Type-shape checks (no native needed): FieldDesc mirror (16B, Name @0,
// Offset @8, Type @12), FieldType enum (F32=0..AssetRef=10), and the three
// ABI 4.24 tail entries (component_describe/field_count/field_at).
// Live negative tests (skipped when engine_core is unloadable):
// describe-before-register -> NOT_FOUND, type 11 -> BAD_ARG, offset past
// size -> BAD_ARG, nulls -> NULL_ARG, field_at OOB -> NOT_FOUND,
// re-describe overwrite works, and a Health-shaped component round-trips
// Current f32 @0 / Max i32 @4 through field_at.
using System.Runtime.InteropServices;
using Engine.Bindings;
using Engine.Runtime;

namespace Engine.Tests;

public static unsafe class FieldMetadataTests
{
    public static int Run(string[]? args = null)
    {
        var failures = 0;

        // --- Type-shape contract (no native needed) ----------------------
        failures += Check(sizeof(NativeApi.FieldDesc) == 16, "FieldDesc 16B");
        failures += Check(Marshal.OffsetOf<NativeApi.FieldDesc>("Name").ToInt64() == 0, "FieldDesc.Name @ 0");
        failures += Check(Marshal.OffsetOf<NativeApi.FieldDesc>("Offset").ToInt64() == 8, "FieldDesc.Offset @ 8");
        failures += Check(Marshal.OffsetOf<NativeApi.FieldDesc>("Type").ToInt64() == 12, "FieldDesc.Type @ 12");
        failures += Check((uint)NativeApi.FieldType.F32 == 0, "FieldType.F32 == 0");
        failures += Check((uint)NativeApi.FieldType.I32 == 1, "FieldType.I32 == 1");
        failures += Check((uint)NativeApi.FieldType.U32 == 2, "FieldType.U32 == 2");
        failures += Check((uint)NativeApi.FieldType.Bool8 == 3, "FieldType.Bool8 == 3");
        failures += Check((uint)NativeApi.FieldType.Vec2 == 4, "FieldType.Vec2 == 4");
        failures += Check((uint)NativeApi.FieldType.Vec3 == 5, "FieldType.Vec3 == 5");
        failures += Check((uint)NativeApi.FieldType.Vec4 == 6, "FieldType.Vec4 == 6");
        failures += Check((uint)NativeApi.FieldType.Quat == 7, "FieldType.Quat == 7");
        failures += Check((uint)NativeApi.FieldType.Color4 == 8, "FieldType.Color4 == 8");
        failures += Check((uint)NativeApi.FieldType.Entity == 9, "FieldType.Entity == 9");
        failures += Check((uint)NativeApi.FieldType.AssetRef == 10, "FieldType.AssetRef == 10");

        var fields = typeof(NativeApi.ApiTable).GetFields().Select(f => f.Name).ToArray();
        foreach (var want in new[] { "EngineComponentDescribe", "EngineComponentFieldCount", "EngineComponentFieldAt" })
        {
            failures += Check(fields.Contains(want), $"ApiTable.{want} exists");
        }

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
                Console.WriteLine($"FieldMetadataTests: {failures} FAILED");
                return failures;
            }

            Console.WriteLine($"[SKIP] field metadata live (no native lib: {ex.GetType().Name})");
            Console.WriteLine(failures == 0 ? "FieldMetadataTests: all checks passed" : $"FieldMetadataTests: {failures} FAILED");
            return failures;
        }

        try
        {
            failures += RunLive(table);
        }
        catch (Exception ex)
        {
            failures += Check(false, $"field metadata live threw: {ex.GetType().Name}: {ex.Message}");
        }

        Console.WriteLine(failures == 0 ? "FieldMetadataTests: all checks passed" : $"FieldMetadataTests: {failures} FAILED");
        return failures;
    }

    private static unsafe int RunLive(NativeApi.ApiTable* table)
    {
        var failures = 0;
        failures += Check(table->EngineComponentDescribe != null, "live: component_describe non-null");
        failures += Check(table->EngineComponentFieldCount != null, "live: component_field_count non-null");
        failures += Check(table->EngineComponentFieldAt != null, "live: component_field_at non-null");
        if (table->EngineComponentDescribe == null || table->EngineComponentFieldCount == null || table->EngineComponentFieldAt == null)
        {
            return failures;
        }

        var world = table->EngineWorldCreate();
        failures += Check(world != null, "live: world create");
        if (world == null)
        {
            return failures;
        }

        try
        {
            // describe-before-register -> NOT_FOUND.
            fixed (byte* ghost = "Ghost.Component\0"u8)
            {
                failures += Check(table->EngineComponentDescribe(world, ghost, null, 0) == NativeApi.ErrNotFound, "live: describe-before-register -> NOT_FOUND");
                failures += Check(table->EngineComponentFieldCount(world, ghost) == NativeApi.ErrNotFound, "live: field_count unknown -> NOT_FOUND");
                var outDesc = default(NativeApi.FieldDesc);
                failures += Check(table->EngineComponentFieldAt(world, ghost, 0, &outDesc) == NativeApi.ErrNotFound, "live: field_at unknown -> NOT_FOUND");
            }

            // Register a Health-shaped component (float Current + int Max = 8B).
            var compId = Ecs.ComponentId<LiveHealth>(table, world);
            failures += Check(compId != 0, "live: LiveHealth registered");
            fixed (byte* name = Ecs.ComponentName<LiveHealth>.Utf8)
            {
                // Null-table paths: field_count/field_at on null world -> NULL_ARG.
                failures += Check(table->EngineComponentFieldCount(null, name) == NativeApi.ErrNullArg, "live: field_count null world -> NULL_ARG");
                // Registered but undescribed: count 0 (plan §3.2 rule 7).
                failures += Check(table->EngineComponentFieldCount(world, name) == 0, "live: field_count undescribed == 0");
                // field_at undescribed -> NOT_FOUND.
                var outDesc = default(NativeApi.FieldDesc);
                failures += Check(table->EngineComponentFieldAt(world, name, 0, &outDesc) == NativeApi.ErrNotFound, "live: field_at undescribed -> NOT_FOUND");
                // Null out_desc -> NULL_ARG.
                failures += Check(table->EngineComponentFieldAt(world, name, 0, null) == NativeApi.ErrNullArg, "live: field_at null out -> NULL_ARG");
                // Null fields with count > 0 -> NULL_ARG.
                failures += Check(table->EngineComponentDescribe(world, name, null, 2) == NativeApi.ErrNullArg, "live: describe null fields -> NULL_ARG");
                // Bad type (11) -> BAD_ARG.
                var bad = stackalloc NativeApi.FieldDesc[1];
                fixed (byte* fn = "X\0"u8)
                {
                    bad[0].Name = fn;
                    bad[0].Offset = 0;
                    bad[0].Type = 11;
                    failures += Check(table->EngineComponentDescribe(world, name, bad, 1) == NativeApi.ErrBadArg, "live: describe type 11 -> BAD_ARG");
                }

                // Offset past size -> BAD_ARG.
                fixed (byte* fn2 = "Y\0"u8)
                {
                    bad[0].Name = fn2;
                    bad[0].Offset = 8;
                    bad[0].Type = (uint)NativeApi.FieldType.F32;
                    failures += Check(table->EngineComponentDescribe(world, name, bad, 1) == NativeApi.ErrBadArg, "live: describe offset past size -> BAD_ARG");
                }

                // Real describe: Current f32 @0, Max i32 @4.
                var ok = stackalloc NativeApi.FieldDesc[2];
                fixed (byte* f0 = "Current\0"u8)
                fixed (byte* f1 = "Max\0"u8)
                {
                    ok[0].Name = f0;
                    ok[0].Offset = (uint)Marshal.OffsetOf<LiveHealth>("Current");
                    ok[0].Type = (uint)NativeApi.FieldType.F32;
                    ok[1].Name = f1;
                    ok[1].Offset = (uint)Marshal.OffsetOf<LiveHealth>("Max");
                    ok[1].Type = (uint)NativeApi.FieldType.I32;
                    failures += Check(table->EngineComponentDescribe(world, name, ok, 2) == 0, "live: describe Health ok");
                }

                failures += Check(table->EngineComponentFieldCount(world, name) == 2, "live: field_count == 2");
                // Read back through field_at: Current f32 @0, Max i32 @4.
                var d0 = default(NativeApi.FieldDesc);
                var d1 = default(NativeApi.FieldDesc);
                failures += Check(table->EngineComponentFieldAt(world, name, 0, &d0) == 0, "live: field_at 0 ok");
                failures += Check(table->EngineComponentFieldAt(world, name, 1, &d1) == 0, "live: field_at 1 ok");
                failures += Check(d0.Offset == 0 && d0.Type == (uint)NativeApi.FieldType.F32, "live: Current f32 @0");
                failures += Check(d1.Offset == 4 && d1.Type == (uint)NativeApi.FieldType.I32, "live: Max i32 @4");
                failures += Check(Marshal.PtrToStringUTF8((IntPtr)d0.Name) == "Current", "live: field_at 0 name Current");
                failures += Check(Marshal.PtrToStringUTF8((IntPtr)d1.Name) == "Max", "live: field_at 1 name Max");
                // OOB -> NOT_FOUND.
                var oob = default(NativeApi.FieldDesc);
                failures += Check(table->EngineComponentFieldAt(world, name, 2, &oob) == NativeApi.ErrNotFound, "live: field_at OOB -> NOT_FOUND");
                // Re-describe overwrite: single-field version sticks.
                fixed (byte* f0b = "Current\0"u8)
                {
                    ok[0].Name = f0b;
                    failures += Check(table->EngineComponentDescribe(world, name, ok, 1) == 0, "live: re-describe ok");
                }

                failures += Check(table->EngineComponentFieldCount(world, name) == 1, "live: field_count == 1 after re-describe");
            }
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

/// <summary>P3-04 live-test component: Health { float Current; int Max } (8B).</summary>
#pragma warning disable CA1051 // Blittable interop struct by design.
public struct LiveHealth : Engine.Runtime.IComponent
{
    public float Current;
    public int Max;
}
#pragma warning restore CA1051

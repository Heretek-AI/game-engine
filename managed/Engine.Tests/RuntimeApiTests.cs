// RuntimeApiTests.cs — M2-01: acceptance checks for the ergonomic scripting surface.
//
// Verifies every AC bullet without native interop: contract existence
// (IComponent/Entity/UpdateInPhase), blittable layouts (sizeof + field
// offsets + no GC references), FixedString64/TempList/AssetRef behavior,
// and SIMD math correctness against System.Numerics oracles.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Engine.Runtime;

namespace Engine.Tests;

public static class RuntimeApiTests
{
    public static int Run()
    {
        var failures = 0;

        // --- AC1: contracts ------------------------------------------------
        failures += Check(typeof(IComponent).IsInterface, "IComponent is an interface");
        failures += Check(typeof(Entity).IsValueType, "Entity is a value type");
        failures += Check(new Entity(42).Id == 42, "Entity(42).Id == 42");
        failures += Check(!new Entity(0).IsValid && new Entity(7).IsValid, "Entity.IsValid (0 invalid)");
        failures += Check(Entity.Invalid.Id == 0 && !Entity.Invalid.IsValid, "Entity.Invalid");
        failures += Check(new Entity(1).Equals(new Entity(1)), "Entity record equality");

        var attrUse = typeof(UpdateInPhaseAttribute).GetCustomAttributes(typeof(AttributeUsageAttribute), false);
        failures += Check(attrUse.Length == 1, "UpdateInPhase has AttributeUsage");
        if (attrUse.Length == 1)
        {
            var use = (AttributeUsageAttribute)attrUse[0];
            failures += Check(use.AllowMultiple, "UpdateInPhase AllowMultiple");
            failures += Check(
                (use.ValidOn & (AttributeTargets.Struct | AttributeTargets.Class | AttributeTargets.Method)) != 0,
                "UpdateInPhase targets struct/class/method");
        }

        failures += Check(
            new UpdateInPhaseAttribute(Phase.Update).Phase == Phase.Update,
            "UpdateInPhase(Phase.Update) round-trips");
        failures += Check(
            (int)Phase.Startup == 0 && (int)Phase.PreUpdate == 1 && (int)Phase.Update == 2 &&
            (int)Phase.PostUpdate == 3 && (int)Phase.Render == 4,
            "Phase ordering Startup..Render");

        // --- AC2: blittable layouts ----------------------------------------
        failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<Entity>() == false, "Entity blittable");
        failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<FixedString64>() == false, "FixedString64 blittable");
        failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<AssetRef<FixedString64>>() == false, "AssetRef<T> blittable");
        failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<float3>() == false, "float3 blittable");
        failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<float4>() == false, "float4 blittable");
        failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<quaternion>() == false, "quaternion blittable");
        failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<float4x4>() == false, "float4x4 blittable");

        unsafe
        {
            failures += Check(sizeof(Entity) == 8, $"Entity 8B (got {sizeof(Entity)})");
            failures += Check(sizeof(FixedString64) == 65, $"FixedString64 65B (got {sizeof(FixedString64)})");
            failures += Check(sizeof(AssetRef<FixedString64>) == 8, "AssetRef<T> 8B");
            failures += Check(sizeof(float3) == 16, $"float3 16B (got {sizeof(float3)})");
            failures += Check(sizeof(float4) == 16, $"float4 16B (got {sizeof(float4)})");
            failures += Check(sizeof(quaternion) == 16, $"quaternion 16B (got {sizeof(quaternion)})");
            failures += Check(sizeof(float4x4) == 64, $"float4x4 64B (got {sizeof(float4x4)})");
            failures += Check(sizeof(TempList<float>) == 16, $"TempList<T> 16B (got {sizeof(TempList<float>)})");
            failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<SpriteRenderer2D>() == false, "SpriteRenderer2D blittable");
            failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<OrthoCamera>() == false, "OrthoCamera blittable");
            failures += Check(sizeof(SpriteRenderer2D) == 44, $"SpriteRenderer2D 44B (got {sizeof(SpriteRenderer2D)})");
            failures += Check(sizeof(OrthoCamera) == 40, $"OrthoCamera 40B (got {sizeof(OrthoCamera)})");
            failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<TilemapRenderer2D>() == false, "TilemapRenderer2D blittable");
            failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<TilemapCollider2D>() == false, "TilemapCollider2D blittable");
            failures += Check(sizeof(TilemapRenderer2D) == 32, $"TilemapRenderer2D 32B (got {sizeof(TilemapRenderer2D)})");
            failures += Check(sizeof(TilemapCollider2D) == 16, $"TilemapCollider2D 16B (got {sizeof(TilemapCollider2D)})");
            failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<ParticleSystem>() == false, "ParticleSystem blittable");
            failures += Check(sizeof(ParticleSystem) == 92, $"ParticleSystem 92B (got {sizeof(ParticleSystem)})");
        }

        failures += Check(Marshal.OffsetOf<FixedString64>(nameof(FixedString64.Length)).ToInt64() == 64, "FixedString64.Length @ 64");

        // --- AC2: FixedString64 behavior ------------------------------------
        var hello = new FixedString64("hello");
        failures += Check(hello.Count == 5 && hello.ToString() == "hello", "FixedString64 hello round-trip");
        failures += Check(new FixedString64((string?)null).Count == 0, "FixedString64 null -> empty");
        failures += Check(new FixedString64(string.Empty).Count == 0, "FixedString64 empty");
        failures += Check(new FixedString64(new string('a', 200)).Count == 64, "FixedString64 truncates to 64");
        // 63 ASCII bytes + U+20AC (€, 3 UTF-8 bytes) straddles the 64-byte
        // boundary: the € must be dropped whole, never left as a dangling
        // lead byte (63 kept, valid UTF-8).
        var boundary = new FixedString64(new string('a', 63) + "€");
        failures += Check(boundary.Count == 63 && boundary.ToString() == new string('a', 63), "FixedString64 multibyte boundary truncates whole");
        failures += Check(hello == new FixedString64("hello"), "FixedString64 equality");
        failures += Check(hello != new FixedString64("world"), "FixedString64 inequality");
        failures += Check(hello.GetHashCode() == new FixedString64("hello").GetHashCode(), "FixedString64 hash");

        Span<byte> fmt = stackalloc byte[64];
        failures += Check(hello.TryFormat(fmt, out int w) && w == 5 && fmt[0] == (byte)'h', "FixedString64 TryFormat");
        failures += Check(!hello.TryFormat(fmt.Slice(0, 2), out _), "FixedString64 TryFormat small -> false");

        var parsed = FixedString64.Parse("abc"u8);
        failures += Check(parsed.ToString() == "abc", "FixedString64 Parse");
        var longBytes = new byte[200];
        Array.Fill(longBytes, (byte)'z');
        failures += Check(FixedString64.Parse(longBytes).Count == 64, "FixedString64 Parse truncates");

        var threw = false;
        try
        {
            _ = FixedString64.Parse("a\0b"u8);
        }
        catch (ArgumentException)
        {
            threw = true;
        }

        failures += Check(threw, "FixedString64 Parse rejects NUL");

        // --- AC2: TempList behavior (native scratch) -------------------------
        unsafe
        {
            const int cap = 8;
            float* buf = (float*)NativeMemory.AlignedAlloc((nuint)(cap * sizeof(float)), 64);
            if (buf == null)
            {
                failures += Check(false, "TempList scratch alloc");
            }
            else
            {
                try
                {
                    var list = new TempList<float>(buf, cap);
                    failures += Check(list.Count == 0 && list.Capacity == cap && !list.IsFull, "TempList empty");
                    var ok = true;
                    for (var i = 0; i < cap; i++)
                    {
                        ok &= list.Add(i);
                    }

                    failures += Check(ok && list.Count == cap && list.IsFull, "TempList fills to capacity");
                    failures += Check(!list.Add(99.0f) && list.Count == cap, "TempList Add past capacity -> false");
                    failures += Check(list[3] == 3.0f, "TempList indexer");
                    list[3] = 30.0f;
                    failures += Check(list[3] == 30.0f, "TempList ref indexer writes through");

                    var oob = false;
                    try
                    {
                        _ = list[cap];
                    }
                    catch (ArgumentOutOfRangeException)
                    {
                        oob = true;
                    }

                    failures += Check(oob, "TempList indexer OOB throws");

                    var sum = 0.0f;
                    foreach (var v in list.AsSpan())
                    {
                        sum += v;
                    }

                    failures += Check(sum == (0 + 1 + 2 + 30 + 4 + 5 + 6 + 7), "TempList AsSpan iterates");
                    list.Clear();
                    failures += Check(list.Count == 0 && !list.IsFull, "TempList Clear");

                    var neg = false;
                    try
                    {
                        _ = new TempList<float>(buf, -1);
                    }
                    catch (ArgumentException)
                    {
                        neg = true;
                    }

                    failures += Check(neg, "TempList negative capacity throws");
                }
                finally
                {
                    NativeMemory.AlignedFree(buf);
                }
            }
        }

        // --- AC2: AssetRef behavior ------------------------------------------
        failures += Check(!default(AssetRef<FixedString64>).IsValid, "AssetRef default invalid");
        failures += Check(new AssetRef<FixedString64>(9).IsValid, "AssetRef(9) valid");
        failures += Check(new AssetRef<FixedString64>(9) == new AssetRef<FixedString64>(9), "AssetRef equality");

        // --- AC3: SIMD math vs System.Numerics oracles ------------------------
        var a = new float3(1, 2, 3);
        var b = new float3(4, -5, 6);
        failures += Check(float3.Dot(a, b) == ((System.Numerics.Vector3)a).Length() * 0 + (1 * 4 + 2 * -5 + 3 * 6), "float3.Dot");
        failures += Check(float3.Cross(a, b) == (float3)System.Numerics.Vector3.Cross(a, b), "float3.Cross");
        failures += Check(MathF.Abs(a.LengthSquared() - 14.0f) < 1e-6f, "float3.LengthSquared");
        failures += Check(MathF.Abs(a.Length() - MathF.Sqrt(14.0f)) < 1e-5f, "float3.Length");
        failures += Check((a + b) == new float3(5, -3, 9), "float3 add");
        failures += Check((a - b) == new float3(-3, 7, -3), "float3 sub");
        failures += Check((a * 2.0f) == new float3(2, 4, 6), "float3 scale");
        failures += Check(new float3(3, 4, 0).Normalized() == new float3(0.6f, 0.8f, 0.0f), "float3.Normalized");
        failures += Check((float3)new System.Numerics.Vector3(1, 2, 3) == a, "float3 from Vector3");
        failures += Check((System.Numerics.Vector3)new float3(1, 2, 3) == new System.Numerics.Vector3(1, 2, 3), "float3 to Vector3");

        var c = new float4(1, 2, 3, 4);
        var d = new float4(5, 6, 7, 8);
        failures += Check(float4.Dot(c, d) == 70.0f, "float4.Dot");
        failures += Check((c + d) == new float4(6, 8, 10, 12), "float4 add");
        failures += Check((d - c) == new float4(4, 4, 4, 4), "float4 sub");
        failures += Check((c * 2.0f) == new float4(2, 4, 6, 8), "float4 scale");
        failures += Check((float4)new System.Numerics.Vector4(1, 2, 3, 4) == c, "float4 from Vector4");

        var q = quaternion.Identity;
        failures += Check((q * q) == quaternion.Identity, "quaternion identity * identity");
        failures += Check(quaternion.Rotate(q, new float3(1, 2, 3)) == new float3(1, 2, 3), "quaternion.Rotate identity");
        var q90 = (quaternion)System.Numerics.Quaternion.CreateFromAxisAngle(System.Numerics.Vector3.UnitY, MathF.PI / 2.0f);
        var rotated = quaternion.Rotate(q90, new float3(1, 0, 0));
        failures += Check(
            MathF.Abs(rotated.X) < 1e-5f && MathF.Abs(rotated.Y) < 1e-5f && MathF.Abs(rotated.Z + 1.0f) < 1e-5f,
            $"quaternion.Rotate 90deg Y (got {rotated.X},{rotated.Y},{rotated.Z})");

        var m = float4x4.Identity;
        failures += Check(m == float4x4.Identity, "float4x4.Identity");
        failures += Check((m * m) == float4x4.Identity, "float4x4 identity * identity");
        failures += Check(m.TransformPoint(new float3(1, 2, 3)) == new float3(1, 2, 3), "float4x4 TransformPoint identity");
        var t = float4x4.Identity;
        t.M30 = 10.0f;
        t.M31 = 20.0f;
        t.M32 = 30.0f;
        failures += Check(t.TransformPoint(new float3(1, 2, 3)) == new float3(11, 22, 33), "float4x4 TransformPoint translate");
        failures += Check(t.Row(0) == new float4(1, 0, 0, 0), "float4x4 Row(0)");
        failures += Check(t.Row(3) == new float4(10, 20, 30, 1), "float4x4 Row(3) translation");

        var oobM = false;
        try
        {
            _ = m[4, 0];
        }
        catch (ArgumentOutOfRangeException)
        {
            oobM = true;
        }

        failures += Check(oobM, "float4x4 indexer OOB throws");

        var sys = new System.Numerics.Matrix4x4(
            1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);
        var conv = (float4x4)sys;
        failures += Check(
            conv.M00 == 1 && conv.M01 == 2 && conv.M33 == 16 &&
            (System.Numerics.Matrix4x4)conv == sys,
            "float4x4 Matrix4x4 round-trip");

        // --- Zero-alloc frame-path probes ------------------------------------
        long gc0 = GC.GetAllocatedBytesForCurrentThread();
        RunFramePathProbe();
        long gcBytes = GC.GetAllocatedBytesForCurrentThread() - gc0;
        failures += Check(gcBytes == 0, $"frame-path probe allocates 0 bytes (got {gcBytes})");

        Console.WriteLine(failures == 0 ? "RuntimeApiTests: all checks passed" : $"RuntimeApiTests: {failures} FAILED");
        return failures;
    }

    // Hot-path-shaped work only: SIMD dots, TempList appends over stack
    // memory, FixedString64 format — no LINQ/closures/strings/ToString.
    private static unsafe void RunFramePathProbe()
    {
        var acc = 0.0f;
        var v1 = new float3(1, 2, 3);
        var v2 = new float3(4, 5, 6);
        for (var i = 0; i < 1024; i++)
        {
            acc += float3.Dot(v1, v2);
        }

        const int cap = 16;
        float* buf = stackalloc float[cap];
        var list = new TempList<float>(buf, cap);
        for (var i = 0; i < cap; i++)
        {
            list.Add(acc);
        }

        var name = FixedString64.Parse("probe"u8);
        Span<byte> dst = stackalloc byte[64];
        name.TryFormat(dst, out _);

        if (acc == float.MinValue)
        {
            Console.WriteLine(list.Count);
        }
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }
}

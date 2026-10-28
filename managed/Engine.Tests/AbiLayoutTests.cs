// M1-04 layout gate: asserts the GENERATED bindings (NativeApi.g.cs) match
// the C99 ABI contract (engine_abi.h) without loading the native library.
// Runs everywhere (Windows/Linux/macOS CI) with zero native dependency:
// struct sizes, field offsets, version constants, and that every ApiTable
// entry is a SuppressGCTransition raw-call pointer.
using System.Reflection;
using System.Runtime.InteropServices;
using Engine.Bindings;

namespace Engine.Tests;

public static class AbiLayoutTests
{
    public static int Run()
    {
        var failures = 0;
        failures += Check(NativeApi.AbiVersionMajor == 1, "abi major == 1");
        failures += Check(NativeApi.AbiVersionMinor == 0, "abi minor == 0");
        failures += Check(NativeApi.AbiVersionPatch == 0, "abi patch == 0");
        failures += Check(NativeApi.ColumnStride == 64, "column stride == 64");

        unsafe
        {
            failures += Check(sizeof(NativeApi.LocalTransform) == 64, "LocalTransform 64B");
            failures += Check(sizeof(NativeApi.WorldTransform) == 64, "WorldTransform 64B");
            failures += Check(sizeof(NativeApi.EcsChunkView) == 48, "EcsChunkView 48B");
            failures += Check(sizeof(NativeApi.ApiTable) == 72, "ApiTable 72B");
            failures += Check(sizeof(NativeApi.EngineWorld) == 8, "EngineWorld opaque 8B");
        }

        failures += Check(Marshal.OffsetOf<NativeApi.EcsChunkView>("Local") == IntPtr.Zero, "chunk.Local @ 0");
        failures += Check(Marshal.OffsetOf<NativeApi.EcsChunkView>("World").ToInt64() == 8, "chunk.World @ 8");
        failures += Check(Marshal.OffsetOf<NativeApi.EcsChunkView>("Entities").ToInt64() == 16, "chunk.Entities @ 16");
        failures += Check(Marshal.OffsetOf<NativeApi.EcsChunkView>("Count").ToInt64() == 24, "chunk.Count @ 24");
        failures += Check(Marshal.OffsetOf<NativeApi.EcsChunkView>("UserData").ToInt64() == 40, "chunk.UserData @ 40");

        // Every ApiTable function pointer must carry SuppressGCTransition:
        // grep the generated source for the exact callconv spelling.
        var gcs = Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "Engine.Bindings", "NativeApi.g.cs");
        gcs = Path.GetFullPath(gcs);
        if (!File.Exists(gcs))
        {
            // Published/test-host layouts: fall back to reflection on types.
            gcs = FindGeneratedSource() ?? string.Empty;
        }

        if (gcs.Length != 0 && File.Exists(gcs))
        {
            var src = File.ReadAllText(gcs);
            var rawCalls = System.Text.RegularExpressions.Regex.Count(src, @"delegate\* unmanaged\[Cdecl, SuppressGCTransition\]");
            failures += Check(rawCalls == 8, $"8 SuppressGCTransition raw calls: 6 table entries + loader (got {rawCalls})");
            failures += Check(!src.Contains("DllImport", StringComparison.Ordinal), "no DllImport P/Invoke stubs");
        }
        else
        {
            // Reflection fallback: all ApiTable fields must be function pointers.
            foreach (var f in typeof(NativeApi.ApiTable).GetFields())
            {
                if (f.Name is "Size" or "AbiVersionMajor" or "AbiVersionMinor" or "AbiVersionPatch" or "ColumnStride")
                {
                    continue;
                }

                failures += Check(f.FieldType.IsPointer, $"ApiTable.{f.Name} is fnptr");
            }
        }

        Console.WriteLine(failures == 0 ? "AbiLayoutTests: all checks passed" : $"AbiLayoutTests: {failures} FAILED");
        return failures;
    }

    private static string? FindGeneratedSource()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        for (var i = 0; i < 8 && dir != null; i++, dir = dir.Parent)
        {
            var cand = Path.Combine(dir.FullName, "Engine.Bindings", "NativeApi.g.cs");
            if (File.Exists(cand))
            {
                return cand;
            }

            var up = Path.Combine(dir.FullName, "managed", "Engine.Bindings", "NativeApi.g.cs");
            if (File.Exists(up))
            {
                return up;
            }
        }

        return null;
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }
}

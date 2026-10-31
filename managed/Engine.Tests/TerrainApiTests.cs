// TerrainApiTests.cs — P6-01: Terrain managed contract gate.
//
// Verifies the C# terrain surface (no native needed):
//   * Type shape: HeightmapDesc/SplatLayer blittable + byte-exact sizes,
//     ChunkLod math mirrors native (IndexCount LOD0/1/2, Select steps).
//   * Validation: HeightmapDesc.IsValidForCollider accepts the 9x9/block-2
//     test grid, rejects degenerate counts.
using System.Runtime.CompilerServices;
using Engine.Runtime;

namespace Engine.Tests;

public static class TerrainApiTests
{
    private static readonly float[] LodDistances = new[] { 25.0f, 60.0f, 120.0f };

    public static int Run(string[]? args = null)
    {
        var failures = 0;
        failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<HeightmapDesc>() == false, "HeightmapDesc blittable");
        failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<SplatLayer>() == false, "SplatLayer blittable");

        unsafe
        {
            failures += Check(sizeof(HeightmapDesc) == 16, "HeightmapDesc 16B");
            failures += Check(sizeof(SplatLayer) == 24, "SplatLayer 24B");
        }

        failures += Check(ChunkLod.IndexCount(5, 0) == 96, "LOD0 4x4x6");
        failures += Check(ChunkLod.IndexCount(5, 1) == 24, "LOD1 2x2x6");
        failures += Check(ChunkLod.IndexCount(5, 2) == 6, "LOD2 1x1x6");
        failures += Check(ChunkLod.IndexCount(1, 0) == 0, "degenerate 0");
        failures += Check(ChunkLod.Select(10.0f, LodDistances) == 0, "lod near 0");
        failures += Check(ChunkLod.Select(30.0f, LodDistances) == 1, "lod mid 1");
        failures += Check(ChunkLod.Select(200.0f, LodDistances) == 2, "lod far last");

        var desc = new HeightmapDesc(1.0f, 0.0f, 10.0f, 9);
        failures += Check(desc.IsValidForCollider(2), "9x9/block2 valid");
        failures += Check(new HeightmapDesc(1.0f, 0.0f, 10.0f, 1).IsValidForCollider(2) == false, "1x1 rejected");
        failures += Check(new HeightmapDesc(1.0f, 0.0f, 10.0f, 3).IsValidForCollider(2) == false, "3/block2 rejected (3/2<2)");

        if (failures == 0)
        {
            System.Console.WriteLine("TerrainApiTests: ALL PASS");
            return 0;
        }

        System.Console.WriteLine($"TerrainApiTests: {failures} FAILURES");
        return 1;
    }

    private static int Check(bool ok, string name)
    {
        System.Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }
}

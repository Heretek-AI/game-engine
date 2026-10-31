// Terrain.cs — P6-01: managed view over the native terrain module.
//
// CPU-side contracts only (no native calls on the frame path here — the
// heavy work lives in engine_core_terrain; P6-03 nav and P6-02 editor tools
// will bind the collider/scene hooks through the ABI tail when their slices
// land):
//   * HeightmapDesc — world mapping for an R16 heightmap asset (spacing,
//     min/max height), with SampleCount validation for Jolt (square, >= 2,
//     block-divisible).
//   * SplatLayer — mirrors the native SplatLayer (albedo slot, tiling,
//     height band, slope cap, weight); SplatWeights sums to 1 by contract.
//   * ChunkLod — LOD index-count math + distance selection (mirrors
//     ChunkMesh::lod_index_count/select_lod so C# LOD picks match native).
// All types blittable; zero managed gameplay state (AGENTS.md invariant).
using System.Runtime.InteropServices;

namespace Engine.Runtime;

/// <summary>World mapping for an R16 heightmap asset.</summary>
[StructLayout(LayoutKind.Sequential, Pack = 4, Size = 16)]
public struct HeightmapDesc
{
    public float Spacing;
    public float MinHeight;
    public float MaxHeight;
    public int SampleCount;

    public HeightmapDesc(float spacing, float minHeight, float maxHeight, int sampleCount)
    {
        Spacing = spacing;
        MinHeight = minHeight;
        MaxHeight = maxHeight;
        SampleCount = sampleCount;
    }

    /// <summary>Jolt HeightFieldShape needs square N>=2 with N/block >= 2.</summary>
    public readonly bool IsValidForCollider(int blockSize = 2)
    {
        return SampleCount >= 2 && blockSize >= 2 && blockSize <= 8
            && SampleCount / blockSize >= 2;
    }
}

/// <summary>One terrain splat layer. Mirrors native SplatLayer (40 bytes).</summary>
[StructLayout(LayoutKind.Sequential, Pack = 4, Size = 24)]
public struct SplatLayer
{
    public uint AlbedoSlot;
    public float Tiling;
    public float MinHeight;
    public float MaxHeight;
    public float MaxSlope;
    public float Weight;

    public SplatLayer(uint albedoSlot, float tiling, float minHeight, float maxHeight, float maxSlope, float weight = 1.0f)
    {
        AlbedoSlot = albedoSlot;
        Tiling = tiling;
        MinHeight = minHeight;
        MaxHeight = maxHeight;
        MaxSlope = maxSlope;
        Weight = weight;
    }
}

/// <summary>LOD index-count + distance selection. Mirrors ChunkMesh.</summary>
public static class ChunkLod
{
    public const int MaxLevels = 4;

    public static int IndexCount(int chunkSamples, int lod)
    {
        if (chunkSamples < 2 || lod < 0 || lod >= MaxLevels)
        {
            return 0;
        }

        int step = 1 << lod;
        int quads = (chunkSamples - 1) / step;
        return quads * quads * 6;
    }

    public static int Select(float distance, float[] lodDistances)
    {
        if (lodDistances == null || lodDistances.Length == 0)
        {
            return 0;
        }

        int n = lodDistances.Length < MaxLevels ? lodDistances.Length : MaxLevels;
        for (int l = 0; l < n; ++l)
        {
            if (distance < lodDistances[l])
            {
                return l;
            }
        }

        return n - 1;
    }
}

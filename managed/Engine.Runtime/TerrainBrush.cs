// TerrainBrush.cs — P6-02: managed terrain brush math + editor stroke command.
//
// Managed mirror of native TerrainBrush (see
// native/terrain/include/engine/terrain/terrain_brush.hpp):
//   * TerrainBrushMode — Raise/Lower/Smooth/Flatten (matches BrushMode 1:1).
//   * TerrainBrushMath — Falloff + ApplyHeightBrush over float[] grids,
//     bit-compatible intent with the native curve (smoothstep(1-d/r)).
//     Outside-radius samples are never touched; smooth reads a snapshot.
//   * TerrainStrokeCommand — IEditCommand over a borrowed float[] height
//     buffer: captures the brush bbox before-image at construction, Apply
//     stamps the dab, Revert restores the before-image bit-exactly. One
//     stroke = one undo step through Inspector.Execute.
// All types blittable-friendly; no native calls, no gameplay state.
using System;

namespace Engine.Runtime;

/// <summary>Brush operation. Matches native BrushMode values 1:1.</summary>
public enum TerrainBrushMode : byte
{
    Raise = 0,
    Lower = 1,
    Smooth = 2,
    Flatten = 3,
}

/// <summary>One brush dab. Units: world-space center/radius, heights in world Y.</summary>
#pragma warning disable CA1051 // Visible instance fields by design: blittable interop struct (Velocity.cs precedent).
public struct TerrainBrushParams
{
    public TerrainBrushMode Mode;
    public float CenterX;
    public float CenterZ;
    public float Radius;
    public float Strength;
    public float TargetHeight;

    public TerrainBrushParams(TerrainBrushMode mode, float centerX, float centerZ, float radius, float strength, float targetHeight = 0.0f)
    {
        Mode = mode;
        CenterX = centerX;
        CenterZ = centerZ;
        Radius = radius;
        Strength = strength;
        TargetHeight = targetHeight;
    }
}
#pragma warning restore CA1051

/// <summary>Pure brush math. Mirrors native TerrainBrush::falloff/apply_brush.</summary>
public static class TerrainBrushMath
{
    /// <summary>Smoothstep falloff: 1 at center, exactly 0 at d >= radius.</summary>
    public static float Falloff(float dist, float radius)
    {
        if (!(radius > 0.0f) || !(dist < radius))
        {
            return 0.0f;
        }

        if (dist <= 0.0f)
        {
            return 1.0f;
        }

        float t = 1.0f - dist / radius;
        return t * t * (3.0f - 2.0f * t);
    }

    /// <summary>
    /// Stamp one height dab over a row-major width*height grid (spacing meters
    /// between samples). Returns false on bad args (grid untouched). Smooth
    /// reads the pre-stroke neighborhood from <paramref name="scratch"/>
    /// (same size temp; must not alias <paramref name="heights"/>).
    /// </summary>
    public static bool ApplyHeightBrush(float[] heights, int width, int height, float spacing, TerrainBrushParams p, float[]? scratch = null)
    {
        if (heights == null || width <= 0 || height <= 0 || !(spacing > 0.0f))
        {
            return false;
        }

        if (heights.Length < width * height)
        {
            return false;
        }

        if (!(p.Radius > 0.0f) || !(p.Strength >= 0.0f))
        {
            return false;
        }

        if (p.Mode == TerrainBrushMode.Smooth && scratch == null)
        {
            return false;
        }

        if (!BrushBBox(width, height, spacing, p.CenterX, p.CenterZ, p.Radius, out int x0, out int z0, out int x1, out int z1))
        {
            return true; // valid no-op: brush misses the grid
        }

        if (p.Mode == TerrainBrushMode.Smooth)
        {
            if (ReferenceEquals(scratch, heights))
            {
                return false;
            }

            if (scratch!.Length < width * height)
            {
                return false;
            }

            for (int z = z0; z < z1; ++z)
            {
                Array.Copy(heights, z * width + x0, scratch, z * width + x0, x1 - x0);
            }
        }

        float r2 = p.Radius * p.Radius;
        float tCap = p.Strength > 1.0f ? 1.0f : p.Strength;
        for (int z = z0; z < z1; ++z)
        {
            float sz = z * spacing;
            for (int x = x0; x < x1; ++x)
            {
                float sx = x * spacing;
                float dx = sx - p.CenterX;
                float dz = sz - p.CenterZ;
                float d2 = dx * dx + dz * dz;
                if (!(d2 < r2))
                {
                    continue;
                }

                float f = Falloff(MathF.Sqrt(d2), p.Radius);
                if (f <= 0.0f)
                {
                    continue;
                }

                int i = z * width + x;
                switch (p.Mode)
                {
                    case TerrainBrushMode.Raise:
                        heights[i] += p.Strength * f;
                        break;
                    case TerrainBrushMode.Lower:
                        heights[i] -= p.Strength * f;
                        break;
                    case TerrainBrushMode.Smooth:
                        {
                            float sum = 0.0f;
                            float n = 0.0f;
                            int ax0 = x > 0 ? x - 1 : x;
                            int az0 = z > 0 ? z - 1 : z;
                            int ax1 = (x + 1 < width) ? x + 2 : x + 1;
                            int az1 = (z + 1 < height) ? z + 2 : z + 1;
                            for (int az = az0; az < az1; ++az)
                            {
                                for (int ax = ax0; ax < ax1; ++ax)
                                {
                                    sum += scratch![az * width + ax];
                                    n += 1.0f;
                                }
                            }

                            float avg = n > 0.0f ? sum / n : heights[i];
                            heights[i] += (avg - heights[i]) * (tCap * f);
                            break;
                        }

                    case TerrainBrushMode.Flatten:
                        heights[i] += (p.TargetHeight - heights[i]) * (tCap * f);
                        break;
                }
            }
        }

        return true;
    }

    /// <summary>
    /// Stamp one splat dab over a per-vertex weight grid (4 floats per vertex,
    /// row-major). Pulls weights toward <paramref name="layer"/> by
    /// strength*falloff, then renormalizes to sum 1.
    /// </summary>
    public static bool PaintSplat(float[] weights, int width, int height, float spacing, float centerX, float centerZ, float radius, float strength, int layer)
    {
        if (weights == null || width <= 0 || height <= 0 || !(spacing > 0.0f))
        {
            return false;
        }

        if (weights.Length < width * height * 4)
        {
            return false;
        }

        if (!(radius > 0.0f) || !(strength >= 0.0f) || layer < 0 || layer >= 4)
        {
            return false;
        }

        if (!BrushBBox(width, height, spacing, centerX, centerZ, radius, out int x0, out int z0, out int x1, out int z1))
        {
            return true;
        }

        float r2 = radius * radius;
        float tFull = strength > 1.0f ? 1.0f : strength;
        for (int z = z0; z < z1; ++z)
        {
            float sz = z * spacing;
            for (int x = x0; x < x1; ++x)
            {
                float dx = x * spacing - centerX;
                float dz = sz - centerZ;
                float d2 = dx * dx + dz * dz;
                if (!(d2 < r2))
                {
                    continue;
                }

                float f = Falloff(MathF.Sqrt(d2), radius) * tFull;
                if (f <= 0.0f)
                {
                    continue;
                }

                int b = (z * width + x) * 4;
                for (int i = 0; i < 4; ++i)
                {
                    float target = i == layer ? 1.0f : 0.0f;
                    weights[b + i] += (target - weights[b + i]) * f;
                }

                float sum = weights[b] + weights[b + 1] + weights[b + 2] + weights[b + 3];
                if (sum > 0.0f)
                {
                    float inv = 1.0f / sum;
                    weights[b] *= inv;
                    weights[b + 1] *= inv;
                    weights[b + 2] *= inv;
                    weights[b + 3] *= inv;
                }
                else
                {
                    weights[b] = 1.0f;
                    weights[b + 1] = 0.0f;
                    weights[b + 2] = 0.0f;
                    weights[b + 3] = 0.0f;
                }
            }
        }

        return true;
    }

    public static bool BrushBBox(int width, int height, float spacing, float cx, float cz, float radius, out int x0, out int z0, out int x1, out int z1)
    {
        x0 = z0 = x1 = z1 = 0;
        if (!(radius > 0.0f))
        {
            return false;
        }

        float inv = 1.0f / spacing;
        int ix0 = (int)MathF.Floor((cx - radius) * inv);
        int iz0 = (int)MathF.Floor((cz - radius) * inv);
        int ix1 = (int)MathF.Ceiling((cx + radius) * inv) + 1;
        int iz1 = (int)MathF.Ceiling((cz + radius) * inv) + 1;
        ix0 = Math.Clamp(ix0, 0, width);
        iz0 = Math.Clamp(iz0, 0, height);
        ix1 = Math.Clamp(ix1, 0, width);
        iz1 = Math.Clamp(iz1, 0, height);
        if (ix1 <= ix0 || iz1 <= iz0)
        {
            return false;
        }

        x0 = ix0;
        z0 = iz0;
        x1 = ix1;
        z1 = iz1;
        return true;
    }
}

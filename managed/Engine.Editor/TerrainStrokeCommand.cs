// TerrainStrokeCommand.cs — P6-02: one terrain brush stroke as an editor undo step.
//
// Captures the brush bbox before-image at construction (heights and/or splat
// weights over a borrowed grid buffer), stamps the dab on Apply, restores
// the before-image bit-exactly on Revert. Goes through Inspector.Execute so
// one stroke = one undo step on the shared editor undo stack (P3-06
// pattern: SetPoseCommand over session state).
using System;
using Engine.Runtime;

namespace Engine.Editor;

/// <summary>One terrain brush stroke (height and/or splat) over borrowed grids.</summary>
public sealed class TerrainStrokeCommand : IEditCommand
{
    private readonly float[] _heights;
    private readonly int _width;
    private readonly int _height;
    private readonly float _spacing;
    private readonly TerrainBrushParams _params;
    private readonly float[]? _scratch;
    private readonly float[]? _splat;
    private readonly int _splatLayer;

    private readonly int _x0;
    private readonly int _z0;
    private readonly int _x1;
    private readonly int _z1;
    private readonly float[]? _beforeHeights;
    private readonly float[]? _beforeSplat;
    private readonly bool _hasHeights;
    private readonly bool _hasSplat;

    public string Label { get; }

    public TerrainStrokeCommand(float[] heights, int width, int height, float spacing, TerrainBrushParams brush, float[]? scratch = null, float[]? splatWeights = null, int splatLayer = 0, string? label = null)
    {
        ArgumentNullException.ThrowIfNull(heights);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(width);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(height);
        if (!(spacing > 0.0f))
        {
            throw new ArgumentOutOfRangeException(nameof(spacing));
        }

        if (heights.Length < width * height)
        {
            throw new ArgumentException("Height buffer too small.", nameof(heights));
        }

        if (!(brush.Radius > 0.0f))
        {
            throw new ArgumentOutOfRangeException(nameof(brush), "Radius must be positive.");
        }

        bool wantSplat = splatWeights != null;
        if (wantSplat && (splatLayer < 0 || splatLayer >= 4))
        {
            throw new ArgumentOutOfRangeException(nameof(splatLayer));
        }

        if (wantSplat && splatWeights!.Length < width * height * 4)
        {
            throw new ArgumentException("Splat buffer too small.", nameof(splatWeights));
        }

        _heights = heights;
        _width = width;
        _height = height;
        _spacing = spacing;
        _params = brush;
        _scratch = scratch;
        _splat = splatWeights;
        _splatLayer = splatLayer;
        Label = label ?? $"TerrainStroke({brush.Mode}@{brush.CenterX:F1},{brush.CenterZ:F1})";

        // Snapshot the bbox before-image now (brush misses grid => empty stroke).
        if (TerrainBrushMath.BrushBBox(width, height, spacing, brush.CenterX, brush.CenterZ, brush.Radius, out _x0, out _z0, out _x1, out _z1))
        {
            int bw = _x1 - _x0;
            _hasHeights = true;
            _beforeHeights = new float[(_x1 - _x0) * (_z1 - _z0)];
            for (int z = _z0; z < _z1; ++z)
            {
                Array.Copy(heights, z * width + _x0, _beforeHeights, (z - _z0) * bw, bw);
            }

            if (wantSplat)
            {
                _hasSplat = true;
                _beforeSplat = new float[(_x1 - _x0) * (_z1 - _z0) * 4];
                for (int z = _z0; z < _z1; ++z)
                {
                    Array.Copy(splatWeights!, (z * width + _x0) * 4, _beforeSplat, ((z - _z0) * bw) * 4, bw * 4);
                }
            }
            else
            {
                _hasSplat = false;
            }
        }
        else
        {
            _hasHeights = false;
            _hasSplat = false;
        }
    }

    /// <summary>Total samples covered by this stroke (0 when it misses the grid).</summary>
    public int CoveredSamples => _hasHeights ? (_x1 - _x0) * (_z1 - _z0) : 0;

    public void Apply()
    {
        if (!_hasHeights)
        {
            return;
        }

        if (!TerrainBrushMath.ApplyHeightBrush(_heights, _width, _height, _spacing, _params, _scratch))
        {
            throw new InvalidOperationException("Terrain stroke dab failed.");
        }

        if (_hasSplat)
        {
            if (!TerrainBrushMath.PaintSplat(_splat!, _width, _height, _spacing, _params.CenterX, _params.CenterZ, _params.Radius, _params.Strength, _splatLayer))
            {
                throw new InvalidOperationException("Terrain splat dab failed.");
            }
        }
    }

    public void Revert()
    {
        if (!_hasHeights)
        {
            return;
        }

        int bw = _x1 - _x0;
        for (int z = _z0; z < _z1; ++z)
        {
            Array.Copy(_beforeHeights!, (z - _z0) * bw, _heights, z * _width + _x0, bw);
        }

        if (_hasSplat)
        {
            for (int z = _z0; z < _z1; ++z)
            {
                Array.Copy(_beforeSplat!, ((z - _z0) * bw) * 4, _splat!, (z * _width + _x0) * 4, bw * 4);
            }
        }
    }
}

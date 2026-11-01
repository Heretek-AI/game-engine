// TerrainStrokeCommand.cs — P6-02: one terrain brush stroke as an editor undo step.
//
// A stroke is one or more dabs (a drag stamps many). The command captures
// the before-image over the union of every dab's bbox at construction
// (heights and/or splat weights over a borrowed grid buffer), stamps every
// dab in order on Apply, and restores the before-image bit-exactly on
// Revert. Goes through Inspector.Execute so one stroke = one undo step on
// the shared editor undo stack (P3-06 pattern: SetPoseCommand over session
// state).
//
// Height and splat are independent: pass paintHeights = false with a splat
// buffer for a paint-only stroke (heights untouched), or no splat buffer for
// a sculpt-only stroke.
using System;
using System.Collections.Generic;
using Engine.Runtime;

namespace Engine.Editor;

/// <summary>One terrain brush stroke (height and/or splat, one or more dabs) over borrowed grids.</summary>
public sealed class TerrainStrokeCommand : IEditCommand
{
    private readonly float[] _heights;
    private readonly int _width;
    private readonly int _height;
    private readonly float _spacing;
    private readonly TerrainBrushParams[] _dabs;
    private readonly float[]? _scratch;
    private readonly float[]? _splat;
    private readonly int _splatLayer;
    private readonly bool _paintHeights;

    private readonly int _x0;
    private readonly int _z0;
    private readonly int _x1;
    private readonly int _z1;
    private readonly bool _covers;
    private readonly float[]? _beforeHeights;
    private readonly float[]? _beforeSplat;

    public string Label { get; }

    /// <summary>Single-dab stroke.</summary>
    public TerrainStrokeCommand(float[] heights, int width, int height, float spacing, TerrainBrushParams brush, float[]? scratch = null, float[]? splatWeights = null, int splatLayer = 0, string? label = null, bool paintHeights = true)
        : this(heights, width, height, spacing, new[] { brush }, scratch, splatWeights, splatLayer, label, paintHeights)
    {
    }

    /// <summary>Multi-dab stroke: every dab is applied in order and undone together.</summary>
    public TerrainStrokeCommand(float[] heights, int width, int height, float spacing, IReadOnlyList<TerrainBrushParams> dabs, float[]? scratch = null, float[]? splatWeights = null, int splatLayer = 0, string? label = null, bool paintHeights = true)
    {
        ArgumentNullException.ThrowIfNull(heights);
        ArgumentNullException.ThrowIfNull(dabs);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(width);
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(height);
        if (dabs.Count == 0)
        {
            throw new ArgumentException("A stroke needs at least one dab.", nameof(dabs));
        }

        if (!(spacing > 0.0f))
        {
            throw new ArgumentOutOfRangeException(nameof(spacing));
        }

        if (heights.Length < width * height)
        {
            throw new ArgumentException("Height buffer too small.", nameof(heights));
        }

        foreach (var d in dabs)
        {
            if (!(d.Radius > 0.0f))
            {
                throw new ArgumentOutOfRangeException(nameof(dabs), "Radius must be positive.");
            }
        }

        bool wantSplat = splatWeights != null;
        if (!paintHeights && !wantSplat)
        {
            throw new ArgumentException("A stroke must paint heights, splat weights, or both.", nameof(paintHeights));
        }

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
        _dabs = new TerrainBrushParams[dabs.Count];
        for (int i = 0; i < dabs.Count; ++i)
        {
            _dabs[i] = dabs[i];
        }

        _scratch = scratch;
        _splat = splatWeights;
        _splatLayer = splatLayer;
        _paintHeights = paintHeights;
        var first = _dabs[0];
        Label = label ?? (_dabs.Length == 1
            ? $"TerrainStroke({first.Mode}@{first.CenterX:F1},{first.CenterZ:F1})"
            : $"TerrainStroke({first.Mode}, {_dabs.Length} dabs)");

        // Union of every dab's bbox (dabs that miss the grid contribute nothing).
        _x0 = int.MaxValue;
        _z0 = int.MaxValue;
        _x1 = int.MinValue;
        _z1 = int.MinValue;
        foreach (var d in _dabs)
        {
            if (TerrainBrushMath.BrushBBox(width, height, spacing, d.CenterX, d.CenterZ, d.Radius, out int x0, out int z0, out int x1, out int z1))
            {
                _x0 = Math.Min(_x0, x0);
                _z0 = Math.Min(_z0, z0);
                _x1 = Math.Max(_x1, x1);
                _z1 = Math.Max(_z1, z1);
                _covers = true;
            }
        }

        if (!_covers)
        {
            _x0 = _z0 = _x1 = _z1 = 0;
            return;
        }

        int bw = _x1 - _x0;
        int bh = _z1 - _z0;
        if (_paintHeights)
        {
            _beforeHeights = new float[bw * bh];
            for (int z = _z0; z < _z1; ++z)
            {
                Array.Copy(heights, z * width + _x0, _beforeHeights, (z - _z0) * bw, bw);
            }
        }

        if (wantSplat)
        {
            _beforeSplat = new float[bw * bh * 4];
            for (int z = _z0; z < _z1; ++z)
            {
                Array.Copy(splatWeights!, (z * width + _x0) * 4, _beforeSplat, ((z - _z0) * bw) * 4, bw * 4);
            }
        }
    }

    /// <summary>Total samples covered by this stroke (0 when it misses the grid).</summary>
    public int CoveredSamples => _covers ? (_x1 - _x0) * (_z1 - _z0) : 0;

    /// <summary>Number of dabs in this stroke.</summary>
    public int DabCount => _dabs.Length;

    public void Apply()
    {
        if (!_covers)
        {
            return;
        }

        foreach (var d in _dabs)
        {
            if (_paintHeights && !TerrainBrushMath.ApplyHeightBrush(_heights, _width, _height, _spacing, d, _scratch))
            {
                throw new InvalidOperationException("Terrain stroke dab failed.");
            }

            if (_splat != null && !TerrainBrushMath.PaintSplat(_splat, _width, _height, _spacing, d.CenterX, d.CenterZ, d.Radius, d.Strength, _splatLayer))
            {
                throw new InvalidOperationException("Terrain splat dab failed.");
            }
        }
    }

    public void Revert()
    {
        if (!_covers)
        {
            return;
        }

        int bw = _x1 - _x0;
        if (_beforeHeights != null)
        {
            for (int z = _z0; z < _z1; ++z)
            {
                Array.Copy(_beforeHeights, (z - _z0) * bw, _heights, z * _width + _x0, bw);
            }
        }

        if (_beforeSplat != null)
        {
            for (int z = _z0; z < _z1; ++z)
            {
                Array.Copy(_beforeSplat, ((z - _z0) * bw) * 4, _splat!, (z * _width + _x0) * 4, bw * 4);
            }
        }
    }
}

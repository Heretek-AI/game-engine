// ProfilerPanel.cs — P3-09: profiler panel custom Control over FrameProfiler.
//
// Draws the last-300-frame CPU graph (OnRender over CpuFrames), a
// per-system timing table (system index + ms rows), and the GPU scene-pass
// time from renderer_gpu_time. RefreshFromProfiler() re-reads the ring
// and invalidates the render.
using System.Globalization;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using Engine.Editor;

namespace Engine.EditorApp;

/// <summary>Profiler panel: 300-frame CPU graph + system timings + GPU time.</summary>
public sealed class ProfilerPanel : Control
{
    private readonly FrameProfiler? _profiler;
    private IReadOnlyList<float> _frames = [];
    private IReadOnlyList<float> _systems = [];
    private float _gpuMs = -1.0f;

    public ProfilerPanel()
    {
        Name = "Profiler";
    }

    public ProfilerPanel(FrameProfiler? profiler)
        : this()
    {
        _profiler = profiler;
        RefreshFromProfiler();
    }

    /// <summary>Re-read the profiler rings and request a redraw.</summary>
    public void RefreshFromProfiler()
    {
        if (_profiler == null)
        {
            return;
        }

        _profiler.Refresh();
        _frames = _profiler.CpuFrames;
        _systems = _profiler.SystemTimes;
        _gpuMs = _profiler.GpuTimeMs;
        InvalidateVisual();
    }

    public override void Render(DrawingContext context)
    {
        base.Render(context);
        var bounds = Bounds;
        context.FillRectangle(Brushes.Black, bounds);

        // CPU frame graph: polyline over the last 300 samples (green).
        if (_frames.Count > 1)
        {
            var max = 0.01f;
            foreach (var f in _frames)
            {
                if (f > max)
                {
                    max = f;
                }
            }

            var graphH = bounds.Height * 0.55;
            var pts = new Point[_frames.Count];
            for (var i = 0; i < _frames.Count; i++)
            {
                var x = bounds.Width * i / (FrameProfiler.MaxFrames - 1);
                var y = graphH - (graphH * (_frames[i] / max));
                pts[i] = new Point(x, y);
            }

            var geometry = new PolylineGeometry(pts, false);
            context.DrawGeometry(null, new Pen(Brushes.LimeGreen, 1.0), geometry);
            var label = $"CPU max {max.ToString("F2", CultureInfo.InvariantCulture)} ms · GPU {(_gpuMs >= 0 ? _gpuMs.ToString("F3", CultureInfo.InvariantCulture) + " ms" : "n/a")}";
            context.DrawText(new FormattedText(label, CultureInfo.InvariantCulture, FlowDirection.LeftToRight,
                Typeface.Default, 12, Brushes.White), new Point(4, graphH + 4));
        }
        else
        {
            context.DrawText(new FormattedText("no frame samples", CultureInfo.InvariantCulture,
                FlowDirection.LeftToRight, Typeface.Default, 12, Brushes.Gray), new Point(4, 4));
        }

        // Per-system timing rows below the graph.
        var y0 = bounds.Height * 0.55 + 24;
        for (var i = 0; i < _systems.Count; i++)
        {
            var row = $"sys{i}: {_systems[i].ToString("F3", CultureInfo.InvariantCulture)} ms";
            context.DrawText(new FormattedText(row, CultureInfo.InvariantCulture, FlowDirection.LeftToRight,
                Typeface.Default, 12, Brushes.LightGray), new Point(4, y0 + i * 16));
        }
    }
}

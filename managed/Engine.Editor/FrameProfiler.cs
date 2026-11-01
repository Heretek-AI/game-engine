// FrameProfiler.cs — P3-09: profiler panel model over the native timing rings.
//
// A headless-safe view-model (no GPU): wraps the ABI 4.5 profiler tail.
// RecordFrame drains native frame times (profile_count/profile_frame_time)
// into a 300-entry CPU ring; per-system timings come from
// profile_system_count/profile_system_time (captured around each
// systems_run dispatch); GpuTimeMs reads renderer_gpu_time (the
// FrameGpuTimer scene-pass resolve: -1 until a frame retires, >= 0 after).
using Engine.Bindings;

namespace Engine.Editor;

/// <summary>Profiler panel model: 300-frame CPU ring + Flecs per-system timings + GPU time.</summary>
public sealed unsafe class FrameProfiler
{
    private readonly NativeApi.ApiTable* _table;
    private readonly NativeApi.EngineWorld* _world;
    private readonly NativeApi.EngineRenderer* _renderer;
    private readonly Queue<float> _frames = new();

    /// <summary>Maximum CPU frame samples retained (spec: last 300 frames).</summary>
    public const int MaxFrames = 300;

    public FrameProfiler(NativeApi.ApiTable* table, NativeApi.EngineWorld* world, NativeApi.EngineRenderer* renderer = null)
    {
        if (table == null)
        {
            throw new ArgumentNullException(nameof(table));
        }

        if (world == null)
        {
            throw new ArgumentNullException(nameof(world));
        }

        _table = table;
        _world = world;
        _renderer = renderer;
    }

    /// <summary>CPU frame samples in this ring, oldest first.</summary>
    public IReadOnlyList<float> CpuFrames => _frames.ToArray();

    /// <summary>
    /// Last resolved GPU scene-pass time in milliseconds, or -1 when no
    /// frame resolved yet (null renderer included).
    /// </summary>
    public float GpuTimeMs => _renderer == null ? -1.0f : _table->EngineRendererGpuTime(_renderer);

    /// <summary>Per-system CPU times for the last systems_run dispatch (ms).</summary>
    public IReadOnlyList<float> SystemTimes
    {
        get
        {
            var n = _table->EngineProfileSystemCount(_world);
            var times = new List<float>((int)Math.Min(n, int.MaxValue));
            for (uint i = 0; i < n; i++)
            {
                times.Add(_table->EngineProfileSystemTime(_world, i));
            }

            return times;
        }
    }

    /// <summary>
    /// Record one CPU frame sample and drain new native samples into the
    /// ring (oldest dropped past 300). Native record failures throw.
    /// </summary>
    public void RecordFrame(float ms)
    {
        if (!float.IsFinite(ms))
        {
            throw new ArgumentOutOfRangeException(nameof(ms), "Frame time must be finite.");
        }

        var rc = _table->EngineProfileFrame(_world, ms);
        if (rc != 0)
        {
            throw new InvalidOperationException($"profile_frame failed with code {rc}.");
        }

        Drain();
    }

    /// <summary>Drain the native frame ring into the 300-entry CPU ring.</summary>
    public void Refresh()
    {
        Drain();
    }

    /// <summary>Clear the native rings and the CPU mirror.</summary>
    public void Reset()
    {
        _table->EngineProfileReset(_world);
        _frames.Clear();
    }

    private void Drain()
    {
        var count = _table->EngineProfileCount(_world);
        for (ulong i = 0; i < count; i++)
        {
            var ms = _table->EngineProfileFrameTime(_world, i);
            if (ms < 0)
            {
                continue;
            }

            _frames.Enqueue(ms);
            while (_frames.Count > MaxFrames)
            {
                _frames.Dequeue();
            }
        }
    }
}

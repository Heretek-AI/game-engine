// Timeline.cs — P5-07: managed sequencer sequencing/timing surface over the
// ABI 4.19 tail (duration/clip_get/sort/trim/evaluate_active) on top of the
// 4.18 track/clip data model. The Timeline owns one native timeline handle
// (create/destroy); tracks and clips are 1-based handles owned natively.
//
// Sequencing semantics (match the native side exactly):
//   * duration = max clip end (0 when empty);
//   * sort orders each track's clips by (start, clip id);
//   * trim cuts every clip to [0, length): drops clips starting at/after
//     length, clamps straddlers (duration = length - start);
//   * evaluate at t reports the live clip per track ([start, end) half-open:
//     ending exactly at t is NOT live), later-starting clip wins overlaps
//     (later clip id breaks exact ties), weight 1 solo else duration share.
// All entries are plain Cdecl (never SuppressGCTransition).
using System.Runtime.InteropServices;
using Engine.Bindings;

namespace Engine.Runtime;

/// <summary>
/// Managed owner of a native sequencer timeline: track/clip data model
/// plus sequencing (sort/trim) and time queries (duration/evaluate).
/// </summary>
public sealed unsafe class Timeline : IDisposable
{
    private NativeApi.EngineTimeline* _tl;
    private bool _disposed;

    // Test/live-probe injection (same pattern as UIDocument/Audio).
#pragma warning disable CA2211
    public static IntPtr TableOverride = IntPtr.Zero;
#pragma warning restore CA2211
#pragma warning disable CS0649
    private static unsafe NativeApi.ApiTable* TablePtr =>
        TableOverride != IntPtr.Zero ? (NativeApi.ApiTable*)TableOverride : NativeApi.ApiLoader.Table;
#pragma warning restore CS0649

    /// <summary>Creates an empty timeline.</summary>
    public Timeline()
    {
        var tl = TablePtr->EngineTimelineCreate();
        if (tl == null)
        {
            throw new InvalidOperationException("timeline_create failed (allocation failure).");
        }

        _tl = tl;
    }

    private void ThrowIfDisposed()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
    }

    public void Dispose()
    {
        if (!_disposed)
        {
            _disposed = true;
            if (_tl != null)
            {
                TablePtr->EngineTimelineDestroy(_tl);
                _tl = null;
            }
        }
    }

    /// <summary>Adds one track of the given kind, returns its id (&gt; 0).</summary>
    public uint AddTrack(TimelineTrackKind kind)
    {
        ThrowIfDisposed();
        var id = TablePtr->EngineTimelineTrackAdd(_tl, (uint)kind);
        if (id == 0)
        {
            throw new InvalidOperationException($"timeline_track_add failed (kind {(uint)kind}).");
        }

        return id;
    }

    /// <summary>Removes one track and all its clips.</summary>
    public void RemoveTrack(uint track)
    {
        ThrowIfDisposed();
        var rc = TablePtr->EngineTimelineTrackRemove(_tl, track);
        if (rc != 0)
        {
            throw new InvalidOperationException($"timeline_track_remove failed (rc {rc}).");
        }
    }

    /// <summary>Appends one clip row to a track, returns its clip id (&gt; 0).</summary>
    public uint AddClip(uint track, float start, float duration, float sourceStart = 0, ulong targetEntity = 0, ulong assetId = 0)
    {
        ThrowIfDisposed();
        var clip = new NativeApi.TimelineClip
        {
            Start = start,
            Duration = duration,
            SourceStart = sourceStart,
            Reserved = 0,
            TargetEntity = targetEntity,
            AssetId = assetId,
        };
        var id = TablePtr->EngineTimelineClipAdd(_tl, track, &clip);
        if (id == 0)
        {
            throw new InvalidOperationException($"timeline_clip_add failed (track {track}, start {start}, duration {duration}).");
        }

        return id;
    }

    /// <summary>Removes one clip from a track.</summary>
    public void RemoveClip(uint track, uint clip)
    {
        ThrowIfDisposed();
        var rc = TablePtr->EngineTimelineClipRemove(_tl, track, clip);
        if (rc != 0)
        {
            throw new InvalidOperationException($"timeline_clip_remove failed (rc {rc}).");
        }
    }

    /// <summary>Reads one clip row back by handle.</summary>
    public NativeApi.TimelineClip GetClip(uint track, uint clip)
    {
        ThrowIfDisposed();
        NativeApi.TimelineClip outClip;
        var rc = TablePtr->EngineTimelineClipGet(_tl, track, clip, &outClip);
        if (rc != 0)
        {
            throw new InvalidOperationException($"timeline_clip_get failed (rc {rc}).");
        }

        return outClip;
    }

    /// <summary>Timeline duration: max clip end (0 when empty).</summary>
    public float Duration
    {
        get
        {
            ThrowIfDisposed();
            float duration;
            var rc = TablePtr->EngineTimelineDuration(_tl, &duration);
            if (rc != 0)
            {
                throw new InvalidOperationException($"timeline_duration failed (rc {rc}).");
            }

            return duration;
        }
    }

    /// <summary>Sorts every track's clips by (start, clip id).</summary>
    public void Sort()
    {
        ThrowIfDisposed();
        var rc = TablePtr->EngineTimelineSort(_tl);
        if (rc != 0)
        {
            throw new InvalidOperationException($"timeline_sort failed (rc {rc}).");
        }
    }

    /// <summary>Trims every clip to [0, length); returns clips removed.</summary>
    public int Trim(float length)
    {
        ThrowIfDisposed();
        var rc = TablePtr->EngineTimelineTrim(_tl, length);
        if (rc < 0)
        {
            throw new InvalidOperationException($"timeline_trim failed (rc {rc}).");
        }

        return rc;
    }

    /// <summary>Creates a player bound to this timeline (it must outlive it).</summary>
    public TimelinePlayer CreatePlayer()
    {
        ThrowIfDisposed();
        unsafe
        {
            var handle = TablePtr->EngineTimelinePlayerCreate(_tl);
            if (handle == null)
            {
                throw new InvalidOperationException("timeline_player_create failed (null timeline).");
            }

            return new TimelinePlayer(handle);
        }
    }

    /// <summary>Evaluates the timeline at t: winning live clip per track.</summary>
    public NativeApi.TimelineActiveClip[] EvaluateActive(float t)
    {
        ThrowIfDisposed();
        var count = TablePtr->EngineTimelineEvaluateActive(_tl, t, null, 0);
        if (count < 0)
        {
            throw new InvalidOperationException($"timeline_evaluate_active size query failed (rc {count}).");
        }

        if (count == 0)
        {
            return [];
        }

        var rows = new NativeApi.TimelineActiveClip[count];
        int written;
        fixed (NativeApi.TimelineActiveClip* ptr = rows)
        {
            written = TablePtr->EngineTimelineEvaluateActive(_tl, t, ptr, (uint)count);
        }

        if (written != count)
        {
            throw new InvalidOperationException($"timeline_evaluate_active wrote {written}, expected {count}.");
        }

        return rows;
    }
}

/// <summary>Track kinds mirroring ENGINE_TIMELINE_TRACK_* (#118 spec).</summary>
public enum TimelineTrackKind : uint
{
    Transform = 0,
    AnimClip = 1,
    Audio = 2,
    Activation = 3,
}

/// <summary>Transport states mirroring ENGINE_TIMELINE_* (#118 spec).</summary>
public enum TimelineState : int
{
    Stopped = 0,
    Playing = 1,
    Paused = 2,
}

/// <summary>
/// Managed owner of a native sequencer player: transport cursor (play,
/// pause, stop, seek, time advance, rate, loop) bound to one Timeline.
/// The Timeline must outlive its players (native borrows, never owns).
/// </summary>
public sealed unsafe class TimelinePlayer : IDisposable
{
    private NativeApi.EngineTimelinePlayer* _pl;
    private bool _disposed;

    public static IntPtr TableOverride
    {
        get => Timeline.TableOverride;
        set => Timeline.TableOverride = value;
    }

    private static unsafe NativeApi.ApiTable* TablePtr =>
        Timeline.TableOverride != IntPtr.Zero ? (NativeApi.ApiTable*)Timeline.TableOverride : NativeApi.ApiLoader.Table;

    internal TimelinePlayer(NativeApi.EngineTimelinePlayer* handle)
    {
        _pl = handle;
    }

    private void ThrowIfDisposed()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
    }

    public void Dispose()
    {
        if (!_disposed)
        {
            _disposed = true;
            if (_pl != null)
            {
                TablePtr->EngineTimelinePlayerDestroy(_pl);
                _pl = null;
            }
        }
    }

    /// <summary>Starts (or resumes) playback.</summary>
    public void Play()
    {
        ThrowIfDisposed();
        var rc = TablePtr->EngineTimelinePlayerPlay(_pl);
        if (rc != 0)
        {
            throw new InvalidOperationException($"timeline_player_play failed (rc {rc}).");
        }
    }

    /// <summary>Freezes playback in place.</summary>
    public void Pause()
    {
        ThrowIfDisposed();
        var rc = TablePtr->EngineTimelinePlayerPause(_pl);
        if (rc != 0)
        {
            throw new InvalidOperationException($"timeline_player_pause failed (rc {rc}).");
        }
    }

    /// <summary>Halts playback and rewinds to 0.</summary>
    public void Stop()
    {
        ThrowIfDisposed();
        var rc = TablePtr->EngineTimelinePlayerStop(_pl);
        if (rc != 0)
        {
            throw new InvalidOperationException($"timeline_player_stop failed (rc {rc}).");
        }
    }

    /// <summary>Moves the cursor to t clamped into [0, duration].</summary>
    public void Seek(float t)
    {
        ThrowIfDisposed();
        var rc = TablePtr->EngineTimelinePlayerSeek(_pl, t);
        if (rc != 0)
        {
            throw new InvalidOperationException($"timeline_player_seek failed (rc {rc}).");
        }
    }

    /// <summary>Advances the cursor by dt * rate while playing.</summary>
    public void Advance(float dt)
    {
        ThrowIfDisposed();
        var rc = TablePtr->EngineTimelinePlayerAdvance(_pl, dt);
        if (rc != 0)
        {
            throw new InvalidOperationException($"timeline_player_advance failed (rc {rc}).");
        }
    }

    /// <summary>Current transport state.</summary>
    public TimelineState State
    {
        get
        {
            ThrowIfDisposed();
            int state;
            var rc = TablePtr->EngineTimelinePlayerState(_pl, &state);
            if (rc != 0)
            {
                throw new InvalidOperationException($"timeline_player_state failed (rc {rc}).");
            }

            return (TimelineState)state;
        }
    }

    /// <summary>Cursor time in [0, duration].</summary>
    public float Time
    {
        get
        {
            ThrowIfDisposed();
            float time;
            var rc = TablePtr->EngineTimelinePlayerTime(_pl, &time);
            if (rc != 0)
            {
                throw new InvalidOperationException($"timeline_player_time failed (rc {rc}).");
            }

            return time;
        }
    }

    /// <summary>Playback speed multiplier (&gt; 0).</summary>
    public float Rate
    {
        get
        {
            ThrowIfDisposed();
            float rate;
            var rc = TablePtr->EngineTimelinePlayerRate(_pl, &rate);
            if (rc != 0)
            {
                throw new InvalidOperationException($"timeline_player_rate failed (rc {rc}).");
            }

            return rate;
        }

        set
        {
            ThrowIfDisposed();
            var rc = TablePtr->EngineTimelinePlayerSetRate(_pl, value);
            if (rc != 0)
            {
                throw new InvalidOperationException($"timeline_player_set_rate failed (rc {rc}).");
            }
        }
    }

    /// <summary>Looping flag.</summary>
    public bool Looping
    {
        get
        {
            ThrowIfDisposed();
            int looping;
            var rc = TablePtr->EngineTimelinePlayerLooping(_pl, &looping);
            if (rc != 0)
            {
                throw new InvalidOperationException($"timeline_player_looping failed (rc {rc}).");
            }

            return looping != 0;
        }

        set
        {
            ThrowIfDisposed();
            var rc = TablePtr->EngineTimelinePlayerSetLooping(_pl, value ? 1 : 0);
            if (rc != 0)
            {
                throw new InvalidOperationException($"timeline_player_set_looping failed (rc {rc}).");
            }
        }
    }
}

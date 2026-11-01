// PlayMode.cs — M5-02: instant Play-In-Editor (PIE) via native snapshots.
//
// EnterPlay captures a snapshot (native row backup: Local + parent + name
// per entity); ExitPlay restores it (entities resurrected with the same
// ids, PIE-spawned extras deleted). Both paths are memcpy-scale — the
// measured gate is <10ms on editor scenes (see EditorBenchmark in tests).
// Re-enter safe: EnterPlay while playing is a no-op returning false;
// ExitPlay while not playing is a no-op returning false. Snapshots are
// discarded after restore (single-slot; nested play unsupported by design).
using Engine.Bindings;

namespace Engine.Editor;

/// <summary>
/// PIE controller: snapshot on enter, restore on exit. Not thread-safe
/// (editor thread only, like the rest of the UI).
/// </summary>
public sealed unsafe class PlayModeController : IDisposable
{
    private readonly EditorSession _session;
    private NativeApi.EngineEditorSnapshot* _snapshot;
    private bool _disposed;

    public PlayModeController(EditorSession session)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));
    }

    /// <summary>True between EnterPlay and ExitPlay.</summary>
    public bool IsPlaying { get; private set; }

    /// <summary>
    /// True after <see cref="Pause"/> until <see cref="Resume"/>, <see cref="Step"/>, or
    /// <see cref="ExitPlay"/>. While paused, the game loop skips system+physics ticks
    /// but the Game view keeps showing the main camera frame.
    /// </summary>
    public bool IsPaused { get; private set; }

    /// <summary>
    /// Set by <see cref="Step"/> while paused: the editor loop consumes it to tick
    /// exactly one game frame (systems + physics), then clears it. Inspectors and
    /// tests assert-then-clear via <see cref="ConsumeStepRequest"/>.
    /// </summary>
    public bool StepRequested { get; private set; }

    /// <summary>
    /// True while playing (paused or not): the toolbar background renders with the
    /// accent color. Cleared on <see cref="ExitPlay"/>/Dispose. Play edits are
    /// discarded on Stop — see <see cref="ExitPlay"/>.
    /// </summary>
    public bool UseAccentBackground => IsPlaying;

    /// <summary>
    /// Enter Play Mode: capture the scene checkpoint. Returns false when
    /// already playing (no-op). Throws when the snapshot fails.
    /// </summary>
    public bool EnterPlay()
    {
        ThrowIfDisposed();
        if (IsPlaying)
        {
            return false;
        }

        var snap = EditorSession.Table->EngineEditorSnapshotTake(_session.Handle);
        if (snap == null)
        {
            throw new InvalidOperationException("editor_snapshot_take returned null.");
        }

        _snapshot = snap;
        IsPlaying = true;
        IsPaused = false;
        StepRequested = false;
        return true;
    }

    /// <summary>
    /// Pause the game loop while staying in Play Mode: systems + physics stop
    /// ticking, the snapshot is kept. No-op (false) when not playing or already
    /// paused.
    /// </summary>
    public bool Pause()
    {
        ThrowIfDisposed();
        if (!IsPlaying || IsPaused)
        {
            return false;
        }

        IsPaused = true;
        return true;
    }

    /// <summary>
    /// Resume ticking after <see cref="Pause"/>. No-op (false) when not playing
    /// or not paused.
    /// </summary>
    public bool Resume()
    {
        ThrowIfDisposed();
        if (!IsPlaying || !IsPaused)
        {
            return false;
        }

        IsPaused = false;
        StepRequested = false;
        return true;
    }

    /// <summary>
    /// Advance exactly one game frame while paused: sets <see cref="StepRequested"/>
    /// for the editor loop to consume. No-op (false) unless playing and paused.
    /// </summary>
    public bool Step()
    {
        ThrowIfDisposed();
        if (!IsPlaying || !IsPaused)
        {
            return false;
        }

        StepRequested = true;
        return true;
    }

    /// <summary>
    /// Consume a pending <see cref="Step"/> request. Returns true when a step
    /// was pending (the caller ticks one frame); clears the flag either way.
    /// </summary>
    public bool ConsumeStepRequest()
    {
        ThrowIfDisposed();
        if (!StepRequested)
        {
            return false;
        }

        StepRequested = false;
        return true;
    }

    /// <summary>
    /// Exit Play Mode: restore the checkpoint, discard it. Returns false
    /// when not playing (no-op).
    /// </summary>
    public bool ExitPlay()
    {
        ThrowIfDisposed();
        if (!IsPlaying)
        {
            return false;
        }

        try
        {
            var rc = EditorSession.Table->EngineEditorSnapshotRestore(_session.Handle, _snapshot);
            if (rc != 0)
            {
                throw new InvalidOperationException($"editor_snapshot_restore failed: {rc}.");
            }
        }
        finally
        {
            EditorSession.Table->EngineEditorSnapshotDiscard(_snapshot);
            _snapshot = null;
            IsPlaying = false;
            IsPaused = false;
            StepRequested = false;
        }

        return true;
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        if (_snapshot != null)
        {
            EditorSession.Table->EngineEditorSnapshotDiscard(_snapshot);
            _snapshot = null;
        }

        IsPlaying = false;
        IsPaused = false;
        StepRequested = false;
    }

    private void ThrowIfDisposed()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
    }
}

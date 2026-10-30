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
    }

    private void ThrowIfDisposed()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
    }
}

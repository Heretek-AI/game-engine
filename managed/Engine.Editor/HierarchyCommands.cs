// HierarchyCommands.cs — P3-03: IEditCommand implementations for the
// Hierarchy panel. Spawn/Rename/Reparent are inverse-state commands;
// DeleteCommand holds a native subtree snapshot so undo restores deleted
// entities with their original ids plus every component byte-for-byte.
using Engine.Bindings;

namespace Engine.Editor;

/// <summary>Create-empty command: Apply spawns, Revert destroys the spawn.</summary>
public sealed class SpawnCommand : IEditCommand
{
    private readonly EditorSession _session;
    private readonly string? _name;
    private readonly ulong _parent;
    private ulong _spawned;

    public string Label { get; }

    public SpawnCommand(EditorSession session, string? name = null, ulong parent = 0, string? label = null)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));
        _name = name;
        _parent = parent;
        Label = label ?? $"Spawn({_name ?? "Empty"})";
    }

    /// <summary>Id of the entity created by the last Apply (0 before first Apply).</summary>
    public ulong Spawned => _spawned;

    public void Apply()
    {
        _spawned = _session.Spawn(_name);
        if (_parent != 0)
        {
            _session.SetParent(_spawned, _parent);
        }
    }

    public void Revert()
    {
        if (_spawned != 0)
        {
            _session.Destroy(_spawned);
            _spawned = 0;
        }
    }
}

/// <summary>Rename command: Apply sets the new name, Revert restores the old.</summary>
public sealed class RenameCommand : IEditCommand
{
    private readonly EditorSession _session;
    private readonly ulong _entity;
    private readonly string _before;
    private readonly string _after;

    public string Label { get; }

    public RenameCommand(EditorSession session, ulong entity, string before, string after, string? label = null)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));
        ArgumentOutOfRangeException.ThrowIfZero(entity);
        _entity = entity;
        _before = before ?? throw new ArgumentNullException(nameof(before));
        _after = after ?? throw new ArgumentNullException(nameof(after));
        Label = label ?? $"Rename({_entity})";
    }

    public void Apply() => _session.SetName(_entity, _after);

    public void Revert() => _session.SetName(_entity, _before);
}

/// <summary>Reparent command: Apply moves under the new parent, Revert restores the old.</summary>
public sealed class ReparentCommand : IEditCommand
{
    private readonly EditorSession _session;
    private readonly ulong _entity;
    private readonly ulong _before;
    private readonly ulong _after;

    public string Label { get; }

    public ReparentCommand(EditorSession session, ulong entity, ulong beforeParent, ulong afterParent, string? label = null)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));
        ArgumentOutOfRangeException.ThrowIfZero(entity);
        _entity = entity;
        _before = beforeParent;
        _after = afterParent;
        Label = label ?? $"Reparent({_entity})";
    }

    public void Apply() => _session.SetParent(_entity, _after);

    public void Revert() => _session.SetParent(_entity, _before);
}

/// <summary>
/// Delete command: Apply snapshots the subtree then destroys the root;
/// Revert restores the snapshot (same ids, all components). Redo replays
/// from a FRESH snapshot of the current row, so repeated undo/redo cycles
/// never replay a stale discarded handle. Holds native memory: Dispose
/// discards pending snapshots.
/// </summary>
public sealed unsafe class DeleteCommand : IEditCommand
{
    private readonly EditorSession _session;
    private readonly ulong _entity;
    private NativeApi.EngineEditorSnapshot* _before;
    private bool _disposed;

    public string Label { get; }

    public DeleteCommand(EditorSession session, ulong entity, string? label = null)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));
        ArgumentOutOfRangeException.ThrowIfZero(entity);
        _entity = entity;
        Label = label ?? $"Delete({_entity})";
    }

    public void Apply()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        DiscardBefore();
        _before = _session.TakeSubtree(_entity);
        _session.Destroy(_entity);
    }

    public void Revert()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (_before == null)
        {
            throw new InvalidOperationException("DeleteCommand has no snapshot to revert (Apply was never called).");
        }

        _session.RestoreSubtree(_before, _entity);
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        DiscardBefore();
    }

    private void DiscardBefore()
    {
        if (_before != null)
        {
            EditorSession.DiscardSnapshot(_before);
            _before = null;
        }
    }
}

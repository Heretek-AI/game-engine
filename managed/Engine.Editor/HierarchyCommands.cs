// HierarchyCommands.cs — P3-03: IEditCommand implementations for the
// Hierarchy panel. Spawn/Rename/Reparent are inverse-state commands;
// DeleteCommand holds a native subtree snapshot so undo restores deleted
// entities with their original ids plus every component byte-for-byte.
using Engine.Bindings;

namespace Engine.Editor;

/// <summary>
/// Shared undo for commands that CREATE one entity (Create Empty,
/// Duplicate). The first Apply creates it; Revert snapshots the entity's
/// subtree and destroys it; redo restores that snapshot, so the entity
/// keeps its ORIGINAL id and later commands in the same history (rename,
/// reparent, delete) still target a live entity. Holds native memory:
/// Dispose discards the pending snapshot.
/// </summary>
public abstract unsafe class CreateEntityCommand : IEditCommand
{
    private NativeApi.EngineEditorSnapshot* _undone;
    private ulong _created;
    private bool _disposed;

    protected CreateEntityCommand(EditorSession session)
    {
        Session = session ?? throw new ArgumentNullException(nameof(session));
    }

    public abstract string Label { get; }

    /// <summary>Id of the created entity (0 before the first Apply). Stable across undo/redo.</summary>
    public ulong Created => _created;

    protected EditorSession Session { get; }

    public void Apply()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (_undone != null)
        {
            Session.RestoreSubtree(_undone, _created);
            DiscardUndone();
            return;
        }

        _created = Create();
    }

    public void Revert()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (_created == 0)
        {
            return;
        }

        DiscardUndone();
        _undone = Session.TakeSubtree(_created);
        Session.Destroy(_created);
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        DiscardUndone();
        GC.SuppressFinalize(this);
    }

    /// <summary>Create the entity (first Apply only) and return its id.</summary>
    protected abstract ulong Create();

    private void DiscardUndone()
    {
        if (_undone != null)
        {
            EditorSession.DiscardSnapshot(_undone);
            _undone = null;
        }
    }
}

/// <summary>Create-empty command: Apply spawns, Revert destroys the spawn, redo restores the same id.</summary>
public sealed class SpawnCommand : CreateEntityCommand
{
    private readonly string? _name;
    private readonly ulong _parent;

    public SpawnCommand(EditorSession session, string? name = null, ulong parent = 0, string? label = null)
        : base(session)
    {
        _name = name;
        _parent = parent;
        Label = label ?? $"Spawn({_name ?? "Empty"})";
    }

    public override string Label { get; }

    /// <summary>Id of the spawned entity (0 before the first Apply). Stable across undo/redo.</summary>
    public ulong Spawned => Created;

    protected override ulong Create()
    {
        var id = Session.Spawn(_name);
        if (_parent != 0)
        {
            Session.SetParent(id, _parent);
        }

        return id;
    }
}

/// <summary>Duplicate command: Apply copies the source under its parent, Revert destroys the copy, redo restores the same id.</summary>
public sealed class DuplicateCommand : CreateEntityCommand
{
    private readonly ulong _source;

    public DuplicateCommand(EditorSession session, ulong source, string? label = null)
        : base(session)
    {
        ArgumentOutOfRangeException.ThrowIfZero(source);
        _source = source;
        Label = label ?? $"Duplicate({_source})";
    }

    public override string Label { get; }

    protected override ulong Create() => Session.Duplicate(_source);
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

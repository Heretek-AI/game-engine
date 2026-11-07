// PrefabCommands.cs — P6-09: IEditCommand implementations for prefab
// Apply/Revert over the Inspector CommandStack.
//
// ApplyPrefabCommand snapshots the ASSET FILE (text) before writing, so
// Revert restores the file byte-for-byte and re-updates instances (the
// live merge result of the apply is undone by the update that follows the
// file restore). RevertPrefabCommand snapshots the instance's live pose
// rows (entity -> 10 floats) before reverting, so Revert re-applies the
// captured poses.
using System.Text;

namespace Engine.Editor;

/// <summary>
/// Apply an instance's live state to its asset file (undo restores the
/// file and re-merges instances).
/// </summary>
public sealed class ApplyPrefabCommand : IEditCommand
{
    private readonly EditorSession _session;
    private readonly ulong _entity;
    private readonly string _assetPath;
    private string? _before;
    private bool _disposed;

    public ApplyPrefabCommand(EditorSession session, ulong entity, string assetPath)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));
        ArgumentOutOfRangeException.ThrowIfZero(entity);
        ArgumentException.ThrowIfNullOrEmpty(assetPath);
        _entity = entity;
        _assetPath = assetPath;
    }

    public string Label => $"Apply Prefab {_entity}";

    public void Apply()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        _before = File.ReadAllText(_assetPath);
        _session.ApplyPrefab(_entity);
    }

    public void Revert()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (_before is null)
        {
            return;
        }

        File.WriteAllText(_assetPath, _before);
        _session.UpdatePrefab(_assetPath);
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _before = null;
        GC.SuppressFinalize(this);
    }
}

/// <summary>
/// Revert an instance's live state to its base image (undo restores the
/// captured live poses).
/// </summary>
public sealed unsafe class RevertPrefabCommand : IEditCommand
{
    private readonly EditorSession _session;
    private readonly ulong _entity;
    private float[]? _before;
    private bool _disposed;

    public RevertPrefabCommand(EditorSession session, ulong entity)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));
        ArgumentOutOfRangeException.ThrowIfZero(entity);
        _entity = entity;
    }

    public string Label => $"Revert Prefab {_entity}";

    public void Apply()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        _before = new float[10];
        _session.GetLocal(_entity, _before.AsSpan());
        _session.RevertPrefab(_entity);
    }

    public void Revert()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (_before is null)
        {
            return;
        }

        _session.SetLocal(_entity, _before);
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _before = null;
        GC.SuppressFinalize(this);
    }
}

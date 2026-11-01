// Inspector.cs — M5-02: Reflection/Source-Generated Inspector panel.
//
// Edits native Flecs component memory in place with full Command-pattern
// Undo/Redo. V1 backs the LocalTransform component (the only natively
// addressable component in ABI 4): each SetPose command captures the
// before-image (9 floats), applies the after-image via editor_set_local,
// Undo replays the before-image. Redo re-applies: full Command contract.
//
// The undo stack is bounded (default 100, oldest dropped) so an editing
// session cannot grow memory without bound. Commands are plain data — the
// docking UI binds BeginEdit/CommitEdit to its property grid.
namespace Engine.Editor;

/// <summary>Reversible edit command (Command pattern).</summary>
public interface IEditCommand : IDisposable
{
    void Apply();
    void Revert();
    string Label { get; }

    void IDisposable.Dispose()
    {
        GC.SuppressFinalize(this);
    }
}

/// <summary>
/// Set-pose command over one entity's 10-float Local (p/q/s).
/// </summary>
public sealed class SetPoseCommand : IEditCommand
{
    private readonly EditorSession _session;
    private readonly ulong _entity;
    private readonly float[] _before;
    private readonly float[] _after;

    public string Label { get; }

    public SetPoseCommand(EditorSession session, ulong entity, float[] before, float[] after, string? label = null)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));
        ArgumentOutOfRangeException.ThrowIfZero(entity);

        ArgumentNullException.ThrowIfNull(before);
        ArgumentNullException.ThrowIfNull(after);
        if (before.Length != 10 || after.Length != 10)
        {
            throw new ArgumentException("Pose images must be exactly 10 floats.");
        }

        _entity = entity;
        _before = (float[])before.Clone();
        _after = (float[])after.Clone();
        Label = label ?? $"SetPose({_entity})";
    }

    public void Apply() => _session.SetLocal(_entity, _after);

    public void Revert() => _session.SetLocal(_entity, _before);
}

/// <summary>
/// Inspector panel: begin/commit in-place edits with Undo/Redo stacks.
/// </summary>
public sealed class Inspector
{
    private readonly EditorSession _session;
    private readonly Stack<IEditCommand> _undo = new();
    private readonly Stack<IEditCommand> _redo = new();

    /// <summary>Max retained undo depth (oldest dropped beyond).</summary>
    public int MaxUndoDepth { get; }

    public Inspector(EditorSession session, int maxUndoDepth = 100)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(maxUndoDepth);

        MaxUndoDepth = maxUndoDepth;
    }

    public int UndoCount => _undo.Count;
    public int RedoCount => _redo.Count;
    public bool CanUndo => _undo.Count > 0;
    public bool CanRedo => _redo.Count > 0;

    /// <summary>Read the current 10-float pose of an entity.</summary>
    public float[] ReadPose(ulong entity)
    {
        var buf = new float[10];
        _session.GetLocal(entity, buf);
        return buf;
    }

    /// <summary>
    /// Commit a pose edit: captures before-image, applies after-image,
    /// pushes a SetPoseCommand (clears redo).
    /// </summary>
    public void CommitPose(ulong entity, float[] after, string? label = null)
    {
        ArgumentNullException.ThrowIfNull(after);
        if (after.Length != 10)
        {
            throw new ArgumentException("Pose must be exactly 10 floats.", nameof(after));
        }

        var before = ReadPose(entity);
        var cmd = new SetPoseCommand(_session, entity, before, after, label);
        cmd.Apply();
        PushUndo(cmd);
        while (_redo.Count > 0)
        {
            _redo.Pop().Dispose();
        }
    }

    /// <summary>Execute a custom command (pushes undo, clears redo).</summary>
    public void Execute(IEditCommand command)
    {
        ArgumentNullException.ThrowIfNull(command);
        command.Apply();
        PushUndo(command);
        while (_redo.Count > 0)
        {
            _redo.Pop().Dispose();
        }
    }

    /// <summary>Undo the newest command (moves it to redo).</summary>
    public bool Undo()
    {
        if (_undo.Count == 0)
        {
            return false;
        }

        var cmd = _undo.Pop();
        cmd.Revert();
        _redo.Push(cmd);
        return true;
    }

    /// <summary>Redo the newest undone command.</summary>
    public bool Redo()
    {
        if (_redo.Count == 0)
        {
            return false;
        }

        var cmd = _redo.Pop();
        cmd.Apply();
        _undo.Push(cmd);
        return true;
    }

    /// <summary>Drop both stacks (e.g. on scene load).</summary>
    public void Clear()
    {
        while (_undo.Count > 0)
        {
            _undo.Pop().Dispose();
        }

        while (_redo.Count > 0)
        {
            _redo.Pop().Dispose();
        }
    }

    private void PushUndo(IEditCommand cmd)
    {
        _undo.Push(cmd);
        while (_undo.Count > MaxUndoDepth)
        {
            // Drop the OLDEST: reverse through a temp (depth is small).
            var keep = new Stack<IEditCommand>(_undo.Count - 1);
            while (_undo.Count > 1)
            {
                keep.Push(_undo.Pop());
            }

            _undo.Pop().Dispose();
            while (keep.Count > 0)
            {
                _undo.Push(keep.Pop());
            }
        }
    }
}

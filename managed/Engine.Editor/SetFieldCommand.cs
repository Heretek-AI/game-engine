// SetFieldCommand.cs — P3-05: one-undo-step field edit over entity_get/entity_set.
//
// Captures the whole component byte image before the edit (via the
// session's world + table), applies the after-image, and replays the
// before-image on undo. The panel commits one of these per LostFocus/Enter
// (never per keystroke); Ctrl+Z reverts through the shared Inspector stack.
using Engine.Bindings;

namespace Engine.Editor;

/// <summary>
/// Rewrites one field of one component on one entity as a single undo step.
/// </summary>
public sealed unsafe class SetFieldCommand : IEditCommand
{
    private readonly EditorSession _session;
    private readonly ulong _entity;
    private readonly string _component;
    private readonly string _field;
    private readonly byte[] _before;
    private readonly byte[] _after;
    private bool _disposed;

    public string Label { get; }

    public SetFieldCommand(
        EditorSession session,
        ulong entity,
        string component,
        string field,
        byte[] before,
        byte[] after,
        string? label = null)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));
        ArgumentOutOfRangeException.ThrowIfZero(entity);
        ArgumentException.ThrowIfNullOrEmpty(component);
        ArgumentException.ThrowIfNullOrEmpty(field);
        ArgumentNullException.ThrowIfNull(before);
        ArgumentNullException.ThrowIfNull(after);
        if (before.Length == 0 || before.Length != after.Length)
        {
            throw new ArgumentException("Field images must be non-empty and equal-length.", nameof(after));
        }

        _entity = entity;
        _component = component;
        _field = field;
        _before = (byte[])before.Clone();
        _after = (byte[])after.Clone();
        Label = label ?? $"SetField({_entity}.{_component}.{_field})";
    }

    public ulong Entity => _entity;
    public string Component => _component;
    public string Field => _field;

    public void Apply()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        Write(_after);
    }

    public void Revert()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        Write(_before);
    }

    public void Dispose()
    {
        _disposed = true;
        GC.SuppressFinalize(this);
    }

    private void Write(byte[] image)
    {
        if (_component == "LocalTransform")
        {
            _session.SetLocal(_entity, LocalPoseOf(image));
            return;
        }

        var world = _session.World;
        if (world == null)
        {
            throw new InvalidOperationException("EditorSession is disposed.");
        }

        var table = EditorSession.Table;
        var componentId = ResolveComponent(table, world, _component, image.Length);
        int rc;
        fixed (byte* p = image)
        {
            rc = table->EngineEntitySet(world, _entity, componentId, p, (uint)image.Length);
        }

        if (rc != 0)
        {
            throw new InvalidOperationException($"entity_set({_entity}.{_component}.{_field}) failed: {rc}.");
        }
    }

    /// <summary>
    /// Reads the whole current component byte image for
    /// (<paramref name="entity"/>, <paramref name="component"/>).
    /// Size comes from the field-metadata envelope: the caller passes the
    /// highest (offset + size) end over the component's described rows, or
    /// the exact native size when known (e.g. LocalTransform is 64 bytes).
    /// LocalTransform routes through editor_get_local (the native B-side
    /// mirror); everything else goes through entity_get/entity_set.
    /// </summary>
    public static byte[] ReadImage(EditorSession session, ulong entity, string component, int size)
    {
        ArgumentNullException.ThrowIfNull(session);
        ArgumentOutOfRangeException.ThrowIfZero(entity);
        ArgumentException.ThrowIfNullOrEmpty(component);
        ArgumentOutOfRangeException.ThrowIfLessThanOrEqual(size, 0);

        if (component == "LocalTransform")
        {
            return ReadLocalImage(session, entity, size);
        }

        var world = session.World;
        if (world == null)
        {
            throw new InvalidOperationException("EditorSession is disposed.");
        }

        var table = EditorSession.Table;
        var componentId = ResolveComponent(table, world, component, size);
        var image = new byte[size];
        int rc;
        fixed (byte* p = image)
        {
            rc = table->EngineEntityGet(world, entity, componentId, p, (uint)size);
        }

        if (rc != 0)
        {
            throw new InvalidOperationException($"entity_get({entity}.{component}) failed: {rc}.");
        }

        return image;
    }

    private static byte[] ReadLocalImage(EditorSession session, ulong entity, int size)
    {
        var pose = new float[10];
        session.GetLocal(entity, pose);
        var image = new byte[size];
        Buffer.BlockCopy(BitConverter.GetBytes(pose[0]), 0, image, 0, 4);
        Buffer.BlockCopy(BitConverter.GetBytes(pose[1]), 0, image, 4, 4);
        Buffer.BlockCopy(BitConverter.GetBytes(pose[2]), 0, image, 8, 4);
        Buffer.BlockCopy(BitConverter.GetBytes(pose[3]), 0, image, 16, 4);
        Buffer.BlockCopy(BitConverter.GetBytes(pose[4]), 0, image, 20, 4);
        Buffer.BlockCopy(BitConverter.GetBytes(pose[5]), 0, image, 24, 4);
        Buffer.BlockCopy(BitConverter.GetBytes(pose[6]), 0, image, 28, 4);
        Buffer.BlockCopy(BitConverter.GetBytes(pose[7]), 0, image, 32, 4);
        Buffer.BlockCopy(BitConverter.GetBytes(pose[8]), 0, image, 36, 4);
        Buffer.BlockCopy(BitConverter.GetBytes(pose[9]), 0, image, 40, 4);
        return image;
    }

    private static float[] LocalPoseOf(byte[] image)
    {
        return new float[]
        {
            BitConverter.ToSingle(image, 0), BitConverter.ToSingle(image, 4), BitConverter.ToSingle(image, 8),
            BitConverter.ToSingle(image, 16), BitConverter.ToSingle(image, 20), BitConverter.ToSingle(image, 24), BitConverter.ToSingle(image, 28),
            BitConverter.ToSingle(image, 32), BitConverter.ToSingle(image, 36), BitConverter.ToSingle(image, 40),
        };
    }

    private static ulong ResolveComponent(NativeApi.ApiTable* table, NativeApi.EngineWorld* world, string component, int size)
    {
        // Alignment must match the native registration exactly (gotcha 20:
        // a size/alignment mismatch returns 0, never a second component).
        // LocalTransform is align-64; MeshRenderer/user components are align-4.
        uint align = component == "LocalTransform" ? 64u : 4u;
        var nameBytes = System.Text.Encoding.UTF8.GetBytes(component + "\0");
        ulong id;
        fixed (byte* np = nameBytes)
        {
            id = table->EngineComponentRegister(world, np, (uint)size, align);
        }

        if (id == 0)
        {
            throw new InvalidOperationException($"component_register({component}) failed (unknown component or size mismatch).");
        }

        return id;
    }
}

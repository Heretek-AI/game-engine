// ComponentCommands.cs — P3-05: Add/Remove component undo commands.
//
// AddComponentCommand writes a zeroed (or supplied) byte image through
// entity_set (which adds the row when absent) and removes the row on
// revert via entity_remove (ABI 4.5). RemoveComponentCommand snapshots
// the current image through entity_get, removes the row with
// entity_remove on apply, and replays the snapshot bytes on undo.
using Engine.Bindings;

namespace Engine.Editor;

/// <summary>Adds a component row (zeroed image) to an entity; revert removes it.</summary>
public sealed unsafe class AddComponentCommand : IEditCommand
{
    private readonly EditorSession _session;
    private readonly ulong _entity;
    private readonly string _component;
    private readonly byte[] _image;
    private bool _disposed;

    public string Label { get; }

    public AddComponentCommand(EditorSession session, ulong entity, string component, byte[] image, string? label = null)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));
        ArgumentOutOfRangeException.ThrowIfZero(entity);
        ArgumentException.ThrowIfNullOrEmpty(component);
        ArgumentNullException.ThrowIfNull(image);
        if (image.Length == 0)
        {
            throw new ArgumentException("Component image must be non-empty.", nameof(image));
        }

        _entity = entity;
        _component = component;
        _image = (byte[])image.Clone();
        Label = label ?? $"AddComponent({_entity}.{_component})";
    }

    public void Apply()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (_component == "LocalTransform")
        {
            _session.EnsureLocal(_entity);
            return;
        }

        var world = _session.World;
        if (world == null)
        {
            throw new InvalidOperationException("EditorSession is disposed.");
        }

        var table = EditorSession.Table;
        var id = ResolveComponent(table, world, _component, _image.Length);
        int rc;
        fixed (byte* p = _image)
        {
            rc = table->EngineEntitySet(world, _entity, id, p, (uint)_image.Length);
        }

        if (rc != 0)
        {
            throw new InvalidOperationException($"entity_set({_entity}.{_component}) failed: {rc}.");
        }
    }

    public void Revert()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        Remove();
    }

    public void Dispose()
    {
        _disposed = true;
        GC.SuppressFinalize(this);
    }

    private void Remove()
    {
        if (_component == "LocalTransform")
        {
            throw new InvalidOperationException("LocalTransform is structural and cannot be removed.");
        }

        var world = _session.World;
        if (world == null)
        {
            throw new InvalidOperationException("EditorSession is disposed.");
        }

        var table = EditorSession.Table;
        var id = ResolveComponent(table, world, _component, _image.Length);
        var rc = table->EngineEntityRemove(world, _entity, id);
        if (rc != 0)
        {
            throw new InvalidOperationException($"entity_remove({_entity}.{_component}) failed: {rc}.");
        }
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

/// <summary>Removes a component row; undo restores the snapshotted bytes.</summary>
public sealed unsafe class RemoveComponentCommand : IEditCommand
{
    private readonly EditorSession _session;
    private readonly ulong _entity;
    private readonly string _component;
    private readonly int _size;
    private byte[]? _before;
    private bool _disposed;

    public string Label { get; }

    public RemoveComponentCommand(EditorSession session, ulong entity, string component, int componentSize, string? label = null)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));
        ArgumentOutOfRangeException.ThrowIfZero(entity);
        ArgumentException.ThrowIfNullOrEmpty(component);
        ArgumentOutOfRangeException.ThrowIfLessThanOrEqual(componentSize, 0);
        _entity = entity;
        _component = component;
        _size = componentSize;
        Label = label ?? $"RemoveComponent({entity}.{component})";
    }

    public void Apply()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (_component == "LocalTransform")
        {
            throw new InvalidOperationException("LocalTransform is structural and cannot be removed.");
        }

        _before = SetFieldCommand.ReadImage(_session, _entity, _component, _size);
        var world = _session.World;
        if (world == null)
        {
            throw new InvalidOperationException("EditorSession is disposed.");
        }

        var table = EditorSession.Table;
        var id = ResolveComponent(table, world, _component, _size);
        var rc = table->EngineEntityRemove(world, _entity, id);
        if (rc != 0)
        {
            throw new InvalidOperationException($"entity_remove({_entity}.{_component}) failed: {rc}.");
        }
    }

    public void Revert()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (_before == null)
        {
            throw new InvalidOperationException("RemoveComponentCommand has no snapshot to revert (Apply was never called).");
        }

        var world = _session.World;
        if (world == null)
        {
            throw new InvalidOperationException("EditorSession is disposed.");
        }

        var table = EditorSession.Table;
        var id = ResolveComponent(table, world, _component, _size);
        int rc;
        fixed (byte* p = _before)
        {
            rc = table->EngineEntitySet(world, _entity, id, p, (uint)_size);
        }

        if (rc != 0)
        {
            throw new InvalidOperationException($"entity_set({_entity}.{_component}) failed: {rc}.");
        }
    }

    public void Dispose()
    {
        _disposed = true;
        GC.SuppressFinalize(this);
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

// EditorSession.cs — M5-02: managed owner of a native editor session.
//
// Wraps the ABI 4 editor tail: one session bound to one EngineWorld native
// handle (created via world_create on the same ApiTable). All hierarchy
// reads and in-place edits go through the session; Dispose destroys it
// (idempotent). Table injection (TableOverride) mirrors GraphicsCompositor
// so the ctest live probe can drive an absolute-path library handle.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using Engine.Bindings;

namespace Engine.Editor;

/// <summary>
/// Managed owner of a native EngineEditorSession: hierarchy walks
/// (EcsChildOf), in-place LocalTransform edits, PIE snapshots.
/// </summary>
public sealed unsafe class EditorSession : IDisposable
{
    private NativeApi.EngineEditorSession* _session;
    private NativeApi.EngineWorld* _world;
    private bool _disposed;

    /// <summary>Test/live-probe table injection (mirrors GraphicsCompositor).</summary>
#pragma warning disable CA2211
    public static IntPtr TableOverride = IntPtr.Zero;
#pragma warning restore CA2211
    private static unsafe NativeApi.ApiTable* TablePtr =>
        TableOverride != IntPtr.Zero ? (NativeApi.ApiTable*)TableOverride : NativeApi.ApiLoader.Table;

    /// <summary>Bind a session to an open native world handle.</summary>
    public EditorSession(NativeApi.EngineWorld* world)
    {
        if (world == null)
        {
            throw new ArgumentNullException(nameof(world));
        }

        var session = TablePtr->EngineEditorCreate(world);
        if (session == null)
        {
            throw new InvalidOperationException("editor_create returned null.");
        }

        _session = session;
        _world = world;
    }

    /// <summary>Spawn one named identity entity; returns its id.</summary>
    public ulong Spawn(string? name = null)
    {
        ThrowIfDisposed();
        ulong id = 0;
        int rc;
        if (string.IsNullOrEmpty(name))
        {
            rc = TablePtr->EngineEditorSpawn(_session, null, &id);
        }
        else
        {
            Span<byte> utf8 = stackalloc byte[64];
            var n = Encoding.UTF8.GetBytes(name, utf8);
            if (n <= 0 || n >= 64)
            {
                throw new ArgumentException("entity name must be 1..63 UTF-8 bytes.", nameof(name));
            }

            utf8[n] = 0;
            fixed (byte* p = utf8)
            {
                rc = TablePtr->EngineEditorSpawn(_session, p, &id);
            }
        }

        if (rc != 0 || id == 0)
        {
            throw new InvalidOperationException($"editor_spawn failed: {rc}.");
        }

        return id;
    }

    /// <summary>Rename an entity (null/empty clears to unnamed).</summary>
    public void SetName(ulong entity, string? name)
    {
        ThrowIfDisposed();
        ArgumentOutOfRangeException.ThrowIfZero(entity);

        int rc;
        if (string.IsNullOrEmpty(name))
        {
            rc = TablePtr->EngineEditorSetName(_session, entity, null);
        }
        else
        {
            Span<byte> utf8 = stackalloc byte[64];
            var n = Encoding.UTF8.GetBytes(name, utf8);
            if (n <= 0 || n >= 64)
            {
                throw new ArgumentException("entity name must be 1..63 UTF-8 bytes.", nameof(name));
            }

            utf8[n] = 0;
            fixed (byte* p = utf8)
            {
                rc = TablePtr->EngineEditorSetName(_session, entity, p);
            }
        }

        if (rc != 0)
        {
            throw new InvalidOperationException($"editor_set_name({entity}) failed: {rc}.");
        }
    }

    /// <summary>Reparent under <paramref name="parent"/> (0 = detach to root).</summary>
    public void SetParent(ulong entity, ulong parent)
    {
        ThrowIfDisposed();
        ArgumentOutOfRangeException.ThrowIfZero(entity);

        var rc = TablePtr->EngineEditorSetParent(_session, entity, parent);
        if (rc != 0)
        {
            throw new InvalidOperationException($"editor_set_parent({entity} -> {parent}) failed: {rc}.");
        }
    }

    /// <summary>Ensure the entity carries a LocalTransform row (idempotent).</summary>
    public void EnsureLocal(ulong entity)
    {
        ThrowIfDisposed();
        ArgumentOutOfRangeException.ThrowIfZero(entity);

        var rc = TablePtr->EngineEditorAddLocal(_session, entity);
        if (rc != 0)
        {
            throw new InvalidOperationException($"editor_add_local({entity}) failed: {rc}.");
        }
    }

    /// <summary>Overwrite the 10-float Local pose (p/q/s).</summary>
    public void SetLocal(ulong entity, ReadOnlySpan<float> xyzQS)
    {
        ThrowIfDisposed();
        ArgumentOutOfRangeException.ThrowIfZero(entity);

        if (xyzQS.Length != 10)
        {
            throw new ArgumentException("Local pose must be exactly 10 floats (px,py,pz,qx,qy,qz,qw,sx,sy,sz).", nameof(xyzQS));
        }

        fixed (float* p = xyzQS)
        {
            var rc = TablePtr->EngineEditorSetLocal(_session, entity, p);
            if (rc != 0)
            {
                throw new InvalidOperationException($"editor_set_local({entity}) failed: {rc}.");
            }
        }
    }

    /// <summary>Read the 10-float Local pose into <paramref name="outXyzQS"/>.</summary>
    public void GetLocal(ulong entity, Span<float> outXyzQS)
    {
        ThrowIfDisposed();
        ArgumentOutOfRangeException.ThrowIfZero(entity);

        if (outXyzQS.Length != 10)
        {
            throw new ArgumentException("Local pose buffer must be exactly 10 floats.", nameof(outXyzQS));
        }

        fixed (float* p = outXyzQS)
        {
            var rc = TablePtr->EngineEditorGetLocal(_session, entity, p);
            if (rc != 0)
            {
                throw new InvalidOperationException($"editor_get_local({entity}) failed: {rc}.");
            }
        }
    }

    /// <summary>Number of direct children (0 = roots).</summary>
    public ulong CountChildren(ulong parent)
    {
        ThrowIfDisposed();
        ulong n = 0;
        var rc = TablePtr->EngineEditorCountChildren(_session, parent, &n);
        if (rc != 0)
        {
            throw new InvalidOperationException($"editor_count_children({parent}) failed: {rc}.");
        }

        return n;
    }

    /// <summary>Indexed child of <paramref name="parent"/> (0 = roots).</summary>
    public ulong GetChild(ulong parent, ulong index)
    {
        ThrowIfDisposed();
        ulong child = 0;
        var rc = TablePtr->EngineEditorGetChild(_session, parent, index, &child);
        if (rc != 0 || child == 0)
        {
            throw new InvalidOperationException($"editor_get_child({parent},{index}) failed: {rc}.");
        }

        return child;
    }

    /// <summary>Number of root entities.</summary>
    public ulong CountRoots()
    {
        ThrowIfDisposed();
        ulong n = 0;
        var rc = TablePtr->EngineEditorCountRoots(_session, &n);
        if (rc != 0)
        {
            throw new InvalidOperationException($"editor_count_roots failed: {rc}.");
        }

        return n;
    }

    /// <summary>Indexed root entity.</summary>
    public ulong GetRoot(ulong index)
    {
        ThrowIfDisposed();
        ulong root = 0;
        var rc = TablePtr->EngineEditorGetRoot(_session, index, &root);
        if (rc != 0 || root == 0)
        {
            throw new InvalidOperationException($"editor_get_root({index}) failed: {rc}.");
        }

        return root;
    }

    /// <summary>True when the entity inherits from a prefab (EcsIsA).</summary>
    public bool IsPrefabInstance(ulong entity)
    {
        ThrowIfDisposed();
        ArgumentOutOfRangeException.ThrowIfZero(entity);

        var rc = TablePtr->EngineEditorIsPrefabInstance(_session, entity);
        if (rc < 0)
        {
            throw new InvalidOperationException($"editor_is_prefab_instance({entity}) failed: {rc}.");
        }

        return rc == 1;
    }

    /// <summary>Destroy an entity (cascades to descendants). Throws on failure.</summary>
    public void Destroy(ulong entity)
    {
        ThrowIfDisposed();
        ArgumentOutOfRangeException.ThrowIfZero(entity);

        var rc = TablePtr->EngineEditorDestroyEntity(_session, entity);
        if (rc != 0)
        {
            throw new InvalidOperationException($"editor_destroy({entity}) failed: {rc}.");
        }
    }

    /// <summary>Read an entity's display name (empty when unnamed).</summary>
    public string GetName(ulong entity)
    {
        ThrowIfDisposed();
        ArgumentOutOfRangeException.ThrowIfZero(entity);

        Span<byte> utf8 = stackalloc byte[64];
        int rc;
        fixed (byte* p = utf8)
        {
            rc = TablePtr->EngineEditorGetName(_session, entity, p, 64);
        }

        if (rc != 0)
        {
            throw new InvalidOperationException($"editor_get_name({entity}) failed: {rc}.");
        }

        var n = utf8.IndexOf((byte)0);
        return System.Text.Encoding.UTF8.GetString(utf8[..n]);
    }

    /// <summary>Duplicate an entity under the same parent; returns the copy id.</summary>
    public ulong Duplicate(ulong entity)
    {
        ThrowIfDisposed();
        ArgumentOutOfRangeException.ThrowIfZero(entity);

        ulong copy = 0;
        var rc = TablePtr->EngineEditorDuplicate(_session, entity, &copy);
        if (rc != 0 || copy == 0)
        {
            throw new InvalidOperationException($"editor_duplicate({entity}) failed: {rc}.");
        }

        return copy;
    }

    /// <summary>
    /// Take a subtree snapshot (root + descendants). Returns a native handle
    /// the caller must discard via <see cref="DiscardSnapshot"/>.
    /// </summary>
    public unsafe NativeApi.EngineEditorSnapshot* TakeSubtree(ulong entity)
    {
        ThrowIfDisposed();
        ArgumentOutOfRangeException.ThrowIfZero(entity);

        var snap = TablePtr->EngineEditorSnapshotTakeSubtree(_session, entity);
        if (snap == null)
        {
            throw new InvalidOperationException($"editor_snapshot_take_subtree({entity}) returned null.");
        }

        return snap;
    }

    /// <summary>Restore a subtree snapshot (touches only captured rows).</summary>
    public unsafe void RestoreSubtree(NativeApi.EngineEditorSnapshot* snapshot, ulong root)
    {
        ThrowIfDisposed();
        if (snapshot == null)
        {
            throw new ArgumentNullException(nameof(snapshot));
        }

        ArgumentOutOfRangeException.ThrowIfZero(root);

        var rc = TablePtr->EngineEditorSnapshotRestoreSubtree(_session, snapshot, root);
        if (rc != 0)
        {
            throw new InvalidOperationException($"editor_snapshot_restore_subtree({root}) failed: {rc}.");
        }
    }

    /// <summary>Discard a subtree snapshot handle (idempotent on null).</summary>
    public static unsafe void DiscardSnapshot(NativeApi.EngineEditorSnapshot* snapshot)
    {
        if (snapshot != null)
        {
            TablePtr->EngineEditorSnapshotDiscard(snapshot);
        }
    }

    internal NativeApi.EngineEditorSession* Handle => _disposed ? null : _session;

    /// <summary>Borrowed native world this session is bound to (null when disposed).</summary>
    internal NativeApi.EngineWorld* World => _disposed ? null : _world;

    /// <summary>True when the entity currently carries the named component (P3-05 panel).</summary>
    public bool HasComponent(ulong entity, string componentName, int componentSize)
    {
        ThrowIfDisposed();
        ArgumentOutOfRangeException.ThrowIfZero(entity);
        ArgumentException.ThrowIfNullOrEmpty(componentName);
        ArgumentOutOfRangeException.ThrowIfLessThanOrEqual(componentSize, 0);

        // LocalTransform lives in the native B-side mirror (registered by
        // EcsWorld::construct, not the generic name registry): presence is
        // editor_get_local success, not a name-registry lookup.
        if (componentName == "LocalTransform")
        {
            var probe = new float[10];
            return TryGetLocal(entity, probe);
        }

        var id = ComponentId(componentName, componentSize);
        var image = new byte[componentSize];
        int rc;
        fixed (byte* img = image)
        {
            rc = TablePtr->EngineEntityGet(_world, entity, id, img, (uint)componentSize);
        }

        return rc == 0;
    }

    private bool TryGetLocal(ulong entity, float[] pose)
    {
        fixed (float* p = pose)
        {
            return TablePtr->EngineEditorGetLocal(_session, entity, p) == 0;
        }
    }

    /// <summary>Resolve (and validate) the native component id for a name+size pair.</summary>
    private ulong ComponentId(string componentName, int componentSize)
    {
        var nameBytes = System.Text.Encoding.UTF8.GetBytes(componentName + "\0");
        ulong id;
        fixed (byte* np = nameBytes)
        {
            id = TablePtr->EngineComponentRegister(_world, np, (uint)componentSize, 4);
        }

        if (id == 0)
        {
            throw new InvalidOperationException($"component_register({componentName}) failed (unknown component or size mismatch).");
        }

        return id;
    }

    /// <summary>Field rows for a component (empty when undescribed). P3-05 panel.</summary>
    public IReadOnlyList<ComponentFieldInfo> FieldsOf(string componentName)
    {
        ThrowIfDisposed();
        ArgumentException.ThrowIfNullOrEmpty(componentName);
        return ComponentFields.ReadAll(_world, componentName);
    }

    public static unsafe NativeApi.ApiTable* Table => TablePtr;

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        if (_session != null)
        {
            TablePtr->EngineEditorDestroy(_session);
            _session = null;
        }

        _world = null;
    }

    private void ThrowIfDisposed([CallerMemberName] string? caller = null)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
    }
}

/// <summary>
/// One component field row read back through the ABI 4.4 field-metadata
/// tail (P3-04): managed name plus the native (offset, ENGINE_ABI_FIELD_*
/// type) pair. Returned by <see cref="ComponentFields.ReadAll"/>.
/// </summary>
public sealed record ComponentFieldInfo(string Name, uint Offset, uint Type);

/// <summary>
/// ABI 4.4 field-metadata readback over an open native world (P3-04):
/// component_describe for generator-equivalent registration, field count/at
/// readers for the C# editor. Init-time API: not for use inside system
/// callbacks.
/// </summary>
public static unsafe class ComponentFields
{
    private static NativeApi.ApiTable* TablePtr =>
        EditorSession.TableOverride != IntPtr.Zero
            ? (NativeApi.ApiTable*)EditorSession.TableOverride
            : NativeApi.ApiLoader.Table;

    /// <summary>
    /// Stores field metadata for the component registered under
    /// <paramref name="componentName"/> (replaces any prior rows).
    /// </summary>
    public static void Describe(
        NativeApi.EngineWorld* world,
        string componentName,
        ReadOnlySpan<(string Name, uint Offset, uint Type)> fields)
    {
        ArgumentNullException.ThrowIfNull(world);
        ArgumentNullException.ThrowIfNull(componentName);
        var nameBytes = Encoding.UTF8.GetBytes(componentName + "\0");
        var pinned = new byte[fields.Length][];
        for (var i = 0; i < fields.Length; i++)
        {
            pinned[i] = Encoding.UTF8.GetBytes(fields[i].Name + "\0");
        }

        var handles = new System.Runtime.InteropServices.GCHandle[pinned.Length];
        try
        {
            var descs = new NativeApi.EngineFieldDesc[fields.Length];
            for (var i = 0; i < pinned.Length; i++)
            {
                handles[i] = System.Runtime.InteropServices.GCHandle.Alloc(pinned[i], System.Runtime.InteropServices.GCHandleType.Pinned);
                descs[i].Name = (byte*)handles[i].AddrOfPinnedObject().ToPointer();
                descs[i].Offset = fields[i].Offset;
                descs[i].Type = fields[i].Type;
            }

            int rc;
            fixed (byte* np = nameBytes)
            {
                fixed (NativeApi.EngineFieldDesc* dp = descs)
                {
                    rc = TablePtr->EngineComponentDescribe(world, np, dp, (uint)fields.Length);
                }
            }

            if (rc != 0)
            {
                throw new InvalidOperationException($"component_describe({componentName}) failed: {rc}.");
            }
        }
        finally
        {
            foreach (var h in handles)
            {
                if (h.IsAllocated)
                {
                    h.Free();
                }
            }
        }
    }

    /// <summary>Reads back every field row for a component (empty when undescribed).</summary>
    public static IReadOnlyList<ComponentFieldInfo> ReadAll(NativeApi.EngineWorld* world, string componentName)
    {
        ArgumentNullException.ThrowIfNull(world);
        ArgumentNullException.ThrowIfNull(componentName);
        var nameBytes = Encoding.UTF8.GetBytes(componentName + "\0");
        uint count = 0;
        int rc;
        fixed (byte* np = nameBytes)
        {
            rc = TablePtr->EngineComponentFieldCount(world, np, &count);
        }

        if (rc != 0)
        {
            throw new InvalidOperationException($"component_field_count({componentName}) failed: {rc}.");
        }

        var rows = new List<ComponentFieldInfo>((int)count);
        for (uint i = 0; i < count; i++)
        {
            rows.Add(ReadAt(world, componentName, i));
        }

        return rows;
    }

    private static ComponentFieldInfo ReadAt(NativeApi.EngineWorld* world, string componentName, uint index)
    {
        var nameBytes = Encoding.UTF8.GetBytes(componentName + "\0");
        Span<byte> buf = stackalloc byte[256];
        uint off = 0, type = 0;
        int rc;
        fixed (byte* np = nameBytes)
        {
            fixed (byte* bp = buf)
            {
                rc = TablePtr->EngineComponentFieldAt(world, np, index, bp, 256, &off, &type);
            }
        }

        if (rc != 0)
        {
            throw new InvalidOperationException($"component_field_at({componentName}#{index}) failed: {rc}.");
        }

        var nul = buf.IndexOf((byte)0);
        var name = Encoding.UTF8.GetString(buf.Slice(0, nul < 0 ? buf.Length : nul));
        return new ComponentFieldInfo(name, off, type);
    }
}

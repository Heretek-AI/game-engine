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

    internal NativeApi.EngineEditorSession* Handle => _disposed ? null : _session;

    /// <summary>
    /// The borrowed native world this session is bound to (null after
    /// Dispose). P1-10: lets a snapshot test attach a Renderer to the same
    /// world; the renderer borrows it and must be destroyed first.
    /// </summary>
    public NativeApi.EngineWorld* World => _disposed ? null : _world;

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
    }

    private void ThrowIfDisposed([CallerMemberName] string? caller = null)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
    }
}

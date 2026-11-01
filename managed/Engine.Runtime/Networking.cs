// Networking.cs — P7-04: ENet sessions, snapshot replication, reliable RPCs.
//
// Managed wrapper over the ABI 4.11 networking tail (net_create_server /
// create_client / destroy / connected / poll / snapshot_push / snapshot_pull /
// rpc_send / rpc_poll / set_loss). Sessions do socket I/O (service may block)
// so all entries are plain Cdecl — never SuppressGCTransition.
//
// ReplicatedAttribute marks a component struct for snapshot replication: the
// server serializes its 10 LocalTransform words (px..sz) per tick with delta
// compression against the last-sent baseline; the client applies deltas onto
// its per-entity baseline and reads FULL 10-word rows. RPCs ride the reliable
// channel FIFO per sender. All types blittable; no managed state on the frame
// path (mirrors the native NetSession layout contract).
using System.Runtime.InteropServices;
using Engine.Bindings;

namespace Engine.Runtime;

/// <summary>
/// Marks a component struct as network-replicated: the server serializes its
/// 10 LocalTransform words per tick (delta-compressed); the client applies
/// deltas and reads full rows. Blittable 10-word payload (px..sz).
/// </summary>
[AttributeUsage(AttributeTargets.Struct | AttributeTargets.Class, AllowMultiple = false, Inherited = false)]
public sealed class ReplicatedAttribute : Attribute
{
}

/// <summary>
/// Managed owner of one network session (server or client). Owns the native
/// session lifetime; destroy is idempotent. TableOverride allows tests to pin
/// an absolute-path library handle instead of ApiLoader.
/// </summary>
public sealed unsafe class NetworkSession : IDisposable
{
    // Test seam: absolute-path library handle instead of ApiLoader.
#pragma warning disable CA2211
    public static IntPtr TableOverride = IntPtr.Zero;
#pragma warning restore CA2211

    private static unsafe NativeApi.ApiTable* TablePtr =>
        TableOverride != IntPtr.Zero ? (NativeApi.ApiTable*)TableOverride : NativeApi.ApiLoader.Table;

    private NativeApi.EngineNetSession* _session;
    private bool _disposed;

    private NetworkSession(NativeApi.EngineNetSession* session)
    {
        _session = session;
    }

    /// <summary>Creates a server session bound to the loopback port. Null on failure (bad port 0, init/host failure, double-create).</summary>
    public static NetworkSession? CreateServer(ushort port)
    {
        var s = TablePtr->EngineNetCreateServer(port);
        return s == null ? null : new NetworkSession(s);
    }

    /// <summary>Creates a client session connecting to 127.0.0.1:port. Completes async: poll until Connected.</summary>
    public static NetworkSession? CreateClient(ushort port)
    {
        var s = TablePtr->EngineNetCreateClient(port);
        return s == null ? null : new NetworkSession(s);
    }

    /// <summary>True once a peer is linked (server: >=1 client; client: server peer CONNECTED).</summary>
    public bool Connected => _session != null && TablePtr->EngineNetConnected(_session) != 0;

    /// <summary>Pumps ENet service. Returns false on null session.</summary>
    public bool Poll(uint timeoutMs = 0)
    {
        if (_session == null)
        {
            return false;
        }

        return TablePtr->EngineNetPoll(_session, timeoutMs) == 0;
    }

    /// <summary>
    /// Server side: serializes one tick for count rows (forceFull on tick 0 /
    /// after NAK resync). Returns rows queued. Zero rows on no-change
    /// (mask-all-clear skip). Uses spans — zero managed alloc.
    /// </summary>
    public uint SnapshotPush(ReadOnlySpan<NativeApi.NetTransformRow> rows, bool forceFull)
    {
        if (_session == null || rows.IsEmpty)
        {
            return 0;
        }

        fixed (NativeApi.NetTransformRow* p = rows)
        {
            return TablePtr->EngineNetSnapshotPush(_session, p, (uint)rows.Length, forceFull ? 1 : 0);
        }
    }

    /// <summary>
    /// Client side: drains queued snapshots into out (capacity rows, FULL
    /// 10-word values). Returns rows written.
    /// </summary>
    public uint SnapshotPull(Span<NativeApi.NetTransformRow> outRows)
    {
        if (_session == null || outRows.IsEmpty)
        {
            return 0;
        }

        fixed (NativeApi.NetTransformRow* p = outRows)
        {
            return TablePtr->EngineNetSnapshotPull(_session, p, (uint)outRows.Length);
        }
    }

    /// <summary>Sends one reliable RPC (size &lt;= 1024). Returns true when queued. Loss injection does NOT apply.</summary>
    public bool RpcSend(uint id, ReadOnlySpan<byte> data)
    {
        if (_session == null || data.Length > 1024)
        {
            return false;
        }

        fixed (byte* p = data)
        {
            return TablePtr->EngineNetRpcSend(_session, id, p, (uint)data.Length) == 0;
        }
    }

    /// <summary>
    /// Drains one queued RPC: returns bytes written (&gt;0), 0 when none
    /// pending, negative errno-style on bad args (-1 null args, -2 caller
    /// buffer too small; entry kept on -2).
    /// </summary>
    public int RpcPoll(out uint id, Span<byte> outBuf)
    {
        id = 0;
        if (_session == null)
        {
            return -1;
        }

        uint rid = 0;
        int n;
        fixed (byte* p = outBuf)
        {
            n = TablePtr->EngineNetRpcPoll(_session, &rid, p, (uint)outBuf.Length);
        }

        if (n > 0)
        {
            id = rid;
        }

        return n;
    }

    /// <summary>Uniform send-side drop probability [0,1] applied to snapshot sends only (RPC/control stay reliable).</summary>
    public bool SetLoss(float probability, uint seed)
    {
        if (_session == null)
        {
            return false;
        }

        return TablePtr->EngineNetSetLoss(_session, probability, seed) == 0;
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        if (_session != null)
        {
            TablePtr->EngineNetDestroy(_session);
            _session = null;
        }

        GC.SuppressFinalize(this);
    }

    ~NetworkSession()
    {
        if (_session != null)
        {
            TablePtr->EngineNetDestroy(_session);
            _session = null;
        }
    }
}

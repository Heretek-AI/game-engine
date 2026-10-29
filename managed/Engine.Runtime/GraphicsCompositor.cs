// GraphicsCompositor.cs — M3-03: C# RenderPass registration over the ABI bridge.
//
// Wraps the ABI 3 renderpass_* tail (EngineRenderPassRegistry): create,
// add images, register custom compute/post passes with managed callbacks,
// compile, run frames. Custom passes slot between the built-in PBR passes
// (vis -> shade -> [customs] -> post).
//
// Callback shape: [UnmanagedCallersOnly(CallConvs = [CallConvCdecl])]
// static int Method(uint passIndex, void* userData). The GCHandle to the
// delegate target rides user_data so instance state is reachable without
// allocation on the frame path. Return 0 = ok; nonzero marks the pass
// failed (isolated — the frame continues).
//
// Zero Warn compliance: no LINQ/closures in hot paths; UTF-8 names via
// fixed byte* buffers (bounded 64B, same rule as native valid_image_ref).
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Engine.Bindings;

namespace Engine.Runtime;

/// <summary>Signature of a custom render-pass callback (matches the ABI).</summary>
#pragma warning disable CA1051 // Visible instance field by design: blittable interop struct (Velocity.cs precedent).
public unsafe struct RenderPassCallback
{
    public delegate* unmanaged[Cdecl]<uint, void*, int> Fn;
}
#pragma warning restore CA1051

/// <summary>
/// Managed owner of a native EngineRenderPassRegistry: registers custom
/// render passes and drives per-frame execution. Dispose destroys the
/// native registry (idempotent).
/// </summary>
public sealed unsafe class GraphicsCompositor : IDisposable
{
    private NativeApi.EngineRenderPassRegistry* _reg;
    private bool _disposed;

    // Keeps managed callback targets alive: GCHandle per pass, freed on
    // Dispose. Bounded (matches kMaxCustomPasses = 32); over-capacity
    // AddPass throws before touching native.
    private readonly List<GCHandle> _handles = new();

    // Test/live-probe injection: when set, the compositor drives this table
    // instead of ApiLoader.Table (lets the probe use an absolute-path
    // NativeLibrary handle; ApiLoader only knows the bare SONAME).
    // Test/live-probe injection (public setter by design: lets the ctest
    // live probe drive an absolute-path library handle).
#pragma warning disable CA2211
    public static IntPtr TableOverride = IntPtr.Zero;
#pragma warning restore CA2211
#pragma warning disable CS0649 // Assigned by tests/live probe via reflection-safe field.
    private static unsafe NativeApi.ApiTable* TablePtr =>
        TableOverride != IntPtr.Zero ? (NativeApi.ApiTable*)TableOverride : NativeApi.ApiLoader.Table;
#pragma warning restore CS0649

    public GraphicsCompositor()
    {
        var table = TablePtr;
        var reg = table->EngineRenderPassCreate();
        if (reg == null)
        {
            throw new InvalidOperationException("renderpass_create returned null.");
        }

        _reg = reg;
    }

    private int _nativePassCount;
    public int CustomPassCount => _nativePassCount;

    public void AddImage(string name, ulong width, ulong height, bool transientImage = true)
    {
        ThrowIfDisposed();
        ArgumentException.ThrowIfNullOrEmpty(name);
        if (width == 0 || height == 0)
        {
            throw new ArgumentOutOfRangeException(nameof(width), "image extent must be non-zero.");
        }

        Span<byte> utf8 = stackalloc byte[64];
        var bytes = System.Text.Encoding.UTF8.GetBytes(name, utf8);
        if (bytes == 0 || bytes >= 64)
        {
            throw new ArgumentException("image name must be 1..63 UTF-8 bytes.", nameof(name));
        }

        utf8[bytes] = 0;
        int rc;
        fixed (byte* p = utf8)
        {
            rc = TablePtr->EngineRenderPassAddImage(_reg, p, width, height, transientImage ? 1 : 0);
        }

        if (rc != 0)
        {
            throw new InvalidOperationException($"renderpass_add_image({name}) failed: {rc}.");
        }
    }

    /// <summary>
    /// Registers one custom pass. <paramref name="callback"/> is a raw
    /// function pointer to an [UnmanagedCallersOnly] static method;
    /// <paramref name="state"/> (optional) is a GCHandle target reachable
    /// via user_data.
    /// </summary>
    public void AddPass(string name, string[] reads, string[] writes, delegate* unmanaged[Cdecl]<uint, void*, int> callback, object? state = null)
    {
        ThrowIfDisposed();
        ArgumentException.ThrowIfNullOrEmpty(name);
        ArgumentNullException.ThrowIfNull(reads);
        ArgumentNullException.ThrowIfNull(writes);
        if (callback == null)
        {
            throw new ArgumentNullException(nameof(callback));
        }

        if (_handles.Count >= 32)
        {
            throw new InvalidOperationException("too many custom passes (max 32).");
        }

        GCHandle handle = default;
        var hasHandle = false;
        void* ctx = null;
        if (state != null)
        {
            handle = GCHandle.Alloc(state, GCHandleType.Normal);
            hasHandle = true;
            ctx = (void*)GCHandle.ToIntPtr(handle);
        }

        try
        {
            var rc = AddPassNative(name, reads, writes, callback, ctx);
            if (rc != 0)
            {
                // No local Free here: the catch block below owns cleanup, so
                // the original native error code always reaches the caller
                // (double-Free would replace it with its own throw).
                throw new InvalidOperationException($"renderpass_add_pass({name}) failed: {rc}.");
            }

            if (hasHandle)
            {
                _handles.Add(handle);
            }

            _nativePassCount++;
        }
        catch
        {
            if (hasHandle && !_handles.Contains(handle))
            {
                handle.Free();
            }

            throw;
        }
    }

    public void Compile()
    {
        ThrowIfDisposed();
        var rc = TablePtr->EngineRenderPassCompile(_reg);
        if (rc != 0)
        {
            throw new InvalidOperationException($"renderpass_compile failed: {rc}.");
        }
    }

    public void RunFrame()
    {
        ThrowIfDisposed();
        var rc = TablePtr->EngineRenderPassRunFrame(_reg);
        if (rc != 0)
        {
            throw new InvalidOperationException($"renderpass_run_frame failed: {rc}.");
        }
    }

    public int GetPassResult(int index)
    {
        ThrowIfDisposed();
        return TablePtr->EngineRenderPassPassResult(_reg, index);
    }

    public int FailedCount
    {
        get
        {
            ThrowIfDisposed();
            return TablePtr->EngineRenderPassFailedCount(_reg);
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        if (_reg != null)
        {
            TablePtr->EngineRenderPassDestroy(_reg);
            _reg = null;
        }

        foreach (var h in _handles)
        {
            if (h.IsAllocated)
            {
                h.Free();
            }
        }

        _handles.Clear();
    }

    private unsafe int AddPassNative(string name, string[] reads, string[] writes, delegate* unmanaged[Cdecl]<uint, void*, int> callback, void* ctx)
    {
        // (nameBuf removed: the pass name lives in arena slot 0 now.)

        // Marshal string arrays as NUL-terminated UTF-8 pointer vectors
        // (bounded 16 each, matching kMaxPassAccesses; native copies what
        // it keeps). Pinning: one stackalloc arena holds ALL name bytes
        // (pass name + reads + writes); a single fixed block pins the whole
        // arena for the native call, so every pointer handed to native stays
        // pinned for the call duration (the prior per-buffer fixed blocks
        // unpinned each buffer at its closing brace).
        const int maxAccesses = 16;
        if (reads.Length > maxAccesses || writes.Length > maxAccesses)
        {
            throw new ArgumentException($"pass accesses exceed {maxAccesses}.");
        }

        var totalNames = reads.Length + writes.Length;
        Span<byte> arena = stackalloc byte[64 + (totalNames * 64)];
        Span<byte> nameSpan = arena.Slice(0, 64);
        var nameBytes = System.Text.Encoding.UTF8.GetBytes(name, nameSpan);
        if (nameBytes == 0 || nameBytes >= 64)
        {
            throw new ArgumentException("pass name must be 1..63 UTF-8 bytes.", nameof(name));
        }

        nameSpan[nameBytes] = 0;

        Span<nint> ptrs = stackalloc nint[totalNames > 0 ? totalNames : 1];
        for (var i = 0; i < reads.Length; i++)
        {
            var slot = arena.Slice(64 + (i * 64), 64);
            var n = System.Text.Encoding.UTF8.GetBytes(reads[i], slot);
            if (n == 0 || n >= 64)
            {
                throw new ArgumentException($"read image name must be 1..63 UTF-8 bytes (index {i}).");
            }

            slot[n] = 0;
        }

        for (var i = 0; i < writes.Length; i++)
        {
            var slot = arena.Slice(64 + ((reads.Length + i) * 64), 64);
            var n = System.Text.Encoding.UTF8.GetBytes(writes[i], slot);
            if (n == 0 || n >= 64)
            {
                throw new ArgumentException($"write image name must be 1..63 UTF-8 bytes (index {i}).");
            }

            slot[n] = 0;
        }

        int rc;
        fixed (byte* arenaPtr = arena)
        fixed (nint* ptrsPtr = ptrs)
        {
            for (var i = 0; i < totalNames; i++)
            {
                ptrsPtr[i] = (nint)(arenaPtr + 64 + (i * 64));
            }

            byte** rp = reads.Length == 0 ? null : (byte**)ptrsPtr;
            byte** wp = writes.Length == 0 ? null : (byte**)(ptrsPtr + reads.Length);
            rc = TablePtr->EngineRenderPassAddPass(
                _reg, arenaPtr,
                rp, reads.Length,
                wp, writes.Length,
                callback, ctx);
        }

        return rc;
    }

    private void ThrowIfDisposed([CallerMemberName] string? caller = null)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
    }
}

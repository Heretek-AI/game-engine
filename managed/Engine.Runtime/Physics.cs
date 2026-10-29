// Physics.cs — M4-03: zero-cost C# spatial queries (Raycast / SphereCast /
// OverlapSphere / RaycastBatch).
//
// Wraps the ABI 4 physics_* tail (EnginePhysicsWorld = caller-owned
// JPH::PhysicsSystem pointer): each entry is one raw `call` instruction
// (plain Cdecl — queries take per-body locks / join worker lanes, so no
// SuppressGCTransition, same rule as run_systems). All types blittable;
// RaycastBatch pins caller spans (zero managed alloc on the frame path).
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Engine.Bindings;

namespace Engine.Runtime;

/// <summary>Single ray: origin + direction*length. 24 bytes, blittable.</summary>
[StructLayout(LayoutKind.Sequential, Pack = 4, Size = 24)]
public struct Ray
{
    public float Ox;
    public float Oy;
    public float Oz;
    public float Dx;
    public float Dy;
    public float Dz;

    public Ray(float ox, float oy, float oz, float dx, float dy, float dz)
    {
        Ox = ox; Oy = oy; Oz = oz; Dx = dx; Dy = dy; Dz = dz;
    }
}

/// <summary>Query hit. 48 bytes, blittable. Hit == 1 on hit, 0 on miss.</summary>
[StructLayout(LayoutKind.Sequential, Pack = 8, Size = 48)]
public struct RaycastHit
{
    public float Px;
    public float Py;
    public float Pz;
    public float Fraction;
    public ulong UserData;
    public float Nx;
    public float Ny;
    public float Nz;
    public uint BodyId;
    public uint Hit;

    public readonly bool IsHit => Hit != 0;
}

/// <summary>
/// Managed owner of a physics-query session: wraps the caller-owned native
/// physics world pointer (JPH::PhysicsSystem from the M4-01 PhysicsWorld)
/// and exposes Raycast / SphereCast / OverlapSphere / RaycastBatch.
/// The world lifetime stays with the caller; this handle never frees it.
/// </summary>
public sealed unsafe class Physics : IDisposable
{
    private NativeApi.EnginePhysicsWorld* _world;
    private bool _disposed;

    // Test/live-probe injection (same pattern as GraphicsCompositor):
    // drive an absolute-path library handle instead of ApiLoader.
#pragma warning disable CA2211
    public static IntPtr TableOverride = IntPtr.Zero;
#pragma warning restore CA2211
#pragma warning disable CS0649
    private static unsafe NativeApi.ApiTable* TablePtr =>
        TableOverride != IntPtr.Zero ? (NativeApi.ApiTable*)TableOverride : NativeApi.ApiLoader.Table;
#pragma warning restore CS0649

    /// <summary>Binds an existing native physics world (non-null).</summary>
    public Physics(IntPtr physicsWorld)
    {
        if (physicsWorld == IntPtr.Zero)
        {
            throw new ArgumentNullException(nameof(physicsWorld));
        }

        _world = (NativeApi.EnginePhysicsWorld*)physicsWorld;
    }

    private void ThrowIfDisposed()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
    }

    public void Dispose()
    {
        _disposed = true;
        _world = null;
    }

    /// <summary>Single raycast: closest hit (true) or miss (false, hit.Hit = 0).</summary>
    public bool Raycast(in Ray ray, out RaycastHit hit)
    {
        ThrowIfDisposed();
        var table = TablePtr;
        var r = ray;
        var h = default(NativeApi.RaycastHit);
        int rc = table->EnginePhysicsRaycast(
            _world, (NativeApi.Ray*)&r, &h);
        hit = *(RaycastHit*)&h;
        return rc != 0;
    }

    /// <summary>Sphere cast: sphere of radius swept along the ray.</summary>
    public bool SphereCast(in Ray ray, float radius, out RaycastHit hit)
    {
        ThrowIfDisposed();
        if (!(radius > 0.0f))
        {
            throw new ArgumentOutOfRangeException(nameof(radius), "radius must be positive.");
        }

        var table = TablePtr;
        var r = ray;
        var h = default(NativeApi.RaycastHit);
        int rc = table->EnginePhysicsSphereCast(
            _world, (NativeApi.Ray*)&r, radius, &h);
        hit = *(RaycastHit*)&h;
        return rc != 0;
    }

    /// <summary>
    /// Sphere overlap: bodies intersecting the sphere. Writes up to
    /// bodyIds.Length entries, returns bodies written. userData (optional,
    /// same length) receives per-body userdata (Flecs entity).
    /// </summary>
    public uint OverlapSphere(float cx, float cy, float cz, float radius,
        Span<uint> bodyIds, Span<ulong> userData = default)
    {
        ThrowIfDisposed();
        if (!(radius > 0.0f))
        {
            throw new ArgumentOutOfRangeException(nameof(radius), "radius must be positive.");
        }

        if (bodyIds.IsEmpty)
        {
            return 0;
        }

        var table = TablePtr;
        fixed (uint* ids = bodyIds)
        {
            if (userData.IsEmpty)
            {
                return table->EnginePhysicsOverlapSphere(
                    _world, cx, cy, cz, radius, ids, null, (uint)bodyIds.Length);
            }

            if (userData.Length < bodyIds.Length)
            {
                throw new ArgumentException("userData must be empty or at least bodyIds.Length.", nameof(userData));
            }

            fixed (ulong* ud = userData)
            {
                return table->EnginePhysicsOverlapSphere(
                    _world, cx, cy, cz, radius, ids, ud, (uint)bodyIds.Length);
            }
        }
    }

    /// <summary>
    /// Parallel batch: rays.Length raycasts -> hits.Length hits (lengths must
    /// match). Executes across native worker lanes; zero managed alloc.
    /// Returns rays processed.
    /// </summary>
    public uint RaycastBatch(ReadOnlySpan<Ray> rays, Span<RaycastHit> hits)
    {
        ThrowIfDisposed();
        if (rays.Length != hits.Length)
        {
            throw new ArgumentException("rays and hits spans must have equal length.");
        }

        if (rays.IsEmpty)
        {
            return 0;
        }

        var table = TablePtr;
        fixed (Ray* rp = rays)
        {
            fixed (RaycastHit* hp = hits)
            {
                return table->EnginePhysicsRaycastBatch(
                    _world, (NativeApi.Ray*)rp, (NativeApi.RaycastHit*)hp,
                    (uint)rays.Length);
            }
        }
    }
}

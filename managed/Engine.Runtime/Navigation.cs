// Navigation.cs — P6-03: managed NavAgent over the ABI 4.5 navigation tail.
//
// Wraps an EngineNavWorld (owns one native engine::navigation::NavWorld:
// baked dtNavMesh + dtNavMeshQuery + dtCrowd). Bake is init-time; the per-
// frame path is agent_update (crowd step, plain Cdecl — it advances the
// crowd, never SuppressGCTransition, same rule as run_systems).
//
// NavAgent is the C# component struct: blittable 64B (gx,gy,gz |
// max_speed | stop_radius | arrived | idle | pad). SetDestination sets the
// goal + clears arrival; the native crowd drives the entity's position
// (read via Position). Layout mirror of EngineNavAgent — AbiLayoutTests
// pins it.
using System.Runtime.InteropServices;
using Engine.Bindings;

namespace Engine.Runtime;

/// <summary>Managed NavAgent component. Blittable 64-byte struct.</summary>
[StructLayout(LayoutKind.Sequential, Pack = 4, Size = 64)]
public struct NavAgent
{
    public float Gx;
    public float Gy;
    public float Gz;
    public float MaxSpeed;
    public float StopRadius;
    public int Arrived;
    public int Idle;
#pragma warning disable CA1051 // Blittable interop struct by design.
#pragma warning disable CA1724 // Type name NavAgent mirrors the native component.
    private unsafe fixed float _pad[9];
#pragma warning restore CA1724
#pragma warning restore CA1051

    /// <summary>
    /// Sets the move destination: writes the goal, clears arrival, marks
    /// the agent active. Mirrors the native agent_goto call.
    /// </summary>
    public void SetDestination(float x, float y, float z)
    {
        Gx = x;
        Gy = y;
        Gz = z;
        Arrived = 0;
        Idle = 0;
    }
}

/// <summary>
/// Managed owner of a navmesh session: bakes once from static box
/// obstacles, then adds ONE crowd agent, sets its destination, steps it
/// per frame, and reads its position. The handle is the native NavWorld
/// pointer (borrowed — destroy this only after the world).
/// </summary>
public sealed unsafe class Navigation : IDisposable
{
    private NativeApi.EngineNavWorld* _world;
    private bool _disposed;

    // Test/live-probe injection (same pattern as Physics): drive an
    // absolute-path library handle instead of ApiLoader.
#pragma warning disable CA2211
    public static IntPtr TableOverride = IntPtr.Zero;
#pragma warning restore CA2211
#pragma warning disable CS0649
    private static unsafe NativeApi.ApiTable* TablePtr =>
        TableOverride != IntPtr.Zero ? (NativeApi.ApiTable*)TableOverride : NativeApi.ApiLoader.Table;
#pragma warning restore CS0649

    /// <summary>Creates a native nav world. Throws on allocation failure.</summary>
    public Navigation()
    {
        _world = TablePtr->EngineNavCreate();
        if (_world == null)
        {
            throw new InvalidOperationException("nav_create failed (allocation failure).");
        }
    }

    private void ThrowIfDisposed()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
    }

    public void Dispose()
    {
        if (!_disposed)
        {
            _disposed = true;
            if (_world != null)
            {
                TablePtr->EngineNavDestroy(_world);
                _world = null;
            }
        }
    }

    /// <summary>
    /// Bakes a navmesh from static box obstacles. Returns poly/detail
    /// counts. Throws when the bake fails.
    /// </summary>
    public NativeApi.NavStats Bake(
        ReadOnlySpan<NativeApi.NavBox> obstacles,
        NativeApi.NavBakeConfig config)
    {
        ThrowIfDisposed();
        var table = TablePtr;
        var stats = default(NativeApi.NavStats);
        int rc;
        fixed (NativeApi.NavBox* boxes = obstacles)
        {
            rc = table->EngineNavBake(
                _world, boxes, (uint)obstacles.Length, &config, &stats);
        }

        if (rc == 0)
        {
            throw new InvalidOperationException("nav_bake failed (null args or Recast bake failure).");
        }

        return stats;
    }

    /// <summary>
    /// Adds the (single) crowd agent at (x,y,z). Replaces any previous
    /// agent. Returns the agent id. Throws on failure.
    /// </summary>
    public int AddAgent(float x, float y, float z, float maxSpeed)
    {
        ThrowIfDisposed();
        var id = TablePtr->EngineNavAgentAdd(_world, x, y, z, maxSpeed);
        if (id < 0)
        {
            throw new InvalidOperationException("nav_agent_add failed (no mesh or allocation failure).");
        }

        return id;
    }

    /// <summary>
    /// Requests the agent move to (x,y,z). Returns true when the target
    /// was accepted (nearest-poly found on both ends). False otherwise.
    /// </summary>
    public bool TrySetDestination(float x, float y, float z)
    {
        ThrowIfDisposed();
        return TablePtr->EngineNavAgentGoto(_world, x, y, z) != 0;
    }

    /// <summary>Steps the crowd by dt seconds. Returns false on bad args.</summary>
    public bool Update(float dt)
    {
        ThrowIfDisposed();
        return TablePtr->EngineNavAgentUpdate(_world, dt) != 0;
    }

    /// <summary>Reads the agent position. Returns false when unavailable.</summary>
    public bool TryGetPosition(out float x, out float y, out float z)
    {
        ThrowIfDisposed();
        var pos = stackalloc float[3];
        var ok = TablePtr->EngineNavAgentPos(_world, pos) != 0;
        x = pos[0];
        y = pos[1];
        z = pos[2];
        return ok;
    }

    /// <summary>
    /// True when the agent is within stopRadius of its goal AND its
    /// velocity is near zero.
    /// </summary>
    public bool HasReached(float stopRadius)
    {
        ThrowIfDisposed();
        return TablePtr->EngineNavAgentReached(_world, stopRadius) != 0;
    }
}

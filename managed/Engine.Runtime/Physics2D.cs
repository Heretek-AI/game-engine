// Physics2D.cs — P6-07: C# 2D physics over Box2D v3 (ABI 4.3 tail).
//
// Wraps the ABI 4.9 physics2d_* entries: the native side owns a
// caller-owned engine::physics::World2D + its CollisionEventRing (the
// same 32B record as the 3D path, P2-05 shape parity). Drain hands the
// caller a Span<CollisionEvent2D> over zero managed alloc on the frame
// path (plain Cdecl — step/drain may block, never SuppressGCTransition,
// same rule as run_systems).
//
// Wire types come straight from Engine.Bindings (Rigidbody2D,
// BoxCollider2D, CircleCollider2D, PolygonCollider2D, CollisionEvent2D):
// the descriptors ARE the ABI format, so no Runtime-side duplication.
using Engine.Bindings;

namespace Engine.Runtime;

/// <summary>
/// Managed owner of a native 2D physics world (Box2D v3). Create with
/// gravity, spawn bodies with an entity id carried as body user-data
/// (event mapping), Step the world, then Drain contact events.
/// </summary>
public sealed unsafe class Physics2D : IDisposable
{
    private NativeApi.EnginePhysics2DWorld* _world;
    private bool _disposed;
    private bool _owns;

    // Test/live-probe injection (same pattern as GraphicsCompositor).
#pragma warning disable CA2211
    public static IntPtr TableOverride = IntPtr.Zero;
#pragma warning restore CA2211
#pragma warning disable CS0649
    private static unsafe NativeApi.ApiTable* TablePtr =>
        TableOverride != IntPtr.Zero ? (NativeApi.ApiTable*)TableOverride : NativeApi.ApiLoader.Table;
#pragma warning restore CS0649

    /// <summary>Creates a native 2D world (subSteps 0 => native default 4).</summary>
    public Physics2D(float gravityX, float gravityY, uint subSteps = 4)
    {
        var table = TablePtr;
        var world = table->EnginePhysics2DCreate(gravityX, gravityY, subSteps);
        if (world == null)
        {
            throw new InvalidOperationException("Native 2D physics world creation failed.");
        }

        _world = world;
        _owns = true;
    }

    /// <summary>Binds an existing native 2D world (non-null, borrowed).</summary>
    public Physics2D(IntPtr physics2DWorld)
    {
        if (physics2DWorld == IntPtr.Zero)
        {
            throw new ArgumentNullException(nameof(physics2DWorld));
        }

        _world = (NativeApi.EnginePhysics2DWorld*)physics2DWorld;
    }

    private void ThrowIfDisposed()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
    }

    public void Dispose()
    {
        // NOTE: no TablePtr touch here — the borrowed-handle ctor
        // (IntPtr, used by tests) never loaded the native library, and
        // Dispose must stay a pure state flip when the world was never
        // created through the table. Only the creating ctor owns a
        // destroy call, tracked by _owns.
        _disposed = true;
        if (_owns && _world != null)
        {
            TablePtr->EnginePhysics2DDestroy(_world);
        }

        _world = null;
        _owns = false;
    }

    /// <summary>Spawns a box body; entity is carried as body user-data. Returns the body token (0 on failure).</summary>
    public ulong BodyBox(ulong entity, float px, float py, float angle, in NativeApi.Rigidbody2D rb, in NativeApi.BoxCollider2D box)
    {
        ThrowIfDisposed();
        if (entity == 0)
        {
            throw new ArgumentOutOfRangeException(nameof(entity), "entity must be non-zero.");
        }

        fixed (NativeApi.Rigidbody2D* rbp = &rb)
        {
            fixed (NativeApi.BoxCollider2D* boxp = &box)
            {
                return TablePtr->EnginePhysics2DBodyBox(_world, entity, px, py, angle, rbp, boxp);
            }
        }
    }

    /// <summary>Spawns a circle body. Returns the body token (0 on failure).</summary>
    public ulong BodyCircle(ulong entity, float px, float py, float angle, in NativeApi.Rigidbody2D rb, in NativeApi.CircleCollider2D circle)
    {
        ThrowIfDisposed();
        if (entity == 0)
        {
            throw new ArgumentOutOfRangeException(nameof(entity), "entity must be non-zero.");
        }

        fixed (NativeApi.Rigidbody2D* rbp = &rb)
        {
            fixed (NativeApi.CircleCollider2D* cp = &circle)
            {
                return TablePtr->EnginePhysics2DBodyCircle(_world, entity, px, py, angle, rbp, cp);
            }
        }
    }

    /// <summary>Spawns a convex-polygon body (count in 1..6). Returns the body token (0 on failure).</summary>
    public ulong BodyPolygon(ulong entity, float px, float py, float angle, in NativeApi.Rigidbody2D rb, in NativeApi.PolygonCollider2D poly)
    {
        ThrowIfDisposed();
        if (entity == 0)
        {
            throw new ArgumentOutOfRangeException(nameof(entity), "entity must be non-zero.");
        }

        fixed (NativeApi.Rigidbody2D* rbp = &rb)
        {
            fixed (NativeApi.PolygonCollider2D* pp = &poly)
            {
                return TablePtr->EnginePhysics2DBodyPolygon(_world, entity, px, py, angle, rbp, pp);
            }
        }
    }

    /// <summary>Destroys a body token (0 is ignored).</summary>
    public void BodyDestroy(ulong body)
    {
        ThrowIfDisposed();
        if (body == 0)
        {
            return;
        }

        TablePtr->EnginePhysics2DBodyDestroy(_world, body);
    }

    /// <summary>Fixed-step update. Returns bodies simulated.</summary>
    public uint Step(float dt)
    {
        ThrowIfDisposed();
        if (!(dt > 0.0f))
        {
            throw new ArgumentOutOfRangeException(nameof(dt), "dt must be positive.");
        }

        return TablePtr->EnginePhysics2DStep(_world, dt);
    }

    /// <summary>
    /// Drains pending contact events into events (kEnter/kExit, sensor
    /// flagged). Returns events written; empty span returns 0.
    /// </summary>
    public uint Drain(Span<NativeApi.CollisionEvent2D> events)
    {
        ThrowIfDisposed();
        if (events.IsEmpty)
        {
            return 0;
        }

        fixed (NativeApi.CollisionEvent2D* ep = events)
        {
            return TablePtr->EnginePhysics2DDrain(_world, ep, (uint)events.Length);
        }
    }
}

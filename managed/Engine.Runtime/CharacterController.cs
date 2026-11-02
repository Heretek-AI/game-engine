// CharacterController.cs — P6-04: managed CharacterController over the ABI
// 4.12 character-controller tail (Jolt CharacterVirtual).
//
// Wraps one native engine::physics::CharacterBody bound to a caller-owned
// M4-01 PhysicsWorld (the world + its scheduler/arena outlive this —
// destroy this BEFORE the world, same borrow model as Physics). Creation
// is init-time (shape build); Move() is the frame path (ExtendedUpdate,
// plain Cdecl — never SuppressGCTransition, same rule as run_systems).
//
// CharacterConfig: capsule + limits, 24 bytes (blittable mirror of
// EngineCharacterConfig — AbiLayoutTests pins it). IComponent-friendly:
// the config/state structs are plain data; the controller class owns the
// native handle lifetime.
using System.Numerics;
using System.Runtime.InteropServices;
using Engine.Bindings;

namespace Engine.Runtime;

/// <summary>
/// Managed owner of one native character: binds a caller-owned physics
/// world, builds the capsule at (x,y,z), steps it with Move, teleports it,
/// updates slope/step limits live, and reads the feet position.
/// The native handle is owned (destroy frees it); the world is borrowed.
/// </summary>
public sealed unsafe class CharacterController : IDisposable
{
    private NativeApi.EngineCharacter* _character;
    private bool _disposed;

    // Test/live-probe injection (same pattern as Navigation/Physics):
    // drive an absolute-path library handle instead of ApiLoader.
#pragma warning disable CA2211
    public static IntPtr TableOverride = IntPtr.Zero;
#pragma warning restore CA2211
#pragma warning disable CS0649
    private static unsafe NativeApi.ApiTable* TablePtr =>
        TableOverride != IntPtr.Zero ? (NativeApi.ApiTable*)TableOverride : NativeApi.ApiLoader.Table;
#pragma warning restore CS0649

    /// <summary>
    /// Creates a character bound to <paramref name="physicsWorld"/>
    /// (borrowed: must outlive this) with <paramref name="config"/> at
    /// (x,y,z). Throws on allocation failure or bad args.
    /// </summary>
    public CharacterController(IntPtr physicsWorld, NativeApi.CharacterConfig config, float x, float y, float z)
    {
        if (physicsWorld == IntPtr.Zero)
        {
            throw new ArgumentNullException(nameof(physicsWorld));
        }

        var world = (NativeApi.EnginePhysicsWorld*)physicsWorld;
        _character = TablePtr->EngineCharacterCreate(world, &config, x, y, z);
        if (_character == null)
        {
            throw new InvalidOperationException("character_create failed (null world or bad capsule args).");
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
            if (_character != null)
            {
                TablePtr->EngineCharacterDestroy(_character);
                _character = null;
            }
        }
    }

    /// <summary>
    /// Advances one fixed step with planar velocity (dx,dz in m/s) over
    /// <paramref name="dt"/> seconds (&gt; 0). Returns the post-move state
    /// (feet position, grounded flag, support height, vertical velocity).
    /// Throws on bad args.
    /// </summary>
    public NativeApi.CharacterState Move(float dx, float dz, float dt)
    {
        ThrowIfDisposed();
        var state = default(NativeApi.CharacterState);
        var rc = TablePtr->EngineCharacterMove(_character, dx, dz, dt, &state);
        if (rc == 0)
        {
            throw new InvalidOperationException("character_move failed (null handle or bad dt).");
        }

        return state;
    }

    /// <summary>
    /// Advances one fixed step with a planar velocity vector (x,z in m/s,
    /// y ignored) over <paramref name="dt"/> seconds (&gt; 0). Returns the
    /// post-move state. Throws on bad args.
    /// </summary>
    public NativeApi.CharacterState Move(Vector3 velocity, float dt)
    {
        return Move(velocity.X, velocity.Z, dt);
    }

    /// <summary>
    /// Teleports the character to (x,y,z) (velocity reset). Throws on
    /// failure.
    /// </summary>
    public void Teleport(float x, float y, float z)
    {
        ThrowIfDisposed();
        if (TablePtr->EngineCharacterTeleport(_character, x, y, z) == 0)
        {
            throw new InvalidOperationException("character_teleport failed (null handle).");
        }
    }

    /// <summary>
    /// Live slope-limit update (degrees; &lt;= 0 restores the 45-degree
    /// default). Throws on failure.
    /// </summary>
    public void SetSlopeLimit(float maxSlopeDeg)
    {
        ThrowIfDisposed();
        if (TablePtr->EngineCharacterSetSlope(_character, maxSlopeDeg) == 0)
        {
            throw new InvalidOperationException("character_set_slope failed (null handle).");
        }
    }

    /// <summary>
    /// Live step-height update (meters; negative clamps to 0, disabling
    /// stairs). Throws on failure.
    /// </summary>
    public void SetStepHeight(float maxStepHeight)
    {
        ThrowIfDisposed();
        if (TablePtr->EngineCharacterSetStep(_character, maxStepHeight) == 0)
        {
            throw new InvalidOperationException("character_set_step failed (null handle).");
        }
    }

    /// <summary>
    /// Reads the feet position without stepping. Throws on failure.
    /// </summary>
    public NativeApi.CharacterState GetPosition()
    {
        ThrowIfDisposed();
        var state = default(NativeApi.CharacterState);
        if (TablePtr->EngineCharacterGetPos(_character, &state) == 0)
        {
            throw new InvalidOperationException("character_get_pos failed (null handle).");
        }

        return state;
    }

    /// <summary>Whether the last move left the character on walkable ground.</summary>
    public bool IsGrounded
    {
        get
        {
            var state = GetPosition();
            return state.Grounded != 0;
        }
    }
}

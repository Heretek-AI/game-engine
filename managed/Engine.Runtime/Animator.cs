// Animator.cs — P5-03: managed animator over the ABI 4.23 animator_* tail.
//
// Animator owns an EngineAnimator (native-owned per-entity state parsed
// from the P5-03 JSON state-machine graph): parameter writes go through
// animator_set_float/set_bool/set_trigger (UTF-8 NUL names, fail-closed),
// Update advances clip times + cross-fade by dt, and ActiveState reads
// back the active state name via animator_state_name. All entries are
// plain Cdecl (update may join worker lanes — never
// SuppressGCTransition, same rule as run_systems).
using System.Runtime.InteropServices;
using System.Text;
using Engine.Bindings;

namespace Engine.Runtime;

/// <summary>
/// Managed owner of a native animator: parameter writes (float/bool/
/// trigger), explicit-dt updates, and active-state readback. The handle
/// is the native EngineAnimator pointer (borrowed world inside — destroy
/// this before the world).
/// </summary>
public sealed unsafe class Animator : IDisposable
{
    private NativeApi.EngineAnimator* _animator;
    private bool _disposed;

    // Test/live-probe injection (same pattern as Audio/Ui/Character).
#pragma warning disable CA2211
    public static IntPtr TableOverride = IntPtr.Zero;
#pragma warning restore CA2211
#pragma warning disable CS0649
    private static unsafe NativeApi.ApiTable* TablePtr =>
        TableOverride != IntPtr.Zero ? (NativeApi.ApiTable*)TableOverride : NativeApi.ApiLoader.Table;
#pragma warning restore CS0649

    /// <summary>
    /// Creates an animator bound to `world` for `entityId` from the
    /// P5-03 JSON state-machine graph (NUL-terminated UTF-8).
    /// </summary>
    public Animator(IntPtr world, ulong entityId, string graphJson)
    {
        ArgumentNullException.ThrowIfNull(graphJson);
        var jsonBytes = Encoding.UTF8.GetBytes(graphJson + "\0");
        NativeApi.EngineAnimator* animator;
        fixed (byte* jsonPtr = jsonBytes)
        {
            animator = TablePtr->EngineAnimatorCreate((NativeApi.EngineWorld*)world, entityId, jsonPtr);
        }

        if (animator == null)
        {
            throw new InvalidOperationException("animator_create failed (null world/graph, bad JSON, or allocation failure).");
        }

        _animator = animator;
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
            if (_animator != null)
            {
                TablePtr->EngineAnimatorDestroy(_animator);
                _animator = null;
            }
        }
    }

    /// <summary>Sets a float parameter by name. Unknown names fail closed.</summary>
    public void SetFloat(string name, float value)
    {
        ThrowIfDisposed();
        ArgumentNullException.ThrowIfNull(name);
        var nameBytes = Encoding.UTF8.GetBytes(name + "\0");
        int rc;
        fixed (byte* namePtr = nameBytes)
        {
            rc = TablePtr->EngineAnimatorSetFloat(_animator, namePtr, value);
        }

        if (rc == 0)
        {
            throw new InvalidOperationException($"animator_set_float failed for '{name}'.");
        }
    }

    /// <summary>Sets a bool parameter by name. Unknown names fail closed.</summary>
    public void SetBool(string name, bool value)
    {
        ThrowIfDisposed();
        ArgumentNullException.ThrowIfNull(name);
        var nameBytes = Encoding.UTF8.GetBytes(name + "\0");
        int rc;
        fixed (byte* namePtr = nameBytes)
        {
            rc = TablePtr->EngineAnimatorSetBool(_animator, namePtr, value ? 1 : 0);
        }

        if (rc == 0)
        {
            throw new InvalidOperationException($"animator_set_bool failed for '{name}'.");
        }
    }

    /// <summary>Fires a trigger parameter by name. Unknown names fail closed.</summary>
    public void SetTrigger(string name)
    {
        ThrowIfDisposed();
        ArgumentNullException.ThrowIfNull(name);
        var nameBytes = Encoding.UTF8.GetBytes(name + "\0");
        int rc;
        fixed (byte* namePtr = nameBytes)
        {
            rc = TablePtr->EngineAnimatorSetTrigger(_animator, namePtr);
        }

        if (rc == 0)
        {
            throw new InvalidOperationException($"animator_set_trigger failed for '{name}'.");
        }
    }

    /// <summary>Advances clip times + cross-fade by `dt` seconds.</summary>
    public void Update(float dt)
    {
        ThrowIfDisposed();
        if (TablePtr->EngineAnimatorUpdate(_animator, dt) == 0)
        {
            throw new InvalidOperationException("animator_update failed.");
        }
    }

    /// <summary>Reads back the active state name (acceptance-test readback).</summary>
    public string ActiveState
    {
        get
        {
            ThrowIfDisposed();
            Span<byte> outBytes = stackalloc byte[256];
            int n;
            fixed (byte* outPtr = outBytes)
            {
                n = TablePtr->EngineAnimatorStateName(_animator, outPtr, outBytes.Length);
            }

            if (n < 0)
            {
                throw new InvalidOperationException("animator_state_name failed.");
            }

            return Encoding.UTF8.GetString(outBytes[..n]);
        }
    }
}

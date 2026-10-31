// Components.cs — HelloCube scripts. The generator emits Register/RunChunk
// for Spinner (2-term: self Spinner + the entity's own LocalTransform row).
//
// global::LocalTransform / global::WorldTransform are HelloCube-local
// mirrors of the native built-in rows. They live in the GLOBAL namespace
// so their FullNames ("LocalTransform", "WorldTransform") match the
// built-ins EXACTLY (a namespaced mirror such as "HelloCube.LocalTransform"
// would fork a second component — gotcha 20: the lookup is name-exact).
// Size (64) and alignment (64, pinned by the single Vector512 field) match
// too, so Ecs.ComponentId<T>() resolves the EXISTING native ids through the
// idempotent RegisterRenderComponent lookup. EngineChunkColumns flows
// columns by size, so the runner's spans overlay the native memory and Tick
// writes rotate the real entity rows.
//
// Lane map (both mirrors): lanes 0..15 are the row's 16 floats. For
// LocalTransform: lanes 0-2 pos, 3-6 quat, 7-9 scale. For WorldTransform:
// lanes 0-15 are row-major m[0..15].
using System.Runtime.InteropServices;
using System.Runtime.Intrinsics;
using Engine.Runtime;

/// <summary>64B/align-64 mirror of the native LocalTransform row (global name binds the built-in).</summary>
[StructLayout(LayoutKind.Sequential)]
public struct LocalTransform : IComponent
{
    /// <summary>Raw 64-byte row (lanes 0-15 = the row's 16 floats).</summary>
#pragma warning disable CA1051 // blittable interop struct by design
    public Vector512<float> Vec;
#pragma warning restore CA1051

    /// <summary>Position X (lane 0).</summary>
    public float Px { readonly get => Vec.GetElement(0); set => Vec = Vec.WithElement(0, value); }
    /// <summary>Position Y (lane 1).</summary>
    public float Py { readonly get => Vec.GetElement(1); set => Vec = Vec.WithElement(1, value); }
    /// <summary>Position Z (lane 2).</summary>
    public float Pz { readonly get => Vec.GetElement(2); set => Vec = Vec.WithElement(2, value); }
    /// <summary>Quaternion X (lane 3).</summary>
    public float Qx { readonly get => Vec.GetElement(3); set => Vec = Vec.WithElement(3, value); }
    /// <summary>Quaternion Y (lane 4).</summary>
    public float Qy { readonly get => Vec.GetElement(4); set => Vec = Vec.WithElement(4, value); }
    /// <summary>Quaternion Z (lane 5).</summary>
    public float Qz { readonly get => Vec.GetElement(5); set => Vec = Vec.WithElement(5, value); }
    /// <summary>Quaternion W (lane 6).</summary>
    public float Qw { readonly get => Vec.GetElement(6); set => Vec = Vec.WithElement(6, value); }
    /// <summary>Scale X (lane 7).</summary>
    public float Sx { readonly get => Vec.GetElement(7); set => Vec = Vec.WithElement(7, value); }
    /// <summary>Scale Y (lane 8).</summary>
    public float Sy { readonly get => Vec.GetElement(8); set => Vec = Vec.WithElement(8, value); }
    /// <summary>Scale Z (lane 9).</summary>
    public float Sz { readonly get => Vec.GetElement(9); set => Vec = Vec.WithElement(9, value); }
}

/// <summary>64B/align-64 mirror of the native WorldTransform row (global name binds the built-in).</summary>
[StructLayout(LayoutKind.Sequential)]
public struct WorldTransform : IComponent
{
    /// <summary>Raw 64-byte row (lanes 0-15 = row-major m[0..15]).</summary>
#pragma warning disable CA1051 // blittable interop struct by design
    public Vector512<float> Vec;
#pragma warning restore CA1051

    /// <summary>Row-major matrix lane access.</summary>
    public float this[int i]
    {
        readonly get => Vec.GetElement(i);
        set => Vec = Vec.WithElement(i, value);
    }
}

namespace HelloCube
{

/// <summary>
/// Spins its entity around Y. The entity carries the native LocalTransform
/// row; the runner hands that column as the second term and Tick writes
/// the rotated quaternion back into shared column memory.
/// </summary>
public partial struct Spinner : IComponent
{
    /// <summary>Radians per second of yaw.</summary>
    public float Speed;

    /// <summary>Per-row update: yaw the entity's own LocalTransform.</summary>
    public readonly void Tick(ref Spinner self, ref global::LocalTransform local, float dt)
    {
        // World-frame yaw: q' = qyaw(half-angle) * q.
        var half = self.Speed * dt * 0.5f;
        var s = MathF.Sin(half);
        var c = MathF.Cos(half);
        var qx = local.Qx;
        var qy = local.Qy;
        var qz = local.Qz;
        var qw = local.Qw;
        local.Qx = qx * c + qz * s;
        local.Qy = qy * c + qw * s;
        local.Qz = qz * c - qx * s;
        local.Qw = qw * c - qy * s;
    }
}
}

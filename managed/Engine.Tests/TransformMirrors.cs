// TransformMirrors.cs — P1-10: global-namespace mirrors of the native
// built-in transform rows for tests that seed render scenes.
//
// global::LocalTransform / global::WorldTransform live in the GLOBAL
// namespace so their FullNames ("LocalTransform", "WorldTransform") match
// the native built-ins EXACTLY (same rule as samples/HelloCube/
// Components.cs: a namespaced mirror forks a second Flecs component and
// world_propagate + the draw query silently see nothing). Size (64) and
// alignment (64, pinned by the single Vector512 field) match too, so
// Ecs.ComponentId<T>() resolves the EXISTING native ids idempotently.
//
// Lane map (both mirrors): lanes 0..15 are the row's 16 floats. For
// LocalTransform: lanes 0-2 pos, 3-6 quat, 7-9 scale. For WorldTransform:
// lanes 0-15 are row-major m[0..15].
using System.Runtime.InteropServices;
using System.Runtime.Intrinsics;
using Engine.Runtime;

#pragma warning disable CA1050 // Global namespace by design: FullName must equal the native built-in name exactly (see header).
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

    /// <summary>Identity (unit quat, unit scale) at the given position.</summary>
    public static LocalTransform IdentityAt(float x, float y, float z)
    {
        var t = new LocalTransform();
        t.Px = x;
        t.Py = y;
        t.Pz = z;
        t.Qx = 0;
        t.Qy = 0;
        t.Qz = 0;
        t.Qw = 1;
        t.Sx = 1;
        t.Sy = 1;
        t.Sz = 1;
        return t;
    }
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

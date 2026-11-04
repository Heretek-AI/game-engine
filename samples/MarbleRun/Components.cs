// Components.cs — MarbleRun scripts.
//
// global::LocalTransform / global::WorldTransform are mirrors of the
// native built-in rows. They live in the GLOBAL namespace so their
// FullNames ("LocalTransform", "WorldTransform") match the built-ins
// EXACTLY (a namespaced mirror would fork a second component — gotcha 20).
// Size (64) and alignment (64, pinned by the single Vector512 field) match
// too, so Ecs.ComponentId<T>() resolves the EXISTING native ids.
//
// Lane map: lanes 0..15 are the row's 16 floats. For LocalTransform:
// lanes 0-2 pos, 3-6 quat, 7-9 scale. For WorldTransform: lanes 0-15 are
// row-major m[0..15].
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

namespace MarbleRun
{
    /// <summary>
    /// Per-frame host job: scripted-drive blend + respawn + goal drain.
    /// Runs under world_frame (Phase.Update) when the player owns the
    /// loop. Multi-term Tick (self + Marble + LocalTransform on the
    /// marble's own row — same shape as MarbleController): everything
    /// happens in-column, with zero Ecs.Get/Set calls (entity access
    /// inside systems_run faults the runner silent). The standalone
    /// Program.cs host loop keeps its own inline Drive (same math) for
    /// the selftest/visual paths, where it runs outside systems_run.
    /// </summary>
    [UpdateInPhase(Phase.Update)]
    public partial struct FrameHost : IComponent
    {
        /// <summary>Per-row update: drive blend, respawn, goal drain.</summary>
        public readonly void Tick(ref FrameHost self, ref Marble marble, ref LocalTransform local, float dt)
        {
            _ = self;
            _ = dt;

            // Drive blend (mirrors the standalone host loop): scripted
            // course vector + live axes into MarbleInput for the
            // FixedUpdate consumer. Statics only — thread-safe reads.
            var hx = Input.GetAxis("Horizontal");
            var vz = Input.GetAxis("Vertical");
            MarbleGame.DriveFor(local.Px, local.Pz, out var dx, out var dz);
            dx += hx;
            dz -= vz;
            MarbleInput.X = dx;
            MarbleInput.Z = dz;

            // Respawn in-column (clock keeps running).
            if (local.Py < MarbleGame.RespawnY)
            {
                var start = MarbleGame.StartPose();
                local.Px = start.Px;
                local.Py = start.Py;
                local.Pz = start.Pz;
                marble.VelY = 0.0f;
            }

            // Goal drain: real sensor kEnter through the collision ring.
            Game.Bootstrap.DrainGoal(marble.Elapsed);
        }
    }

    /// <summary>
    /// Per-frame drive vector consumed by <see cref="MarbleController"/>.
    /// Written by the host loop (scripted demo drive blended with the live
    /// Input axes); read on physics worker threads, so plain fields behind
    /// properties (CA2211 does not apply to properties).
    /// </summary>
    public static class MarbleInput
    {
        /// <summary>Desired drive direction X (need not be normalised).</summary>
        public static float X { get; set; } = 1.0f;

        /// <summary>Desired drive direction Z (need not be normalised).</summary>
        public static float Z { get; set; }
    }

    /// <summary>
    /// The player sphere: kinematic body steered by pose integration.
    /// 16 bytes, align 4. Blittable: whole-struct Ecs.Set/Get compatible.
    /// </summary>
    public partial struct Marble : IComponent
    {
        /// <summary>Planar speed in m/s.</summary>
        public float Speed;

        /// <summary>Vertical velocity in m/s (scripted gravity, reset on landing).</summary>
        public float VelY;

        /// <summary>Sim seconds accumulated (finish line reads this).</summary>
        public float Elapsed;

        /// <summary>Nonzero once the goal sensor has fired.</summary>
        public int Finished;

        /// <summary>
        /// Per-row fixed-step update: drive along the host-provided vector,
        /// apply scripted gravity, snap to the course top, fall through gaps.
        /// No allocation, no throws (frame path).
        /// </summary>
        public readonly void Tick(ref Marble self, ref LocalTransform local, float dt)
        {
            var dx = MarbleInput.X;
            var dz = MarbleInput.Z;
            var len = MathF.Sqrt(dx * dx + dz * dz);
            if (len > 1.0f)
            {
                dx /= len;
                dz /= len;
            }

            local.Px += dx * self.Speed * dt;
            local.Pz += dz * self.Speed * dt;

            self.VelY -= 25.0f * dt;
            var y = local.Py + self.VelY * dt;
            if (MarbleGame.IsGround(local.Px, local.Pz) && y <= MarbleGame.RestY)
            {
                y = MarbleGame.RestY;
                self.VelY = 0.0f;
            }

            local.Py = y;
            self.Elapsed += dt;
        }
    }

    /// <summary>
    /// Fixed-step controller binding: runs <see cref="Marble.Tick"/> once
    /// per 1/50 s fixed step through the world clock.
    /// </summary>
    [UpdateInPhase(Phase.FixedUpdate)]
    public partial struct MarbleController : IComponent
    {
        /// <summary>Per-row fixed-step update (delegates to the drive logic).</summary>
        public readonly void Tick(ref MarbleController self, ref Marble marble, ref LocalTransform local, float dt)
        {
            _ = self;
            marble.Tick(ref marble, ref local, dt);
        }
    }
}

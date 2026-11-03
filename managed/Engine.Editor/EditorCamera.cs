// EditorCamera.cs — P3-02: Scene viewport editor camera (managed, CPU-only).
//
// Pure System.Numerics fly/dolly/frame math — no Avalonia, no GPU, no
// native. The Scene viewport owns one EditorCamera; input events (RMB +
// WASD/QE fly, wheel dolly, F frame) drive these methods, and the CPU
// rasterizer consumes ViewProjection. Right-handed, Y-up; camera looks
// along -Z in view space. Angles: Yaw about world +Y (0 = looking toward
// -Z, positive turns right toward +X); Pitch about camera-local +X
// (positive looks up), clamped to ~+/-89 deg.
//
// ViewProjection is row-major float[16], Vulkan-style: Y-flipped, depth
// 0..1 (matches the P1-07 camera contract: (2,2,3) -> origin, fovY 60,
// near 0.1, far 100). NDC->pixels: sx = (ndcX*0.5+0.5)*W,
// sy = (1-(ndcY*0.5+0.5))*H (the 1- absorbs the Vulkan Y-flip).
using System.Numerics;

namespace Engine.Editor;

/// <summary>
/// Scene viewport camera: fly (RMB+WASD/QE), dolly (wheel), frame (F).
/// </summary>
public sealed class EditorCamera
{
    private const float PitchLimit = 1.553f; // ~89 deg

    /// <summary>World-space position.</summary>
    public Vector3 Position { get; set; }

    /// <summary>Yaw radians about world +Y (0 = looking toward -Z).</summary>
    public float Yaw { get; set; }

    /// <summary>Pitch radians about camera-local +X (positive looks up).</summary>
    public float Pitch { get; set; }

    /// <summary>Fly speed, world units per second.</summary>
    public float Speed { get; set; } = 3.0f;

    /// <summary>Vertical field of view, radians.</summary>
    public float FovYRadians { get; set; } = MathF.PI / 3.0f; // 60 deg

    /// <summary>Near plane distance.</summary>
    public float Near { get; set; } = 0.1f;

    /// <summary>Far plane distance.</summary>
    public float Far { get; set; } = 100.0f;

    public EditorCamera()
    {
    }

    public EditorCamera(Vector3 position, float yaw, float pitch)
    {
        Position = position;
        Yaw = yaw;
        Pitch = Math.Clamp(pitch, -PitchLimit, PitchLimit);
    }

    /// <summary>Default P1-07 pose: (2,2,3) looking at the origin.</summary>
    public static EditorCamera WithDefault()
    {
        var position = new Vector3(2, 2, 3);
        var dir = Vector3.Normalize(-position); // origin - position
        // Forward = (-sinYaw*cosP, sinP, -cosYaw*cosP) => yaw = atan2(-dx, -dz).
        var yaw = MathF.Atan2(-dir.X, -dir.Z);
        var pitch = MathF.Asin(Math.Clamp(dir.Y, -1.0f, 1.0f));
        return new EditorCamera(position, yaw, pitch);
    }

    /// <summary>Forward unit vector (world space).</summary>
    public Vector3 Forward
    {
        get
        {
            var cp = MathF.Cos(Pitch);
            return Vector3.Normalize(new Vector3(
                -MathF.Sin(Yaw) * cp,
                MathF.Sin(Pitch),
                -MathF.Cos(Yaw) * cp));
        }
    }

    /// <summary>Right unit vector (world space).</summary>
    public Vector3 Right => Vector3.Normalize(Vector3.Cross(Forward, Vector3.UnitY));

    /// <summary>Up unit vector (world space).</summary>
    public Vector3 Up => Vector3.Cross(Right, Forward);

    /// <summary>
    /// Fly step: move along Forward/Right/Up by Speed*dt per axis.
    /// One call = one tick (RMB held + keys down); dt = 1/60 in tests.
    /// </summary>
    public void Fly(bool fwd, bool back, bool left, bool right, bool up, bool down, float dt)
    {
        var step = Speed * dt;
        if (fwd)
        {
            Position += Forward * step;
        }

        if (back)
        {
            Position -= Forward * step;
        }

        if (right)
        {
            Position += Right * step;
        }

        if (left)
        {
            Position -= Right * step;
        }

        if (up)
        {
            Position += Vector3.UnitY * step;
        }

        if (down)
        {
            Position -= Vector3.UnitY * step;
        }
    }

    /// <summary>Scroll dolly: positive notches move toward the target.</summary>
    /// <param name="wheelNotches">Wheel ticks (positive = forward).</param>
    /// <param name="distance">Distance to dolly target (origin when unknown).</param>
    public void Dolly(float wheelNotches, float distance)
    {
        Position += Forward * (wheelNotches * 0.15f * MathF.Max(distance, 0.001f));
    }

    /// <summary>
    /// Frame the selection: keep yaw/pitch, place the camera so the
    /// centroid of <paramref name="points"/> is centered at a fit distance.
    /// Empty set frames the origin.
    /// </summary>
    public void FrameTo(IReadOnlyList<Vector3> points)
    {
        var centroid = Vector3.Zero;
        if (points.Count > 0)
        {
            foreach (var p in points)
            {
                centroid += p;
            }

            centroid /= points.Count;
        }

        var extent = 1.0f;
        if (points.Count > 1)
        {
            var min = points[0];
            var max = points[0];
            foreach (var p in points)
            {
                min = Vector3.Min(min, p);
                max = Vector3.Max(max, p);
            }

            var spread = max - min;
            extent = MathF.Max(extent, MathF.Max(spread.X, MathF.Max(spread.Y, spread.Z)));
        }

        var fitDist = Math.Clamp(extent * 2.0f + 2.0f, 2.0f, 20.0f);
        Position = centroid - (Forward * fitDist);
    }

    /// <summary>Distance from the camera to a world point (fly assertions).</summary>
    public float DistanceTo(Vector3 p) => (p - Position).Length();

    /// <summary>
    /// Row-major ViewProjection for System.Numerics column convention:
    /// clip = VP * v, NDC->pixels sx=(ndcX*0.5+0.5)*W,
    /// sy=(1-(ndcY*0.5+0.5))*H (the 1- absorbs the Vulkan Y-flip).
    /// Vulkan clip (Y-flipped vs OpenGL, depth 0..1), right-handed view.
    /// </summary>
    public float[] GetViewProjection(int width, int height)
    {
        var aspect = (float)width / Math.Max(height, 1);
        var view = Matrix4x4.CreateLookAt(Position, Position + Forward, Vector3.UnitY);
        // Framework RH perspective (OpenGL clip, Y-up, depth -1..1);
        // negate M22 for the Vulkan Y-flip, remap depth in Project().
        var proj = Matrix4x4.CreatePerspectiveFieldOfView(FovYRadians, aspect, Near, Far);
        proj.M22 = -proj.M22;
        var vp = Matrix4x4.Multiply(view, proj);
        return
        [
            vp.M11, vp.M12, vp.M13, vp.M14,
            vp.M21, vp.M22, vp.M23, vp.M24,
            vp.M31, vp.M32, vp.M33, vp.M34,
            vp.M41, vp.M42, vp.M43, vp.M44,
        ];
    }
}

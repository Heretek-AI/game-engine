// EditorCamera.cs — P3-02/P3-06: editor viewport camera (pure math, headless).
//
// Fly/Dolly/Frame over a rigid eye (position + yaw/pitch), with a Vulkan-clip
// ViewProjection for the offscreen renderer (render_to_buffer takes the
// explicit column-major matrix; pick reuses the same projection through
// the world's main camera). No Avalonia dependency: the library stays
// headless-safe; the UI binds keys/buttons to these methods.
using System.Numerics;

namespace Engine.Editor;

/// <summary>
/// Editor viewport camera: rigid eye (position + yaw/pitch), Vulkan-clip
/// projection (Y-flipped, depth 0..1, matching the native ColPerspective).
/// Angles in degrees; yaw around +Y, pitch around local +X.
/// </summary>
public sealed class EditorCamera
{
    private Vector3 _position;
    private float _yawDeg;
    private float _pitchDeg;

    /// <summary>Vertical field of view in radians.</summary>
    public float FovYRad { get; set; } = 60f * MathF.PI / 180f;

    /// <summary>Near plane distance (must be &gt; 0).</summary>
    public float NearZ { get; set; } = 0.1f;

    /// <summary>Far plane distance (must exceed <see cref="NearZ"/>).</summary>
    public float FarZ { get; set; } = 100f;

    public EditorCamera()
        : this(new Vector3(2, 2, 3), 0, 0)
    {
    }

    public EditorCamera(Vector3 position, float yawDeg, float pitchDeg)
    {
        _position = position;
        _yawDeg = yawDeg;
        _pitchDeg = pitchDeg;
    }

    /// <summary>Aim the eye at <paramref name="target"/> (yaw/pitch derived).</summary>
    public static EditorCamera LookAt(Vector3 eye, Vector3 target)
    {
        var d = Vector3.Normalize(target - eye);
        var yaw = MathF.Atan2(-d.X, -d.Z) * 180f / MathF.PI;
        var pitch = MathF.Asin(Math.Clamp(d.Y, -1f, 1f)) * 180f / MathF.PI;
        return new EditorCamera(eye, yaw, pitch);
    }

    /// <summary>Aim this eye at <paramref name="target"/> (yaw/pitch derived).</summary>
    public void AimAt(Vector3 target)
    {
        var d = Vector3.Normalize(target - _position);
        _yawDeg = MathF.Atan2(-d.X, -d.Z) * 180f / MathF.PI;
        _pitchDeg = MathF.Asin(Math.Clamp(d.Y, -1f, 1f)) * 180f / MathF.PI;
    }

    /// <summary>Eye position.</summary>
    public Vector3 Position => _position;

    /// <summary>Yaw in degrees (around +Y).</summary>
    public float YawDeg => _yawDeg;

    /// <summary>Pitch in degrees (around local +X).</summary>
    public float PitchDeg => _pitchDeg;

    /// <summary>Move in the camera's local frame (x right, y up, +z forward along the view ray).</summary>
    public void Fly(Vector3 localMove, Vector2 lookDeltaDeg)
    {
        _yawDeg += lookDeltaDeg.X;
        _pitchDeg = Math.Clamp(_pitchDeg + lookDeltaDeg.Y, -89f, 89f);
        var (forward, right, up) = Basis();
        _position += right * localMove.X + up * localMove.Y + forward * localMove.Z;
    }

    /// <summary>Move along the view ray: positive moves toward the target (zoom in), negative pulls back.</summary>
    public void Dolly(float amount)
    {
        var (forward, _, _) = Basis();
        _position += forward * amount;
    }

    /// <summary>Place the eye so <paramref name="center"/> frames at <paramref name="radius"/>.</summary>
    public void Frame(Vector3 center, float radius)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(radius);
        var (forward, _, _) = Basis();
        var dist = radius / MathF.Tan(FovYRad * 0.5f);
        _position = center - forward * dist;
    }

    /// <summary>Forward vector (unit).</summary>
    public Vector3 Forward => Basis().Forward;

    /// <summary>
    /// Column-major view-projection (16 floats) for
    /// <c>renderer_render_to_buffer</c>: Vulkan clip (Y-flipped, depth 0..1).
    /// </summary>
    public float[] ViewProjection(float aspect)
    {
        ArgumentOutOfRangeException.ThrowIfNegativeOrZero(aspect);

        var (forward, right, up) = Basis();
        // View rows (s, u, -f) with translation -basis.eye (row-vector
        // lookAt, same convention as the native RowWorldToColView).
        var view = new float[16];
        view[0] = right.X;
        view[1] = up.X;
        view[2] = -forward.X;
        view[3] = 0;
        view[4] = right.Y;
        view[5] = up.Y;
        view[6] = -forward.Y;
        view[7] = 0;
        view[8] = right.Z;
        view[9] = up.Z;
        view[10] = -forward.Z;
        view[11] = 0;
        view[12] = -(right.X * _position.X + right.Y * _position.Y + right.Z * _position.Z);
        view[13] = -(up.X * _position.X + up.Y * _position.Y + up.Z * _position.Z);
        view[14] = forward.X * _position.X + forward.Y * _position.Y + forward.Z * _position.Z;
        view[15] = 1;
        // Vulkan perspective: Y-flipped, depth 0..1.
        var f = 1.0f / MathF.Tan(FovYRad * 0.5f);
        var near = NearZ > 0 ? NearZ : 0.1f;
        var far = FarZ > near ? FarZ : 100f;
        var proj = new float[16];
        proj[0] = f / aspect;
        proj[5] = -f;
        proj[10] = far / (near - far);
        proj[11] = -1;
        proj[14] = -(far * near) / (far - near);
        return Mul(proj, view);
    }

    private (Vector3 Forward, Vector3 Right, Vector3 Up) Basis()
    {
        var yaw = _yawDeg * MathF.PI / 180f;
        var pitch = _pitchDeg * MathF.PI / 180f;
        var forward = new Vector3(
            -MathF.Sin(yaw) * MathF.Cos(pitch),
            MathF.Sin(pitch),
            -MathF.Cos(yaw) * MathF.Cos(pitch));
        forward = Vector3.Normalize(forward);
        var right = Vector3.Normalize(Vector3.Cross(forward, Vector3.UnitY));
        if (right.LengthSquared() < 1e-12f)
        {
            right = Vector3.UnitX;
        }

        var up = Vector3.Cross(right, forward);
        return (forward, right, up);
    }

    private static float[] Mul(float[] a, float[] b)
    {
        // Column-major multiply: o = a * b.
        var o = new float[16];
        for (var c = 0; c < 4; c++)
        {
            for (var r = 0; r < 4; r++)
            {
                o[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] +
                    a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
            }
        }

        return o;
    }
}

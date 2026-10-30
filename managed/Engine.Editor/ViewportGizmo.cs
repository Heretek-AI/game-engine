// ViewportGizmo.cs — M5-02: GPU Object ID picking + 3D gizmos (headless).
//
// Splits the AC into the CPU-testable half (this file) and the GPU half
// (documented below; lands with the Vulkan viewport in M5-03/04):
//   * Picking model: the viewport renders entity ids into an R32U object-id
//     attachment (resolve = id at pixel, 0 = background); Pick(x, y) reads
//     back one texel. Here: a CPU ObjectIdBuffer of the same layout so the
//     coordinate math (row-major index, OOB -> 0) is pinned headless.
//   * Gizmo math: Translate/Rotate/Scale modes over a 10-float Local pose.
//     DragTo(dragWorld, axis) projects the pointer ray's world delta onto
//     the active axis (translate), accumulates right/up pixel deltas into a
//     yaw/pitch quaternion (rotate), or scales along the axis from the drag
//     distance ratio (scale). Pure System.Numerics math — no GPU, fully
//     unit-testable, and the exact code the viewport drag handler calls.
using System.Numerics;

namespace Engine.Editor;

/// <summary>Gizmo interaction mode.</summary>
public enum GizmoMode
{
    Translate,
    Rotate,
    Scale,
}

/// <summary>
/// CPU mirror of the viewport's R32U object-id attachment: id per pixel,
/// 0 = background. Row-major; OOB reads return 0.
/// </summary>
public sealed class ObjectIdBuffer
{
    private readonly ulong[] _ids;

    public int Width { get; }
    public int Height { get; }

    public ObjectIdBuffer(int width, int height)
    {
        if (width <= 0 || height <= 0)
        {
            throw new ArgumentOutOfRangeException(width <= 0 ? nameof(width) : nameof(height));
        }

        Width = width;
        Height = height;
        _ids = new ulong[width * height];
    }

    public void Write(int x, int y, ulong id)
    {
        if (x < 0 || y < 0 || x >= Width || y >= Height)
        {
            throw new ArgumentOutOfRangeException($"({x},{y}) outside {Width}x{Height}.");
        }

        _ids[y * Width + x] = id;
    }

    public void Clear() => Array.Clear(_ids);

    /// <summary>Entity id at pixel (0 = background/OOB).</summary>
    public ulong Pick(int x, int y)
    {
        if (x < 0 || y < 0 || x >= Width || y >= Height)
        {
            return 0;
        }

        return _ids[y * Width + x];
    }
}

/// <summary>
/// 3D transform gizmo: drag math over a 10-float Local pose (p/q/s).
/// Axis is the active world-space unit axis (X/Y/Z or a custom vector).
/// </summary>
public sealed class TransformGizmo
{
    private float[]? _dragStart;
    private Vector3 _dragOriginWorld;

    public GizmoMode Mode { get; set; } = GizmoMode.Translate;

    /// <summary>Active axis in world space (normalized on set).</summary>
    public Vector3 Axis { get; set; } = Vector3.UnitX;

    /// <summary>Begin a drag: capture the starting pose + pointer world point.</summary>
    public void BeginDrag(float[] startPose, Vector3 pointerWorld)
    {
        ArgumentNullException.ThrowIfNull(startPose);
        if (startPose.Length != 10)
        {
            throw new ArgumentException("Pose must be exactly 10 floats.", nameof(startPose));
        }

        _dragStart = (float[])startPose.Clone();
        _dragOriginWorld = pointerWorld;
        if (Axis.LengthSquared() > 1e-12f)
        {
            Axis = Vector3.Normalize(Axis);
        }
    }

    /// <summary>
    /// Drag to a new pointer world point: returns the updated 10-float pose.
    /// Translate projects the world delta onto the axis; Rotate accumulates
    /// a yaw (around Y) from dx and pitch (around X) from dy; Scale grows
    /// along the axis from the drag distance ratio.
    /// </summary>
    public float[] DragTo(Vector3 pointerWorld, float pixelDx = 0, float pixelDy = 0)
    {
        if (_dragStart == null)
        {
            throw new InvalidOperationException("BeginDrag must precede DragTo.");
        }

        var pose = (float[])_dragStart.Clone();
        switch (Mode)
        {
            case GizmoMode.Translate:
                {
                    var delta = Vector3.Dot(pointerWorld - _dragOriginWorld, Axis);
                    pose[0] += Axis.X * delta;
                    pose[1] += Axis.Y * delta;
                    pose[2] += Axis.Z * delta;
                    break;
                }

            case GizmoMode.Rotate:
                {
                    // Yaw from horizontal pixels, pitch from vertical pixels
                    // (0.5 deg/px): matches viewport drag sensitivity.
                    const float radPerPx = MathF.PI / 360.0f;
                    var yaw = Quaternion.CreateFromAxisAngle(Vector3.UnitY, pixelDx * radPerPx);
                    var pitch = Quaternion.CreateFromAxisAngle(Vector3.UnitX, -pixelDy * radPerPx);
                    var dq = yaw * pitch;
                    var q = new Quaternion(pose[3], pose[4], pose[5], pose[6]);
                    var rotated = dq * q;
                    pose[3] = rotated.X;
                    pose[4] = rotated.Y;
                    pose[5] = rotated.Z;
                    pose[6] = rotated.W;
                    break;
                }

            case GizmoMode.Scale:
                {
                    var signed = Vector3.Dot(pointerWorld - _dragOriginWorld, Axis);
                    var factor = 1.0f + (signed / 10.0f);
                    pose[7] = MathF.Max(0.001f, _dragStart[7] * (1 + (factor - 1) * MathF.Abs(Axis.X)));
                    pose[8] = MathF.Max(0.001f, _dragStart[8] * (1 + (factor - 1) * MathF.Abs(Axis.Y)));
                    pose[9] = MathF.Max(0.001f, _dragStart[9] * (1 + (factor - 1) * MathF.Abs(Axis.Z)));
                    break;
                }

            default:
                throw new InvalidOperationException($"Unknown gizmo mode {Mode}.");
        }

        return pose;
    }

    public void EndDrag() => _dragStart = null;
}

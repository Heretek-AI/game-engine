// Math.cs — M2-01: SIMD math types mapped to System.Numerics/Intrinsics.
//
// Layout-first wrappers around Vector128<float> lanes so the JIT emits real
// SSE/NEON arithmetic on hot paths instead of scalar x87: float3 (padded to
// 16B), float4, quaternion, and float4x4 (row-major, matches the native
// WorldTransform mirror). All structs are Sequential + blittable, with
// explicit sizes so native columns and managed mirrors stride identically.
//
// Implicit conversions to/from System.Numerics types make interop free:
// float3 <-> Vector3, float4/quaternion <-> Vector4/Quaternion,
// float4x4 <-> Matrix4x4.
using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Intrinsics;

namespace Engine.Runtime;

/// <summary>3-component SIMD vector (XYZ + 12B pad = 16 bytes).</summary>
[StructLayout(LayoutKind.Sequential, Pack = 4, Size = 16)]
public struct float3 : IEquatable<float3>
{
    /// <summary>X lane.</summary>
    public float X;

    /// <summary>Y lane.</summary>
    public float Y;

    /// <summary>Z lane.</summary>
    public float Z;

    /// <summary>Constructs (x, y, z).</summary>
    public float3(float x, float y, float z)
    {
        X = x;
        Y = y;
        Z = z;
    }

    /// <summary>Lane view (W forced to 0).</summary>
    public readonly Vector128<float> Lanes => Vector128.Create(X, Y, Z, 0.0f);

    /// <summary>Squared length (dot with self), via one SIMD lane multiply.</summary>
    public readonly float LengthSquared()
    {
        var v = Lanes;
        return Vector128.Sum(v * v);
    }

    /// <summary>Euclidean length.</summary>
    public readonly float Length() => MathF.Sqrt(LengthSquared());

    /// <summary>Unit vector (zero when degenerate).</summary>
    public readonly float3 Normalized()
    {
        float len = Length();
        return len > 0.0f ? this * (1.0f / len) : this;
    }

    /// <summary>SIMD dot product.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static float Dot(in float3 a, in float3 b)
    {
        return Vector128.Sum(a.Lanes * b.Lanes);
    }

    /// <summary>SIMD cross product.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static float3 Cross(in float3 a, in float3 b) => new(
        (a.Y * b.Z) - (a.Z * b.Y),
        (a.Z * b.X) - (a.X * b.Z),
        (a.X * b.Y) - (a.Y * b.X));

    /// <summary>Component-wise add.</summary>
    public static float3 operator +(in float3 a, in float3 b) => new(a.X + b.X, a.Y + b.Y, a.Z + b.Z);

    /// <summary>Component-wise subtract.</summary>
    public static float3 operator -(in float3 a, in float3 b) => new(a.X - b.X, a.Y - b.Y, a.Z - b.Z);

    /// <summary>Scalar scale.</summary>
    public static float3 operator *(in float3 v, float s) => new(v.X * s, v.Y * s, v.Z * s);

    /// <inheritdoc/>
    public readonly bool Equals(float3 other) => X == other.X && Y == other.Y && Z == other.Z;

    /// <inheritdoc/>
    public readonly override bool Equals(object? obj) => obj is float3 other && Equals(other);

    /// <inheritdoc/>
    public readonly override int GetHashCode() => HashCode.Combine(X, Y, Z);

    /// <inheritdoc/>
    public static bool operator ==(in float3 a, in float3 b) => a.Equals(b);

    /// <inheritdoc/>
    public static bool operator !=(in float3 a, in float3 b) => !a.Equals(b);

    /// <summary>From System.Numerics.</summary>
    public static implicit operator float3(Vector3 v) => new(v.X, v.Y, v.Z);

    /// <summary>To System.Numerics.</summary>
    public static implicit operator Vector3(in float3 v) => new(v.X, v.Y, v.Z);
}

/// <summary>4-component SIMD vector (16 bytes, one SSE register).</summary>
[StructLayout(LayoutKind.Sequential, Pack = 4, Size = 16)]
public struct float4 : IEquatable<float4>
{
    /// <summary>X lane.</summary>
    public float X;

    /// <summary>Y lane.</summary>
    public float Y;

    /// <summary>Z lane.</summary>
    public float Z;

    /// <summary>W lane.</summary>
    public float W;

    /// <summary>Constructs (x, y, z, w).</summary>
    public float4(float x, float y, float z, float w)
    {
        X = x;
        Y = y;
        Z = z;
        W = w;
    }

    /// <summary>Full 4-lane view.</summary>
    public readonly Vector128<float> Lanes => Vector128.Create(X, Y, Z, W);

    /// <summary>From a 4-lane register.</summary>
    public static float4 FromLanes(Vector128<float> v) => new(v.GetElement(0), v.GetElement(1), v.GetElement(2), v.GetElement(3));

    /// <summary>SIMD dot product.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static float Dot(in float4 a, in float4 b)
    {
        return Vector128.Sum(a.Lanes * b.Lanes);
    }

    /// <summary>Component-wise add.</summary>
    public static float4 operator +(in float4 a, in float4 b) => FromLanes(a.Lanes + b.Lanes);

    /// <summary>Component-wise subtract.</summary>
    public static float4 operator -(in float4 a, in float4 b) => FromLanes(a.Lanes - b.Lanes);

    /// <summary>Scalar scale.</summary>
    public static float4 operator *(in float4 v, float s) => FromLanes(v.Lanes * s);

    /// <inheritdoc/>
    public readonly bool Equals(float4 other) => X == other.X && Y == other.Y && Z == other.Z && W == other.W;

    /// <inheritdoc/>
    public readonly override bool Equals(object? obj) => obj is float4 other && Equals(other);

    /// <inheritdoc/>
    public readonly override int GetHashCode() => HashCode.Combine(X, Y, Z, W);

    /// <inheritdoc/>
    public static bool operator ==(in float4 a, in float4 b) => a.Equals(b);

    /// <inheritdoc/>
    public static bool operator !=(in float4 a, in float4 b) => !a.Equals(b);

    /// <summary>From System.Numerics.</summary>
    public static implicit operator float4(Vector4 v) => new(v.X, v.Y, v.Z, v.W);

    /// <summary>To System.Numerics.</summary>
    public static implicit operator Vector4(in float4 v) => new(v.X, v.Y, v.Z, v.W);
}

/// <summary>Unit quaternion (X, Y, Z, W = 16 bytes).</summary>
[StructLayout(LayoutKind.Sequential, Pack = 4, Size = 16)]
#pragma warning disable CS8981 // Lowercase name is the spec'd AC API (float3/float4/quaternion/float4x4).
public struct quaternion : IEquatable<quaternion>
{
    /// <summary>X (imaginary) component.</summary>
    public float X;

    /// <summary>Y (imaginary) component.</summary>
    public float Y;

    /// <summary>Z (imaginary) component.</summary>
    public float Z;

    /// <summary>W (real) component.</summary>
    public float W;

    /// <summary>Constructs (x, y, z, w).</summary>
    public quaternion(float x, float y, float z, float w)
    {
        X = x;
        Y = y;
        Z = z;
        W = w;
    }

    /// <summary>Identity rotation.</summary>
    public static quaternion Identity => new(0.0f, 0.0f, 0.0f, 1.0f);

    /// <summary>Full 4-lane view.</summary>
    public readonly Vector128<float> Lanes => Vector128.Create(X, Y, Z, W);

    /// <summary>Hamilton product.</summary>
    public static quaternion operator *(in quaternion a, in quaternion b) => (quaternion)(a.AsNumerics() * b.AsNumerics());

    /// <summary>Rotates vector <paramref name="v"/> by <paramref name="q"/>.</summary>
    public static float3 Rotate(in quaternion q, in float3 v) => (float3)Vector3.Transform(v, q.AsNumerics());

    private readonly Quaternion AsNumerics() => new(X, Y, Z, W);

    /// <inheritdoc/>
    public readonly bool Equals(quaternion other) => X == other.X && Y == other.Y && Z == other.Z && W == other.W;

    /// <inheritdoc/>
    public readonly override bool Equals(object? obj) => obj is quaternion other && Equals(other);

    /// <inheritdoc/>
    public readonly override int GetHashCode() => HashCode.Combine(X, Y, Z, W);

    /// <inheritdoc/>
    public static bool operator ==(in quaternion a, in quaternion b) => a.Equals(b);

    /// <inheritdoc/>
    public static bool operator !=(in quaternion a, in quaternion b) => !a.Equals(b);

    /// <summary>From System.Numerics.</summary>
    public static implicit operator quaternion(Quaternion q) => new(q.X, q.Y, q.Z, q.W);

    /// <summary>To System.Numerics.</summary>
    public static implicit operator Quaternion(in quaternion q) => new(q.X, q.Y, q.Z, q.W);
}

/// <summary>Row-major 4x4 matrix (16 floats = 64 bytes, matches WorldTransform).</summary>
[StructLayout(LayoutKind.Sequential, Pack = 4, Size = 64)]
public unsafe struct float4x4 : IEquatable<float4x4>
{
    private fixed float _m[16];

    /// <summary>Identity matrix.</summary>
    public static float4x4 Identity
    {
        get
        {
            var m = new float4x4();
            m.M00 = 1.0f;
            m.M11 = 1.0f;
            m.M22 = 1.0f;
            m.M33 = 1.0f;
            return m;
        }
    }

    /// <summary>Row r, column c (0-based, bounds-checked).</summary>
    /// <exception cref="ArgumentOutOfRangeException">Row or column out of range.</exception>
    public float this[int row, int col]
    {
        readonly get
        {
            if ((uint)row > 3U)
            {
                throw new ArgumentOutOfRangeException(nameof(row));
            }

            if ((uint)col > 3U)
            {
                throw new ArgumentOutOfRangeException(nameof(col));
            }

            fixed (float* p = _m)
            {
                return p[(row * 4) + col];
            }
        }

        set
        {
            if ((uint)row > 3U)
            {
                throw new ArgumentOutOfRangeException(nameof(row));
            }

            if ((uint)col > 3U)
            {
                throw new ArgumentOutOfRangeException(nameof(col));
            }

            fixed (float* p = _m)
            {
                p[(row * 4) + col] = value;
            }
        }
    }

    /// <summary>Element aliases (Mij = row i, column j).</summary>
#pragma warning disable CA1051 // Visible instance fields by design: blittable interop struct.
#pragma warning disable IDE1006 // Lowercase Mij naming matches HLSL/GLSL convention.
    public float M00 { readonly get => this[0, 0]; set => this[0, 0] = value; }

    /// <inheritdoc/>
    public float M01 { readonly get => this[0, 1]; set => this[0, 1] = value; }

    /// <inheritdoc/>
    public float M02 { readonly get => this[0, 2]; set => this[0, 2] = value; }

    /// <inheritdoc/>
    public float M03 { readonly get => this[0, 3]; set => this[0, 3] = value; }

    /// <inheritdoc/>
    public float M10 { readonly get => this[1, 0]; set => this[1, 0] = value; }

    /// <inheritdoc/>
    public float M11 { readonly get => this[1, 1]; set => this[1, 1] = value; }

    /// <inheritdoc/>
    public float M12 { readonly get => this[1, 2]; set => this[1, 2] = value; }

    /// <inheritdoc/>
    public float M13 { readonly get => this[1, 3]; set => this[1, 3] = value; }

    /// <inheritdoc/>
    public float M20 { readonly get => this[2, 0]; set => this[2, 0] = value; }

    /// <inheritdoc/>
    public float M21 { readonly get => this[2, 1]; set => this[2, 1] = value; }

    /// <inheritdoc/>
    public float M22 { readonly get => this[2, 2]; set => this[2, 2] = value; }

    /// <inheritdoc/>
    public float M23 { readonly get => this[2, 3]; set => this[2, 3] = value; }

    /// <inheritdoc/>
    public float M30 { readonly get => this[3, 0]; set => this[3, 0] = value; }

    /// <inheritdoc/>
    public float M31 { readonly get => this[3, 1]; set => this[3, 1] = value; }

    /// <inheritdoc/>
    public float M32 { readonly get => this[3, 2]; set => this[3, 2] = value; }

    /// <inheritdoc/>
    public float M33 { readonly get => this[3, 3]; set => this[3, 3] = value; }
#pragma warning restore IDE1006
#pragma warning restore CA1051

    /// <summary>Row r as a SIMD vector.</summary>
    public readonly float4 Row(int r) => new(this[r, 0], this[r, 1], this[r, 2], this[r, 3]);

    /// <summary>Matrix multiply (delegates to System.Numerics).</summary>
    public static float4x4 operator *(in float4x4 a, in float4x4 b) => (float4x4)(a.AsNumerics() * b.AsNumerics());

    /// <summary>Transforms the point <paramref name="p"/> (w = 1).</summary>
    public readonly float3 TransformPoint(in float3 p) => (float3)Vector3.Transform(p, AsNumerics());

    private readonly Matrix4x4 AsNumerics()
    {
        fixed (float* p = _m)
        {
            return new Matrix4x4(
                p[0], p[1], p[2], p[3],
                p[4], p[5], p[6], p[7],
                p[8], p[9], p[10], p[11],
                p[12], p[13], p[14], p[15]);
        }
    }

    /// <inheritdoc/>
    public readonly bool Equals(float4x4 other)
    {
        for (var i = 0; i < 4; i++)
        {
            for (var j = 0; j < 4; j++)
            {
                if (this[i, j] != other[i, j])
                {
                    return false;
                }
            }
        }

        return true;
    }

    /// <inheritdoc/>
    public readonly override bool Equals(object? obj) => obj is float4x4 other && Equals(other);

    /// <inheritdoc/>
    public readonly override int GetHashCode()
    {
        var h = new HashCode();
        for (var i = 0; i < 4; i++)
        {
            for (var j = 0; j < 4; j++)
            {
                h.Add(this[i, j]);
            }
        }

        return h.ToHashCode();
    }

    /// <inheritdoc/>
    public static bool operator ==(in float4x4 a, in float4x4 b) => a.Equals(b);

    /// <inheritdoc/>
    public static bool operator !=(in float4x4 a, in float4x4 b) => !a.Equals(b);

    /// <summary>From System.Numerics.</summary>
    public static explicit operator float4x4(Matrix4x4 m)
    {
        var r = new float4x4();
        r.M00 = m.M11;
        r.M01 = m.M12;
        r.M02 = m.M13;
        r.M03 = m.M14;
        r.M10 = m.M21;
        r.M11 = m.M22;
        r.M12 = m.M23;
        r.M13 = m.M24;
        r.M20 = m.M31;
        r.M21 = m.M32;
        r.M22 = m.M33;
        r.M23 = m.M34;
        r.M30 = m.M41;
        r.M31 = m.M42;
        r.M32 = m.M43;
        r.M33 = m.M44;
        return r;
    }

    /// <summary>To System.Numerics.</summary>
    public static explicit operator Matrix4x4(in float4x4 m) => m.AsNumerics();
}

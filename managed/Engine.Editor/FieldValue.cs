// FieldValue.cs — P3-05: typed field-value codec for the inspector.
//
// Converts between native component bytes (read via entity_get, written
// via entity_set) and the per-type editor values the panel binds:
// f32/i32/u32 scalars, bool8, vec2/3/4, quat (Euler degrees in the UI),
// color4 (RGBA floats), entity (ulong id), asset-ref (ulong cookie).
//
// Pure-managed and fully unit-testable (no native handle needed): every
// method works on caller-supplied spans. The panel and SetFieldCommand
// own the native calls; this type owns the byte math.
using System.Numerics;

namespace Engine.Editor;

/// <summary>Field-type codes (mirror ENGINE_ABI_FIELD_* in engine_abi.h).</summary>
public enum InspectorFieldType : uint
{
    F32 = 0,
    I32 = 1,
    U32 = 2,
    Bool8 = 3,
    Vec2 = 4,
    Vec3 = 5,
    Vec4 = 6,
    Quat = 7,
    Color4 = 8,
    Entity = 9,
    AssetRef = 10,
}

/// <summary>Byte size of one value of each field type.</summary>
public static class FieldValue
{
    public static int ByteSize(InspectorFieldType type) => type switch
    {
        InspectorFieldType.F32 => 4,
        InspectorFieldType.I32 => 4,
        InspectorFieldType.U32 => 4,
        InspectorFieldType.Bool8 => 1,
        InspectorFieldType.Vec2 => 8,
        InspectorFieldType.Vec3 => 12,
        InspectorFieldType.Vec4 => 16,
        InspectorFieldType.Quat => 16,
        InspectorFieldType.Color4 => 16,
        InspectorFieldType.Entity => 8,
        InspectorFieldType.AssetRef => 8,
        _ => throw new ArgumentOutOfRangeException(nameof(type), $"Unknown field type {(uint)type}."),
    };

    /// <summary>Reads one lane (float) of a scalar/vector field at <paramref name="lane"/>.</summary>
    public static float ReadFloat(ReadOnlySpan<byte> componentBytes, int fieldOffset, InspectorFieldType type, int lane = 0)
    {
        var lanes = LaneCount(type);
        if ((uint)lane >= (uint)lanes)
        {
            throw new ArgumentOutOfRangeException(nameof(lane), $"Lane {lane} out of range for {type} ({lanes} lanes).");
        }

        return type switch
        {
            InspectorFieldType.F32 => ReadF32(componentBytes, fieldOffset),
            InspectorFieldType.I32 => ReadI32(componentBytes, fieldOffset),
            InspectorFieldType.U32 => ReadU32(componentBytes, fieldOffset),
            InspectorFieldType.Bool8 => ReadBool(componentBytes, fieldOffset) ? 1.0f : 0.0f,
            InspectorFieldType.Vec2 or InspectorFieldType.Vec3 or InspectorFieldType.Vec4
                or InspectorFieldType.Quat or InspectorFieldType.Color4 => ReadF32(componentBytes, fieldOffset + lane * 4),
            _ => throw new ArgumentOutOfRangeException(nameof(type), $"Type {type} is not float-readable."),
        };
    }

    /// <summary>Writes one lane (float) of a scalar/vector field at <paramref name="lane"/>.</summary>
    public static void WriteFloat(Span<byte> componentBytes, int fieldOffset, InspectorFieldType type, float value, int lane = 0)
    {
        var lanes = LaneCount(type);
        if ((uint)lane >= (uint)lanes)
        {
            throw new ArgumentOutOfRangeException(nameof(lane), $"Lane {lane} out of range for {type} ({lanes} lanes).");
        }

        switch (type)
        {
            case InspectorFieldType.F32:
                WriteF32(componentBytes, fieldOffset, value);
                break;
            case InspectorFieldType.I32:
                WriteI32(componentBytes, fieldOffset, (int)MathF.Round(value));
                break;
            case InspectorFieldType.U32:
                WriteU32(componentBytes, fieldOffset, (uint)MathF.Round(value));
                break;
            case InspectorFieldType.Bool8:
                WriteBool(componentBytes, fieldOffset, value != 0.0f);
                break;
            case InspectorFieldType.Vec2:
            case InspectorFieldType.Vec3:
            case InspectorFieldType.Vec4:
            case InspectorFieldType.Quat:
            case InspectorFieldType.Color4:
                WriteF32(componentBytes, fieldOffset + lane * 4, value);
                break;
            default:
                throw new ArgumentOutOfRangeException(nameof(type), $"Type {type} is not float-writable.");
        }
    }

    /// <summary>Reads a bool8 field.</summary>
    public static bool ReadBool(ReadOnlySpan<byte> componentBytes, int fieldOffset)
    {
        CheckRange(componentBytes.Length, fieldOffset, 1);
        return componentBytes[fieldOffset] != 0;
    }

    /// <summary>Writes a bool8 field (1 byte).</summary>
    public static void WriteBool(Span<byte> componentBytes, int fieldOffset, bool value)
    {
        CheckRange(componentBytes.Length, fieldOffset, 1);
        componentBytes[fieldOffset] = value ? (byte)1 : (byte)0;
    }

    /// <summary>Reads an entity/asset-ref field (ulong cookie).</summary>
    public static ulong ReadId(ReadOnlySpan<byte> componentBytes, int fieldOffset, InspectorFieldType type)
    {
        if (type != InspectorFieldType.Entity && type != InspectorFieldType.AssetRef)
        {
            throw new ArgumentOutOfRangeException(nameof(type), $"Type {type} is not id-readable.");
        }

        CheckRange(componentBytes.Length, fieldOffset, 8);
        return BitConverter.ToUInt64(componentBytes.Slice(fieldOffset, 8));
    }

    /// <summary>Writes an entity/asset-ref field (ulong cookie).</summary>
    public static void WriteId(Span<byte> componentBytes, int fieldOffset, InspectorFieldType type, ulong value)
    {
        if (type != InspectorFieldType.Entity && type != InspectorFieldType.AssetRef)
        {
            throw new ArgumentOutOfRangeException(nameof(type), $"Type {type} is not id-writable.");
        }

        CheckRange(componentBytes.Length, fieldOffset, 8);
        BitConverter.TryWriteBytes(componentBytes.Slice(fieldOffset, 8), value);
    }

    /// <summary>Reads a quat field as Euler degrees (XYZ order, matching the panel).</summary>
    public static Vector3 QuatToEulerDeg(ReadOnlySpan<byte> componentBytes, int fieldOffset)
    {
        CheckRange(componentBytes.Length, fieldOffset, 16);
        var q = new Quaternion(
            BitConverter.ToSingle(componentBytes.Slice(fieldOffset, 4)),
            BitConverter.ToSingle(componentBytes.Slice(fieldOffset + 4, 4)),
            BitConverter.ToSingle(componentBytes.Slice(fieldOffset + 8, 4)),
            BitConverter.ToSingle(componentBytes.Slice(fieldOffset + 12, 4)));
        return QuatToEulerDeg(q);
    }

    /// <summary>Writes a quat field from Euler degrees (XYZ order).</summary>
    public static void EulerDegToQuat(Span<byte> componentBytes, int fieldOffset, Vector3 eulerDeg)
    {
        var q = EulerDegToQuatValue(eulerDeg);
        CheckRange(componentBytes.Length, fieldOffset, 16);
        BitConverter.TryWriteBytes(componentBytes.Slice(fieldOffset, 4), q.X);
        BitConverter.TryWriteBytes(componentBytes.Slice(fieldOffset + 4, 4), q.Y);
        BitConverter.TryWriteBytes(componentBytes.Slice(fieldOffset + 8, 4), q.Z);
        BitConverter.TryWriteBytes(componentBytes.Slice(fieldOffset + 12, 4), q.W);
    }

    /// <summary>Quaternion to Euler degrees (XYZ order).</summary>
    public static Vector3 QuatToEulerDeg(Quaternion q)
    {
        // Inverse of EulerDegToQuatValue (q = qz * qy * qx): ZYX
        // yaw-pitch-roll. R = Rz * Ry * Rx gives m20 = -sin(pitch), so the
        // pitch gimbal sits at |m20| = 1 with yaw = atan2(m10, m00).
        var n = MathF.Sqrt(q.X * q.X + q.Y * q.Y + q.Z * q.Z + q.W * q.W);
        float x = q.X, y = q.Y, z = q.Z, w = q.W;
        if (n > 0.0f)
        {
            x /= n; y /= n; z /= n; w /= n;
        }

        var m00 = 1.0f - 2.0f * (y * y + z * z);
        var m10 = 2.0f * (x * y + z * w);
        var m20 = 2.0f * (x * z - y * w);
        var m21 = 2.0f * (y * z + x * w);
        var m22 = 1.0f - 2.0f * (x * x + y * y);

        float roll, pitch, yaw;
        if (m20 >= 0.9999f)
        {
            pitch = -MathF.PI / 2.0f;
            roll = 0.0f;
            yaw = MathF.Atan2(-m10, m00);
        }
        else if (m20 <= -0.9999f)
        {
            pitch = MathF.PI / 2.0f;
            roll = 0.0f;
            yaw = MathF.Atan2(-m10, m00);
        }
        else
        {
            pitch = MathF.Asin(Math.Clamp(-m20, -1.0f, 1.0f));
            roll = MathF.Atan2(m21, m22);
            yaw = MathF.Atan2(m10, m00);
        }

        const float Rad2Deg = 180.0f / MathF.PI;
        return new Vector3(roll * Rad2Deg, pitch * Rad2Deg, yaw * Rad2Deg);
    }

    /// <summary>Euler degrees (XYZ order) to quaternion.</summary>
    public static Quaternion EulerDegToQuatValue(Vector3 eulerDeg)
    {
        // Matches QuatToEulerDeg below: q = qz * qy * qx (extrinsic X
        // then Y then Z, equivalently ZYX intrinsic / yaw-pitch-roll).
        const float Deg2Rad = MathF.PI / 180.0f;
        var half = eulerDeg * (Deg2Rad * 0.5f);
        var cx = MathF.Cos(half.X); var sx = MathF.Sin(half.X);
        var cy = MathF.Cos(half.Y); var sy = MathF.Sin(half.Y);
        var cz = MathF.Cos(half.Z); var sz = MathF.Sin(half.Z);
        var qx = new Quaternion(sx, 0, 0, cx);
        var qy = new Quaternion(0, sy, 0, cy);
        var qz = new Quaternion(0, 0, sz, cz);
        return qz * qy * qx;
    }

    private static int LaneCount(InspectorFieldType type) => type switch
    {
        InspectorFieldType.F32 or InspectorFieldType.I32 or InspectorFieldType.U32 or InspectorFieldType.Bool8 => 1,
        InspectorFieldType.Vec2 => 2,
        InspectorFieldType.Vec3 => 3,
        InspectorFieldType.Vec4 or InspectorFieldType.Quat or InspectorFieldType.Color4 => 4,
        InspectorFieldType.Entity or InspectorFieldType.AssetRef => 0,
        _ => throw new ArgumentOutOfRangeException(nameof(type), $"Unknown field type {(uint)type}."),
    };

    private static float ReadF32(ReadOnlySpan<byte> bytes, int offset)
    {
        CheckRange(bytes.Length, offset, 4);
        return BitConverter.ToSingle(bytes.Slice(offset, 4));
    }

    private static void WriteF32(Span<byte> bytes, int offset, float value)
    {
        CheckRange(bytes.Length, offset, 4);
        BitConverter.TryWriteBytes(bytes.Slice(offset, 4), value);
    }

    private static float ReadI32(ReadOnlySpan<byte> bytes, int offset)
    {
        CheckRange(bytes.Length, offset, 4);
        return BitConverter.ToInt32(bytes.Slice(offset, 4));
    }

    private static void WriteI32(Span<byte> bytes, int offset, int value)
    {
        CheckRange(bytes.Length, offset, 4);
        BitConverter.TryWriteBytes(bytes.Slice(offset, 4), value);
    }

    private static float ReadU32(ReadOnlySpan<byte> bytes, int offset)
    {
        CheckRange(bytes.Length, offset, 4);
        return BitConverter.ToUInt32(bytes.Slice(offset, 4));
    }

    private static void WriteU32(Span<byte> bytes, int offset, uint value)
    {
        CheckRange(bytes.Length, offset, 4);
        BitConverter.TryWriteBytes(bytes.Slice(offset, 4), value);
    }

    private static void CheckRange(int length, int offset, int size)
    {
        if (offset < 0 || size < 0 || offset + size > length)
        {
            throw new ArgumentOutOfRangeException(nameof(offset), $"Field range [{offset}, {offset + size}) exceeds {length} bytes.");
        }
    }
}

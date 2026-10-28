// FixedString64.cs — M2-01: blittable fixed-capacity UTF-8 string.
//
// 64 payload bytes + 1 length byte = 65 bytes, ASCII/UTF-8, embedded inline
// in IComponent structs (no heap reference, no GC tracking). Overlong input
// is truncated to capacity; embedded NUL bytes are rejected (the byte after
// the payload would read as a false terminator to native code).
//
// Zero-alloc formatting/parsing on the frame path: TryFormat/TryParse use
// stack-only spans; ToString allocates (call it on cold paths only).
using System.Runtime.InteropServices;
using System.Text;

namespace Engine.Runtime;

/// <summary>
/// Fixed-capacity (64-byte) UTF-8 string for components. Blittable, inline,
/// never heap-allocates on the frame path. Longer input truncates; embedded
/// NUL bytes throw <see cref="ArgumentException"/>.
/// </summary>
[StructLayout(LayoutKind.Sequential, Pack = 1, Size = SizeBytes)]
public unsafe struct FixedString64 : IEquatable<FixedString64>
{
    /// <summary>Maximum payload bytes (excludes the length byte).</summary>
    public const int Capacity = 64;

    private const int SizeBytes = Capacity + 1;

    private fixed byte _bytes[Capacity];

    private Span<byte> WritableBytes()
    {
        fixed (byte* p = _bytes)
        {
            return new Span<byte>(p, Capacity);
        }
    }

    /// <summary>Payload length in bytes (0..Capacity).</summary>
#pragma warning disable CA1051 // Visible instance field by design: blittable interop struct (Velocity.cs precedent).
    public byte Length;
#pragma warning restore CA1051

    /// <summary>Payload length in bytes (0..Capacity).</summary>
    public readonly int Count => Length;

    /// <summary>Creates a FixedString64 from a UTF-16 span (truncates to fit).</summary>
    /// <exception cref="ArgumentException">Input contains an embedded NUL byte.</exception>
    public FixedString64(ReadOnlySpan<char> text)
    {
        Length = 0;
        if (text.IsEmpty)
        {
            return;
        }

        Span<byte> dst = stackalloc byte[Capacity * 3];
        Span<char> one = stackalloc char[1];
        Span<byte> out4 = stackalloc byte[4];
        int written;
        if (Encoding.UTF8.GetByteCount(text) <= dst.Length)
        {
            written = Encoding.UTF8.GetBytes(text, dst);
        }
        else
        {
            // Overlong: encode incrementally, stopping at the capacity
            // boundary on a char boundary (never splits a surrogate pair).
            written = 0;
            var enc = Encoding.UTF8.GetEncoder();
            foreach (var ch in text)
            {
                one[0] = ch;
                enc.Convert(one, out4, false, out int charsUsed, out int bytesUsed, out _);
                if (charsUsed == 0 || written + bytesUsed > Capacity)
                {
                    break;
                }

                out4.Slice(0, bytesUsed).CopyTo(dst.Slice(written));
                written += bytesUsed;
            }
        }
        if (written > Capacity)
        {
            // Truncate on a UTF-8 boundary: back off continuation bytes.
            written = Capacity;
            while (written > 0 && (dst[written - 1] & 0xC0) == 0x80)
            {
                written--;
            }
        }

        fixed (byte* p = _bytes)
        {
            dst.Slice(0, written).CopyTo(new Span<byte>(p, Capacity));
        }

        for (var i = 0; i < written; i++)
        {
            if (dst[i] == 0)
            {
                throw new ArgumentException("FixedString64 cannot contain NUL bytes.", nameof(text));
            }
        }

        Length = (byte)written;
    }

    /// <summary>Creates a FixedString64 from a string (truncates to fit).</summary>
    public FixedString64(string? text)
        : this(text.AsSpan())
    {
    }

    /// <summary>Current payload as a UTF-8 byte span.</summary>
    public readonly ReadOnlySpan<byte> AsByteSpan()
    {
        fixed (byte* p = _bytes)
        {
            return new ReadOnlySpan<byte>(p, Length);
        }
    }

    /// <summary>Writes the payload into <paramref name="destination"/>; false when too small.</summary>
    public readonly bool TryFormat(Span<byte> destination, out int bytesWritten)
    {
        var src = AsByteSpan();
        if (destination.Length < src.Length)
        {
            bytesWritten = 0;
            return false;
        }

        src.CopyTo(destination);
        bytesWritten = src.Length;
        return true;
    }

    /// <summary>Parses a UTF-8 byte span into a FixedString64 (truncates to fit).</summary>
    /// <exception cref="ArgumentException">Input contains an embedded NUL byte.</exception>
    public static FixedString64 Parse(ReadOnlySpan<byte> utf8)
    {
        var s = new FixedString64();
        int n = Math.Min(utf8.Length, Capacity);
        utf8.Slice(0, n).CopyTo(s.WritableBytes());

        for (var i = 0; i < n; i++)
        {
            if (utf8[i] == 0)
            {
                throw new ArgumentException("FixedString64 cannot contain NUL bytes.", nameof(utf8));
            }
        }

        s.Length = (byte)n;
        return s;
    }

    /// <summary>Decodes the payload to a managed string (allocates — cold paths only).</summary>
    public readonly override string ToString() => Encoding.UTF8.GetString(AsByteSpan());

    /// <inheritdoc/>
    public readonly bool Equals(FixedString64 other) => AsByteSpan().SequenceEqual(other.AsByteSpan());

    /// <inheritdoc/>
    public readonly override bool Equals(object? obj) => obj is FixedString64 other && Equals(other);

    /// <inheritdoc/>
    public readonly override int GetHashCode()
    {
        var h = new HashCode();
        var span = AsByteSpan();
        for (var i = 0; i < span.Length; i++)
        {
            h.Add(span[i]);
        }

        return h.ToHashCode();
    }

    /// <inheritdoc/>
    public static bool operator ==(FixedString64 left, FixedString64 right) => left.Equals(right);

    /// <inheritdoc/>
    public static bool operator !=(FixedString64 left, FixedString64 right) => !left.Equals(right);
}

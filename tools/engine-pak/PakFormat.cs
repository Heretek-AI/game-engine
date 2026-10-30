// PakFormat.cs — M5-04: managed ENGPAK writer (mirror of native asset_pak).
//
// Byte-identical layout to native/assets asset_pak.{hpp,cpp}:
//   header  magic[8]="ENGPAK1\0" version u16=1 flags u16 entry_count u32
//           toc_offset u64 (LE)
//   body    entry blobs back-to-back (stored or LZ4-block)
//   TOC     64-byte records sorted by GUID: guid[16] kind u8 codec u8
//           reserved u16 name_hash u64 offset u64 packed u64 unpacked u64
//           crc32 u32 reserved2 u32 tail[4]
//
// The managed writer packs; the native PakReader verifies (golden-vector
// ctest reads a managed-packed .pak). LZ4 here is a straightforward port
// of the native encoder (same 12-bit hash-chain format) so blobs are
// wire-compatible both directions.
using System.Buffers.Binary;

namespace Engine.Pak;

internal static class PakFormat
{
    public const int MagicLen = 8;
    public static readonly byte[] Magic = "ENGPAK1\0"u8.ToArray();
    public const ushort Version = 1;
    public const int HeaderSize = 24;
    public const int TocRecordSize = 64;
    public const byte CodecStored = 0;
    public const byte CodecLz4 = 1;

    public static ulong HashName(string name)
    {
        ulong h = 14695981039346656037ul;
        foreach (var b in System.Text.Encoding.UTF8.GetBytes(name))
        {
            h ^= b;
            h *= 1099511628211ul;
        }

        return h;
    }

    private static readonly uint[] CrcTable = BuildCrcTable();

    private static uint[] BuildCrcTable()
    {
        var t = new uint[256];
        for (uint i = 0; i < 256; i++)
        {
            uint c = i;
            for (var k = 0; k < 8; k++)
            {
                c = (c & 1) != 0 ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }

            t[i] = c;
        }

        return t;
    }

    public static uint Crc32(ReadOnlySpan<byte> data)
    {
        uint c = 0xFFFFFFFFu;
        foreach (var b in data)
        {
            c = CrcTable[(c ^ b) & 0xFF] ^ (c >> 8);
        }

        return c ^ 0xFFFFFFFFu;
    }

    // --- LZ4 block encode (port of native PakLz4Compress) -------------------

    public static byte[] Lz4Compress(ReadOnlySpan<byte> src)
    {
        var outBuf = new List<byte>(src.Length + src.Length / 255 + 16);
        if (src.Length == 0)
        {
            return [];
        }

        const int hashBits = 12;
        const int hashSize = 1 << hashBits;
        var table = new int[hashSize];
        Array.Fill(table, -1);
        byte[] srcArr = src.ToArray();

        static uint Load32(byte[] s, int p) =>
            (uint)(s[p] | (s[p + 1] << 8) | (s[p + 2] << 16) | (s[p + 3] << 24));

        static int Hash(byte[] s, int p) =>
            (int)((Load32(s, p) * 2654435761u) >> (32 - hashBits));

        void Emit(int litPos, int litLen, int matchOff, int matchLen)
        {
            var tokenPos = outBuf.Count;
            outBuf.Add(0);
            byte token = 0;
            token |= litLen >= 15 ? (byte)0xF0 : (byte)(litLen << 4);
            var m = matchLen >= 4 ? matchLen - 4 : 0;
            token |= m >= 15 ? (byte)0x0F : (byte)m;
            outBuf[tokenPos] = token;
            if (litLen >= 15)
            {
                var len = litLen - 15;
                while (len >= 255)
                {
                    outBuf.Add(255);
                    len -= 255;
                }

                outBuf.Add((byte)len);
            }

            for (var i = 0; i < litLen; i++)
            {
                outBuf.Add(srcArr[litPos + i]);
            }

            if (matchLen >= 4)
            {
                outBuf.Add((byte)(matchOff & 0xFF));
                outBuf.Add((byte)((matchOff >> 8) & 0xFF));
                if (m >= 15)
                {
                    var len = m - 15;
                    while (len >= 255)
                    {
                        outBuf.Add(255);
                        len -= 255;
                    }

                    outBuf.Add((byte)len);
                }
            }
        }

        int anchor = 0, ip = 0, skip = 0;
        int matchLimit = srcArr.Length - 4;
        while (ip <= matchLimit && matchLimit >= 0)
        {
            int h = Hash(srcArr, ip);
            int refe = table[h];
            int cur = ip;
            table[h] = cur > 0x7FFFFFFF ? -1 : cur;
            bool ok = refe >= 0
                && refe + 4 <= ip
                && cur - refe is > 0 and <= 0xFFFF
                && Load32(srcArr, refe) == Load32(srcArr, ip);
            if (!ok)
            {
                ip += 1 + (skip++ >> 6);
                continue;
            }

            int mStart = refe, iStart = ip;
            while (iStart > anchor && mStart > 0 && srcArr[mStart - 1] == srcArr[iStart - 1])
            {
                mStart--;
                iStart--;
            }

            int mEnd = refe + 4, iEnd = ip + 4;
            while (iEnd < srcArr.Length && srcArr[mEnd] == srcArr[iEnd])
            {
                mEnd++;
                iEnd++;
            }

            Emit(anchor, iStart - anchor, iStart - mStart, iEnd - iStart);
            if (iEnd < matchLimit)
            {
                table[Hash(srcArr, iEnd - 2)] = iEnd - 2;
            }

            anchor = iEnd;
            ip = iEnd;
            skip = 0;
        }

        int tail = srcArr.Length - anchor;
        if (tail > 0)
        {
            var tokenPos = outBuf.Count;
            outBuf.Add(0);
            outBuf[tokenPos] = tail >= 15 ? (byte)0xF0 : (byte)(tail << 4);
            if (tail >= 15)
            {
                var len = tail - 15;
                while (len >= 255)
                {
                    outBuf.Add(255);
                    len -= 255;
                }

                outBuf.Add((byte)len);
            }

            for (var i = anchor; i < srcArr.Length; i++)
            {
                outBuf.Add(srcArr[i]);
            }
        }

        return [.. outBuf];
    }

    // --- archive write ------------------------------------------------------

    public sealed record Entry(byte[] Guid16, byte Kind, byte Codec, ulong NameHash, byte[] Blob, uint Crc, ulong Unpacked);

    public static byte[] BuildPak(List<Entry> entries)
    {
        entries.Sort((a, b) =>
        {
            for (var i = 0; i < 16; i++)
            {
                int c = a.Guid16[i].CompareTo(b.Guid16[i]);
                if (c != 0)
                {
                    return c;
                }
            }

            return a.NameHash.CompareTo(b.NameHash);
        });

        ulong bodyOff = HeaderSize;
        var offsets = new ulong[entries.Count];
        for (var i = 0; i < entries.Count; i++)
        {
            offsets[i] = bodyOff;
            bodyOff += (ulong)entries[i].Blob.Length;
        }

        ulong tocOff = bodyOff;
        using var ms = new MemoryStream((int)(tocOff + (ulong)(entries.Count * TocRecordSize)));
        ms.Write(Magic, 0, Magic.Length);
        Span<byte> tmp = stackalloc byte[8];
        BinaryPrimitives.WriteUInt16LittleEndian(tmp, Version);
        ms.Write(tmp[..2]);
        BinaryPrimitives.WriteUInt16LittleEndian(tmp, 0);
        ms.Write(tmp[..2]);
        BinaryPrimitives.WriteUInt32LittleEndian(tmp, (uint)entries.Count);
        ms.Write(tmp[..4]);
        BinaryPrimitives.WriteUInt64LittleEndian(tmp, tocOff);
        ms.Write(tmp[..8]);

        for (var i = 0; i < entries.Count; i++)
        {
            ms.Write(entries[i].Blob, 0, entries[i].Blob.Length);
        }

        Span<byte> rec = stackalloc byte[TocRecordSize];
        for (var i = 0; i < entries.Count; i++)
        {
            rec.Clear();
            entries[i].Guid16.CopyTo(rec[..16]);
            rec[16] = entries[i].Kind;
            rec[17] = entries[i].Codec;
            BinaryPrimitives.WriteUInt64LittleEndian(rec[20..], entries[i].NameHash);
            BinaryPrimitives.WriteUInt64LittleEndian(rec[28..], offsets[i]);
            BinaryPrimitives.WriteUInt64LittleEndian(rec[36..], (ulong)entries[i].Blob.Length);
            BinaryPrimitives.WriteUInt64LittleEndian(rec[44..], entries[i].Unpacked);
            BinaryPrimitives.WriteUInt32LittleEndian(rec[52..], entries[i].Crc);
            ms.Write(rec);
        }

        return ms.ToArray();
    }
}

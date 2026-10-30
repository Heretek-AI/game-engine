// Program.cs — M5-04: engine-pak CLI (pack | list | verify).
//
// pack:  engine-pak pack --out game.pak [--compress] <asset>...
//        each <asset> is <guid>:<kind>:<path> where kind is
//        model|texture|shader|unknown. GUIDs come from the .meta sidecars
//        (M5-01); --compress tries LZ4 and keeps stored when it pays <8%.
// list:  engine-pak list game.pak            (GUID codec packed/unpacked)
// verify: engine-pak verify game.pak         (bounds + CRC, exit 1 on fail)
//
// list/verify parse the TOC directly (no native dependency) so CI can
// inspect archives without loading engine_core.
using Engine.Pak;

internal static class Program
{
    internal static int Main(string[] args)
    {
        if (args.Length == 0 || args[0] is "-h" or "--help" or "help")
        {
            PrintUsage();
            return args.Length == 0 ? 2 : 0;
        }

        return args[0].ToLowerInvariant() switch
        {
            "pack" => Pack(args[1..]),
            "list" => List(args[1..]),
            "verify" => Verify(args[1..]),
            _ => Fail($"unknown command '{args[0]}'."),
        };
    }

    // --- pack ---------------------------------------------------------------

    private static int Pack(string[] args)
    {
        string? outPath = null;
        var compress = false;
        var inputs = new List<string>();
        for (var i = 0; i < args.Length; i++)
        {
            if (args[i] == "--out" && i + 1 < args.Length)
            {
                outPath = args[++i];
            }
            else if (args[i] == "--compress")
            {
                compress = true;
            }
            else if (args[i] == "--manifest" && i + 1 < args.Length)
            {
                var manifestPath = args[++i];
                try
                {
                    inputs.AddRange(File.ReadAllLines(manifestPath));
                }
                catch (Exception ex)
                {
                    return Fail($"cannot read manifest '{manifestPath}': {ex.Message}");
                }
            }
            else if (!args[i].StartsWith('-'))
            {
                inputs.Add(args[i]);
            }
            else
            {
                return Fail("usage: engine-pak pack --out game.pak [--compress] [--manifest specs.txt] <guid:kind:path>...");
            }
        }

        if (outPath == null || inputs.Count == 0)
        {
            return Fail("usage: engine-pak pack --out game.pak [--compress] [--manifest specs.txt] <guid:kind:path>...");
        }

        var entries = new List<PakFormat.Entry>(inputs.Count);
        var seen = new HashSet<Guid>();
        foreach (var input in inputs)
        {
            var parts = input.Split(':', 3);
            if (parts.Length != 3 || !Guid.TryParse(parts[0], out var guid))
            {
                return Fail($"bad asset spec '{input}' (want <guid>:<kind>:<path>).");
            }

            byte kind = parts[1].ToLowerInvariant() switch
            {
                "model" => 1,
                "texture" => 2,
                "shader" => 3,
                "unknown" => 0,
                _ => 0,
            };
            if (parts[1].ToLowerInvariant() is not ("model" or "texture" or "shader" or "unknown"))
            {
                return Fail($"bad kind '{parts[1]}' (want model|texture|shader|unknown).");
            }

            if (!seen.Add(guid))
            {
                return Fail($"duplicate GUID {parts[0]}.");
            }

            byte[] bytes;
            try
            {
                bytes = File.ReadAllBytes(parts[2]);
            }
            catch (Exception ex)
            {
                return Fail($"cannot read '{parts[2]}': {ex.Message}");
            }

            if (bytes.Length == 0)
            {
                return Fail($"empty payload '{parts[2]}'.");
            }

            // Canonical GUID bytes: .NET Guid.ToByteArray uses mixed-endian
            // fields; ENGPAK compares raw bytes in RFC-4122 network order
            // (native MintGuid emit order).
            var guidBytes = GuidToNetworkOrder(guid);

            byte codec = PakFormat.CodecStored;
            var blob = bytes;
            if (compress)
            {
                var packed = PakFormat.Lz4Compress(bytes);
                if (packed.Length < bytes.Length
                    && (bytes.Length - packed.Length) * 100 / bytes.Length >= 8)
                {
                    codec = PakFormat.CodecLz4;
                    blob = packed;
                }
            }

            entries.Add(new PakFormat.Entry(
                guidBytes, kind, codec,
                PakFormat.HashName(parts[2]), blob,
                PakFormat.Crc32(bytes), (ulong)bytes.Length));
        }

        var pak = PakFormat.BuildPak(entries);
        try
        {
            File.WriteAllBytes(outPath, pak);
        }
        catch (Exception ex)
        {
            return Fail($"cannot write '{outPath}': {ex.Message}");
        }

        Console.WriteLine($"packed {entries.Count} entries ({pak.Length} bytes) -> {outPath}");
        return 0;
    }

    // --- list / verify (TOC-direct, no native dep) ---------------------------

    private sealed record TocRow(string AssetGuid, byte Kind, byte Codec, ulong Packed, ulong Unpacked, uint Crc, ulong Offset);

    private static List<TocRow>? ReadToc(string path, out string error)
        => ReadToc(path, out error, out _);

    private static List<TocRow>? ReadToc(string path, out string error, out ulong tocOffset)
    {
        error = string.Empty;
        tocOffset = 0;
        byte[] bytes;
        try
        {
            bytes = File.ReadAllBytes(path);
        }
        catch (Exception ex)
        {
            error = $"cannot read '{path}': {ex.Message}";
            return null;
        }

        if (bytes.Length < 24 || !PakFormat.Magic.AsSpan().SequenceEqual(bytes.AsSpan(0, 8)))
        {
            error = "bad magic (want ENGPAK1).";
            return null;
        }

        var count = System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(12, 4));
        var version = System.Buffers.Binary.BinaryPrimitives.ReadUInt16LittleEndian(bytes.AsSpan(8, 2));
        if (version != PakFormat.Version)
        {
            error = $"unsupported version {version}.";
            return null;
        }

        var tocOff = System.Buffers.Binary.BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(16, 8));
        // Subtraction-based: tocOff/count are file-controlled and the sum
        // can wrap; the (int) casts below would then throw instead of
        // failing closed (CodeRabbit). Cap count first (TOC itself is
        // count*64 bytes inside the file).
        var len = (ulong)bytes.Length;
        if (count > len / 64 || tocOff < 24 || tocOff > len || (ulong)count * 64 > len - tocOff)
        {
            error = "TOC out of bounds.";
            return null;
        }

        tocOffset = tocOff;

        var rows = new List<TocRow>((int)count);
        for (var i = 0; i < count; i++)
        {
            var rec = bytes.AsSpan((int)tocOff + i * 64, 64);
            var gb = rec[..16].ToArray();
            rows.Add(new TocRow(
                GuidFromNetworkOrder(gb).ToString("D"),
                rec[16], rec[17],
                System.Buffers.Binary.BinaryPrimitives.ReadUInt64LittleEndian(rec[36..]),
                System.Buffers.Binary.BinaryPrimitives.ReadUInt64LittleEndian(rec[44..]),
                System.Buffers.Binary.BinaryPrimitives.ReadUInt32LittleEndian(rec[52..]),
                System.Buffers.Binary.BinaryPrimitives.ReadUInt64LittleEndian(rec[28..])));
        }

        return rows;
    }

    private static int List(string[] args)
    {
        if (args.Length != 1)
        {
            return Fail("usage: engine-pak list game.pak");
        }

        var rows = ReadToc(args[0], out var err);
        if (rows == null)
        {
            return Fail(err);
        }

        foreach (var r in rows)
        {
            Console.WriteLine($"{r.AssetGuid} kind={r.Kind} codec={(r.Codec == 1 ? "lz4" : "stored")} packed={r.Packed} unpacked={r.Unpacked} crc={r.Crc:x8}");
        }

        return 0;
    }

    private static int Verify(string[] args)
    {
        if (args.Length != 1)
        {
            return Fail("usage: engine-pak verify game.pak");
        }

        // ReadToc already fails closed on missing/unreadable/malformed
        // input (exit 2 via Fail); no duplicate read here (CodeRabbit:
        // the old double-read crashed on missing files before ReadToc).
        var rows = ReadToc(args[0], out var err, out var tocOff);
        if (rows == null)
        {
            return Fail(err);
        }

        // One read for all entries (CodeRabbit: was per-entry in the loop).
        byte[] bytes;
        try
        {
            bytes = File.ReadAllBytes(args[0]);
        }
        catch (Exception ex)
        {
            return Fail($"cannot read '{args[0]}': {ex.Message}");
        }

        var len = (ulong)bytes.Length;
        TocRow? prev = null;
        foreach (var r in rows)
        {
            // TOC order (mirrors native ParseToc): strictly increasing GUID,
            // so duplicates and reordered TOCs fail the gate.
            if (prev != null && string.CompareOrdinal(prev.AssetGuid, r.AssetGuid) >= 0)
            {
                return Fail($"entry {r.AssetGuid} TOC not sorted.");
            }

            prev = r;

            // Entry blob must not overlap the TOC (mirrors native ParseToc).
            if (r.Offset > tocOff || r.Packed > tocOff - r.Offset)
            {
                return Fail($"entry {r.AssetGuid} overlaps TOC.");
            }

            // Subtraction-based (file-controlled Offset/Packed can wrap).
            if (r.Offset < 24 || r.Offset > len || r.Packed > len - r.Offset)
            {
                return Fail($"entry {r.AssetGuid} out of bounds.");
            }

            // (int) casts: Packed <= len and len fits in memory; guard anyway.
            if (r.Packed > int.MaxValue || r.Unpacked > int.MaxValue)
            {
                return Fail($"entry {r.AssetGuid} too large to verify.");
            }

            if (r.Codec == PakFormat.CodecStored)
            {
                // Stored entries carry the payload as-is (mirrors native
                // PakReader::read packed_size == unpacked_size check).
                if (r.Packed != r.Unpacked)
                {
                    return Fail($"entry {r.AssetGuid} stored size mismatch.");
                }

                var span = bytes.AsSpan((int)r.Offset, (int)r.Packed);
                if (PakFormat.Crc32(span) != r.Crc)
                {
                    return Fail($"entry {r.AssetGuid} CRC mismatch.");
                }
            }
            else if (r.Codec == PakFormat.CodecLz4)
            {
                // Full payload gate (CodeRabbit): decode + CRC, no native dep.
                var blob = bytes.AsSpan((int)r.Offset, (int)r.Packed);
                var plain = new byte[(int)r.Unpacked];
                if (!PakFormat.Lz4Decompress(blob, plain))
                {
                    return Fail($"entry {r.AssetGuid} LZ4 decode failed.");
                }

                if (PakFormat.Crc32(plain) != r.Crc)
                {
                    return Fail($"entry {r.AssetGuid} CRC mismatch.");
                }
            }
            else
            {
                return Fail($"entry {r.AssetGuid} bad codec {r.Codec}.");
            }
        }

        Console.WriteLine($"verify ok: {rows.Count} entries.");
        return 0;
    }

    // ENGPAK canonical bytes = UUID string hex parsed straight through
    // (native GuidFromString writes bytes[0..16] in display order — no
    // field swapping like Guid.ToByteArray). Parse the D-format string.
    private static byte[] GuidToNetworkOrder(Guid guid)
    {
        var hex = guid.ToString("N");
        var b = new byte[16];
        for (var i = 0; i < 16; i++)
        {
            b[i] = Convert.ToByte(hex.Substring(i * 2, 2), 16);
        }

        return b;
    }

    private static Guid GuidFromNetworkOrder(byte[] b) =>
        Guid.Parse(Convert.ToHexString(b));

    private static int Fail(string message)
    {
        Console.Error.WriteLine($"engine-pak: {message}");
        PrintUsage();
        return 2;
    }

    private static void PrintUsage() =>
        Console.Error.WriteLine("""
            usage: engine-pak <command>
              pack --out game.pak [--compress] [--manifest specs.txt] <guid:kind:path>...
              list game.pak
              verify game.pak
            """);
}

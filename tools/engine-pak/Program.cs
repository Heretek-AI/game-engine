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
            else if (!args[i].StartsWith('-'))
            {
                inputs.Add(args[i]);
            }
            else
            {
                return Fail("usage: engine-pak pack --out game.pak [--compress] <guid:kind:path>...");
            }
        }

        if (outPath == null || inputs.Count == 0)
        {
            return Fail("usage: engine-pak pack --out game.pak [--compress] <guid:kind:path>...");
        }

        var entries = new List<PakFormat.Entry>(inputs.Count);
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
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

            if (!seen.Add(parts[0]))
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

    private sealed record TocRow(string Guid, byte Kind, byte Codec, ulong Packed, ulong Unpacked, uint Crc, ulong Offset);

    private static List<TocRow>? ReadToc(string path, out string error)
    {
        error = string.Empty;
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
        var tocOff = System.Buffers.Binary.BinaryPrimitives.ReadUInt64LittleEndian(bytes.AsSpan(16, 8));
        if (tocOff + (ulong)count * 64 > (ulong)bytes.Length)
        {
            error = "TOC out of bounds.";
            return null;
        }

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
            Console.WriteLine($"{r.Guid} kind={r.Kind} codec={(r.Codec == 1 ? "lz4" : "stored")} packed={r.Packed} unpacked={r.Unpacked} crc={r.Crc:x8}");
        }

        return 0;
    }

    private static int Verify(string[] args)
    {
        if (args.Length != 1)
        {
            return Fail("usage: engine-pak verify game.pak");
        }

        var bytes = File.ReadAllBytes(args[0]);
        var rows = ReadToc(args[0], out var err);
        if (rows == null)
        {
            return Fail(err);
        }

        foreach (var r in rows)
        {
            if (r.Offset + r.Packed > (ulong)bytes.Length)
            {
                return Fail($"entry {r.Guid} out of bounds.");
            }

            if (r.Codec == PakFormat.CodecStored)
            {
                var span = bytes.AsSpan((int)r.Offset, (int)r.Packed);
                if (PakFormat.Crc32(span) != r.Crc)
                {
                    return Fail($"entry {r.Guid} CRC mismatch.");
                }
            }
            // LZ4 blobs: CRC needs decompression (native reader gate);
            // here we check bounds + codec validity only.
            else if (r.Codec != PakFormat.CodecLz4)
            {
                return Fail($"entry {r.Guid} bad codec {r.Codec}.");
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
              pack --out game.pak [--compress] <guid:kind:path>...
              list game.pak
              verify game.pak
            """);
}

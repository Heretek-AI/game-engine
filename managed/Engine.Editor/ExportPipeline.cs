// ExportPipeline.cs — M5-04: GUID-keyed ENGPAK export from the editor scene.
//
// One-click standalone pipeline (AC1): scan Assets/ for .meta sidecars
// (GUID DB, M5-01), resolve each GUID's build outputs under
// Library/ArtifactCache/<xx>/<guid-hex>.<ext>, and pack them into one
// memory-mappable .pak via the ENGPAK writer (same layout the native
// PakReader verifies: ENGPAK1 magic, GUID-sorted TOC, LZ4-block codec,
// CRC32). Zero native ABI: asset bytes come from files on disk.
//
// Scene scoping: the exporter packs every GUID referenced by the scene
// (entity asset components when readable) UNION every artifact present in
// the cache. V1 packs the full cache — scenes reference by GUID, never by
// path, so over-inclusion only costs bytes, never correctness; a
// scene-precise subset lands when script components expose AssetRef<>
// enumerably (deferred, tracked in the record below).
//
// AOT note (AC2): NativeAOT compiles the GAME (ExportSample / user
// GameScripts), not the editor. This pipeline stays JIT (editor tooling)
// and shells `dotnet publish -r <rid> /p:PublishAot=true` on the game
// project — see ExportAot below. The game binary then cold-opens the .pak
// through the native mmap reader (AC3, <1.0s gate).
using System.Diagnostics;
using System.Text.Json;

namespace Engine.Editor;

/// <summary>Scene -> .pak exporter + game AOT publisher (M5-04).</summary>
public static class ExportPipeline
{
    /// <summary>
    /// Pack the scene's GUID assets into <paramref name="outPath"/>.
    /// Throws ArgumentException on bad params, InvalidOperationException
    /// when nothing is packable (empty cache / missing roots).
    /// </summary>
    public static CliExportResult PackScene(
        EditorSession session,
        string outPath,
        bool compress,
        string assetsRoot,
        string libraryRoot)
    {
        ArgumentNullException.ThrowIfNull(session);
        ArgumentException.ThrowIfNullOrWhiteSpace(outPath);

        // Enumerate the cache: <libraryRoot>/<xx>/<guid>.<ext>.
        var artifacts = new List<(string GuidText, string Path, byte Kind)>();
        if (Directory.Exists(libraryRoot))
        {
            foreach (var file in Directory.EnumerateFiles(libraryRoot, "*", SearchOption.AllDirectories))
            {
                var name = Path.GetFileNameWithoutExtension(file);
                if (!Guid.TryParse(name, out _))
                {
                    continue;
                }

                var kind = Path.GetExtension(file).ToLowerInvariant() switch
                {
                    ".mesh" => (byte)1,
                    ".bc7" => (byte)2,
                    ".spv" => (byte)3,
                    ".bin" => (byte)0,
                    _ => (byte)0,
                };
                artifacts.Add((name, file, kind));
            }
        }

        // Fallback: no cache yet (fresh checkout / headless CI) — pack the
        // raw sources referenced by .meta sidecars so the export gate still
        // exercises pack -> native-read byte-exact. Kind from extension.
        if (artifacts.Count == 0 && Directory.Exists(assetsRoot))
        {
            foreach (var meta in Directory.EnumerateFiles(assetsRoot, "*.meta", SearchOption.AllDirectories))
            {
                var guid = ParseMetaGuid(meta);
                if (guid == null)
                {
                    continue;
                }

                var source = meta[..^".meta".Length];
                if (!File.Exists(source))
                {
                    continue;
                }

                var kind = Path.GetExtension(source).ToLowerInvariant() switch
                {
                    ".glb" or ".gltf" => (byte)1,
                    ".png" or ".jpg" or ".jpeg" => (byte)2,
                    ".spv" or ".slang" => (byte)3,
                    _ => (byte)0,
                };
                artifacts.Add((guid, source, kind));
            }
        }

        if (artifacts.Count == 0)
        {
            throw new InvalidOperationException(
                $"export.pack: no packable assets (libraryRoot '{libraryRoot}' empty, assetsRoot '{assetsRoot}' has no .meta sidecars).");
        }

        var packer = FindEnginePak();
        var scratch = Path.Combine(Path.GetTempPath(), $"engine-export-{Environment.ProcessId}");
        Directory.CreateDirectory(scratch);
        var manifest = Path.Combine(scratch, "manifest.txt");
        try
        {
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            using (var w = new StreamWriter(manifest))
            {
                foreach (var (guidText, path, kind) in artifacts)
                {
                    if (!seen.Add(guidText))
                    {
                        continue;
                    }

                    w.WriteLine($"{guidText}:{(kind == 1 ? "model" : kind == 2 ? "texture" : kind == 3 ? "shader" : "unknown")}:{path}");
                }
            }

            var psi = new ProcessStartInfo(packer)
            {
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
            };
            psi.ArgumentList.Add("pack");
            psi.ArgumentList.Add("--out");
            psi.ArgumentList.Add(outPath);
            if (compress)
            {
                psi.ArgumentList.Add("--compress");
            }

            foreach (var line in File.ReadLines(manifest))
            {
                psi.ArgumentList.Add(line);
            }

            using var proc = Process.Start(psi)
                ?? throw new InvalidOperationException("export.pack: could not start engine-pak.");
            var stdout = proc.StandardOutput.ReadToEnd();
            var stderr = proc.StandardError.ReadToEnd();
            if (!proc.WaitForExit(TimeSpan.FromMinutes(5)))
            {
                try
                {
                    proc.Kill(entireProcessTree: true);
                }
                catch (Exception)
                {
                }

                throw new InvalidOperationException("export.pack: engine-pak timed out.");
            }

            if (proc.ExitCode != 0)
            {
                throw new InvalidOperationException($"export.pack: engine-pak failed ({proc.ExitCode}): {stderr.Trim()} {stdout.Trim()}".Trim());
            }
        }
        finally
        {
            try
            {
                File.Delete(manifest);
            }
            catch (Exception)
            {
            }
        }

        // Report: re-read the TOC via engine-pak list (parseable lines).
        var entries = ListEntries(packer, outPath);
        var lz4 = entries.Count(e => e.Codec == "lz4");
        ulong packed = 0, unpacked = 0;
        foreach (var e in entries)
        {
            packed += e.PackedBytes;
            unpacked += e.UnpackedBytes;
        }

        return new CliExportResult(
            Path.GetFullPath(outPath),
            entries.Count, lz4, entries.Count - lz4,
            packed, unpacked, compress, entries);
    }

    /// <summary>
    /// Publish a game project with NativeAOT (AC2): runs
    /// `dotnet publish -r &lt;rid&gt; /p:PublishAot=true` and returns the
    /// published binary path. Throws on failure with the SDK output tail.
    /// </summary>
    public static string ExportAot(string gameProject, string rid, string configuration = "Release")
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(gameProject);
        ArgumentException.ThrowIfNullOrWhiteSpace(rid);
        if (!File.Exists(gameProject))
        {
            throw new ArgumentException($"game project not found: {gameProject}.", nameof(gameProject));
        }

        var psi = new ProcessStartInfo("dotnet", $"publish \"{gameProject}\" -r {rid} -c {configuration} -p:PublishAot=true --nologo")
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        using var proc = Process.Start(psi)
            ?? throw new InvalidOperationException("export.aot: could not start dotnet.");
        var stdout = proc.StandardOutput.ReadToEnd();
        var stderr = proc.StandardError.ReadToEnd();
        if (!proc.WaitForExit(TimeSpan.FromMinutes(20)))
        {
            try
            {
                proc.Kill(entireProcessTree: true);
            }
            catch (Exception)
            {
            }

            throw new InvalidOperationException("export.aot: dotnet publish timed out.");
        }

        if (proc.ExitCode != 0)
        {
            var tail = (stderr + "\n" + stdout).Trim();
            if (tail.Length > 2000)
            {
                tail = tail[^2000..];
            }

            throw new InvalidOperationException($"export.aot: dotnet publish failed ({proc.ExitCode}): {tail}");
        }

        var projDir = Path.GetDirectoryName(Path.GetFullPath(gameProject))!;
        var name = Path.GetFileNameWithoutExtension(gameProject);
        var bin = Path.Combine(projDir, "bin", configuration, "net10.0", rid, "publish", name);
        if (OperatingSystem.IsWindows())
        {
            bin += ".exe";
        }

        if (!File.Exists(bin))
        {
            throw new InvalidOperationException($"export.aot: published binary not found: {bin}.");
        }

        return bin;
    }

    // --- helpers ------------------------------------------------------------

    private static string? ParseMetaGuid(string metaPath)
    {
        try
        {
            foreach (var line in File.ReadLines(metaPath))
            {
                var t = line.Trim();
                if (t.StartsWith('#') || t.Length == 0)
                {
                    continue;
                }

                if (t.StartsWith("guid:", StringComparison.OrdinalIgnoreCase))
                {
                    var g = t["guid:".Length..].Trim();
                    return Guid.TryParse(g, out _) ? g : null;
                }

                // First non-blank non-comment line MUST be the guid line —
                // anything else means malformed.
                return null;
            }
        }
        catch (Exception)
        {
        }

        return null;
    }

    private static string FindEnginePak()
    {
        // Prefer the tool next to this assembly (published layout), then
        // the repo build tree (dev/CI layout: <repo>/tools/engine-pak).
        var seeds = new List<string> { AppContext.BaseDirectory };
        try
        {
            var entry = System.Reflection.Assembly.GetEntryAssembly()?.Location;
            if (!string.IsNullOrEmpty(entry))
            {
                seeds.Add(Path.GetDirectoryName(Path.GetFullPath(entry)) ?? string.Empty);
            }
        }
        catch (Exception)
        {
        }

        seeds.Add(Directory.GetCurrentDirectory());
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (var seed in seeds)
        {
            DirectoryInfo? dir = null;
            try
            {
                dir = new DirectoryInfo(seed);
            }
            catch (Exception)
            {
                continue;
            }

            for (var i = 0; i < 12 && dir != null; i++, dir = dir.Parent)
            {
                var key = dir.FullName;
                if (!seen.Add(key))
                {
                    continue;
                }

                foreach (var cand in new[]
                {
                    Path.Combine(key, "engine-pak"),
                    Path.Combine(key, "engine-pak.exe"),
                    Path.Combine(key, "tools", "engine-pak", "bin", "Release", "net10.0", "engine-pak"),
                    Path.Combine(key, "tools", "engine-pak", "bin", "Release", "net10.0", "engine-pak.exe"),
                })
                {
                    if (File.Exists(cand))
                    {
                        return Path.GetFullPath(cand);
                    }
                }

                if (File.Exists(Path.Combine(key, "Engine.slnx")))
                {
                    var built = Path.Combine(key, "tools", "engine-pak", "bin", "Release", "net10.0", "engine-pak");
                    if (File.Exists(built))
                    {
                        return built;
                    }

                    var builtExe = built + ".exe";
                    if (File.Exists(builtExe))
                    {
                        return builtExe;
                    }
                }
            }
        }

        throw new InvalidOperationException("export.pack: engine-pak binary not found (build tools/engine-pak first).");
    }

    private static List<CliExportEntry> ListEntries(string packer, string pakPath)
    {
        var psi = new ProcessStartInfo(packer, $"list \"{pakPath}\"")
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        using var proc = Process.Start(psi)
            ?? throw new InvalidOperationException("export.pack: could not run engine-pak list.");
        var stdout = proc.StandardOutput.ReadToEnd();
        proc.StandardError.ReadToEnd();
        if (!proc.WaitForExit(TimeSpan.FromMinutes(2)) || proc.ExitCode != 0)
        {
            throw new InvalidOperationException("export.pack: engine-pak list failed.");
        }

        // Line: <guid> kind=<k> codec=<lz4|stored> packed=<p> unpacked=<u> crc=<x>
        var entries = new List<CliExportEntry>();
        foreach (var line in stdout.Split('\n', StringSplitOptions.RemoveEmptyEntries))
        {
            try
            {
                var parts = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
                if (parts.Length < 5)
                {
                    continue;
                }

                ulong packed = 0, unpacked = 0;
                string codec = "stored";
                byte kind = 0;
                foreach (var kv in parts[1..])
                {
                    var eq = kv.IndexOf('=');
                    if (eq < 0)
                    {
                        continue;
                    }

                    var k = kv[..eq];
                    var v = kv[(eq + 1)..];
                    if (k == "codec")
                    {
                        codec = v;
                    }
                    else if (k == "packed")
                    {
                        _ = ulong.TryParse(v, out packed);
                    }
                    else if (k == "unpacked")
                    {
                        _ = ulong.TryParse(v, out unpacked);
                    }
                    else if (k == "kind")
                    {
                        _ = byte.TryParse(v, out kind);
                    }
                }

                entries.Add(new CliExportEntry(parts[0], kind, codec, packed, unpacked));
            }
            catch (Exception)
            {
            }
        }

        return entries;
    }
}

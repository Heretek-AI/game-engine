// PackageLockFile.cs — P7-05: packages.lock.json pinning (PARITY-134).
//
// Lock shape (canonical indented JSON, key order name/url/tag/sha):
//   {
//     "version": 1,
//     "packages": [
//       { "name": "core", "url": "https://example.com/core.git",
//         "tag": "v1.2.3", "sha": "<40-char commit SHA>" }
//     ]
//   }
//
// The lock pins each manifest request (name/url/tag) to the exact commit
// SHA it resolved to. SHAs are full 40-char lowercase hex as printed by
// rev-parse. Unknown fields are rejected; any missing/invalid field fails
// with PackageLockException. Pure managed — no ABI surface.
using System.Text.Json;

namespace Engine.Editor;

/// <summary>One pinned entry of packages.lock.json.</summary>
public sealed record LockedPackage(string Name, string Url, string Tag, string Sha);

/// <summary>Parsed packages.lock.json.</summary>
public sealed record PackageLock(IReadOnlyList<LockedPackage> Packages)
{
    /// <summary>Entries; never null (empty when the lock pins nothing).</summary>
    public IReadOnlyList<LockedPackage> PackageList => Packages ?? Array.Empty<LockedPackage>();
}

/// <summary>Validation failure for <see cref="PackageLockFile"/>.</summary>
public sealed class PackageLockException : Exception
{
    public PackageLockException(string message)
        : base(message)
    {
    }

    public PackageLockException(string message, Exception inner)
        : base(message, inner)
    {
    }
}

/// <summary>Load / parse / save for packages.lock.json (P7-05 lockfile).</summary>
public static class PackageLockFile
{
    /// <summary>Conventional lock file name beside the project directory.</summary>
    public const string DefaultFileName = "packages.lock.json";

    /// <summary>Lock format version written and accepted by this reader.</summary>
    public const int CurrentVersion = 1;

    private static readonly HashSet<string> RootFields = new(StringComparer.Ordinal)
    {
        "version", "packages",
    };

    private static readonly HashSet<string> EntryFields = new(StringComparer.Ordinal)
    {
        "name", "url", "tag", "sha",
    };

    /// <summary>Parse lock JSON text; throws <see cref="PackageLockException"/> when invalid.</summary>
    public static PackageLock Parse(string json)
    {
        JsonDocument doc;
        try
        {
            doc = JsonDocument.Parse(json);
        }
        catch (JsonException ex)
        {
            throw new PackageLockException($"lock file is not valid JSON: {ex.Message}", ex);
        }

        using (doc)
        {
            if (doc.RootElement.ValueKind != JsonValueKind.Object)
            {
                throw new PackageLockException("lock file root must be an object.");
            }

            foreach (var prop in doc.RootElement.EnumerateObject())
            {
                if (!RootFields.Contains(prop.Name))
                {
                    throw new PackageLockException($"lock file has unknown field '{prop.Name}'.");
                }
            }

            if (!doc.RootElement.TryGetProperty("version", out var versionEl)
                || versionEl.ValueKind != JsonValueKind.Number
                || !versionEl.TryGetInt32(out var version)
                || version != CurrentVersion)
            {
                throw new PackageLockException(
                    $"lock file 'version' must be {CurrentVersion}.");
            }

            var packages = new List<LockedPackage>();
            if (doc.RootElement.TryGetProperty("packages", out var pkgs))
            {
                if (pkgs.ValueKind != JsonValueKind.Array)
                {
                    throw new PackageLockException("lock file 'packages' must be an array.");
                }

                var seen = new HashSet<string>(StringComparer.Ordinal);
                var index = 0;
                foreach (var entry in pkgs.EnumerateArray())
                {
                    packages.Add(ParseEntry(entry, index));
                    if (!seen.Add(packages[^1].Name))
                    {
                        throw new PackageLockException(
                            $"lock file packages[{index}]: duplicate name '{packages[^1].Name}'.");
                    }

                    index++;
                }
            }

            return new PackageLock(packages);
        }
    }

    /// <summary>Load a lock from a file path; throws when missing or invalid.</summary>
    public static PackageLock Load(string path)
    {
        string text;
        try
        {
            text = File.ReadAllText(path);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            throw new PackageLockException($"cannot read lock file at '{path}': {ex.Message}", ex);
        }

        try
        {
            return Parse(text);
        }
        catch (PackageLockException ex)
        {
            throw new PackageLockException($"invalid lock file at '{path}': {ex.Message}", ex);
        }
    }

    /// <summary>
    /// Load a lock, returning null when the file is missing or fails to
    /// validate. The resolver treats null as "no pin yet" and re-resolves.
    /// </summary>
    public static PackageLock? TryLoad(string path)
    {
        try
        {
            return Load(path);
        }
        catch (PackageLockException)
        {
            return null;
        }
    }

    /// <summary>Serialize a lock to canonical indented JSON.</summary>
    public static string Serialize(PackageLock packageLock)
    {
        ArgumentNullException.ThrowIfNull(packageLock);
        using var stream = new MemoryStream();
        using (var writer = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = true }))
        {
            writer.WriteStartObject();
            writer.WriteNumber("version", CurrentVersion);
            writer.WriteStartArray("packages");
            foreach (var p in packageLock.PackageList)
            {
                writer.WriteStartObject();
                writer.WriteString("name", p.Name);
                writer.WriteString("url", p.Url);
                writer.WriteString("tag", p.Tag);
                writer.WriteString("sha", p.Sha);
                writer.WriteEndObject();
            }

            writer.WriteEndArray();
            writer.WriteEndObject();
        }

        return System.Text.Encoding.UTF8.GetString(stream.ToArray());
    }

    /// <summary>Write a lock to a file path (validates by round-trip).</summary>
    public static void Save(string path, PackageLock packageLock)
    {
        // Round-trip through Parse so Save never writes a lock that Load rejects.
        _ = Parse(Serialize(packageLock));
        File.WriteAllText(path, Serialize(packageLock));
    }

    private static LockedPackage ParseEntry(JsonElement entry, int index)
    {
        var where = $"lock file packages[{index}]";
        if (entry.ValueKind != JsonValueKind.Object)
        {
            throw new PackageLockException($"{where} must be an object.");
        }

        foreach (var prop in entry.EnumerateObject())
        {
            if (!EntryFields.Contains(prop.Name))
            {
                throw new PackageLockException($"{where} has unknown field '{prop.Name}'.");
            }
        }

        var name = RequireNonEmptyString(entry, "name", where);
        var url = RequireNonEmptyString(entry, "url", where);
        var tag = RequireNonEmptyString(entry, "tag", where);
        var sha = RequireNonEmptyString(entry, "sha", where);

        // Names stay compatible with the manifest contract so a lock entry
        // always maps to Packages/<name>/. URLs are stored verbatim: the
        // manifest requires HTTPS, but fixtures and mirrors may resolve
        // local paths, so the lock only requires non-empty.
        if (!ProjectManifestFile.IsValidPackageName(name))
        {
            throw new PackageLockException(
                $"{where}: 'name' must match [A-Za-z0-9_-]+ (got '{name}').");
        }

        if (!IsCommitSha(sha))
        {
            throw new PackageLockException(
                $"{where}: 'sha' must be a 40-char hex commit SHA (got '{sha}').");
        }

        return new LockedPackage(name, url, tag, sha);
    }

    private static string RequireNonEmptyString(JsonElement obj, string field, string where)
    {
        if (!obj.TryGetProperty(field, out var el) || el.ValueKind != JsonValueKind.String)
        {
            throw new PackageLockException($"{where} is missing required field '{field}'.");
        }

        var s = el.GetString() ?? string.Empty;
        if (s.Length == 0)
        {
            throw new PackageLockException($"{where} '{field}' must not be empty.");
        }

        return s;
    }

    internal static bool IsCommitSha(string s)
    {
        if (s.Length != 40)
        {
            return false;
        }

        foreach (var c in s)
        {
            var hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            if (!hex)
            {
                return false;
            }
        }

        return true;
    }
}

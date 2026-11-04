// ProjectManifest.cs — P7-05: project.json manifest schema with `packages`.
//
// Manifest shape (backward compatible with the P3-era flat manifest):
//   {
//     "name": "MyGame",                       // required, non-empty
//     "startScene": "scenes/main.scene.json", // optional string
//     "gameAssembly": "bin/Game.dll",         // optional string
//     "packages": [                            // optional; absent/empty = no packages
//       { "name": "core", "url": "https://example.com/core.git", "tag": "v1.2.3" }
//     ]
//   }
//
// Package entry contract (from P7-05 preflight): name is required, unique,
// `[A-Za-z0-9_-]+`; url is a required git HTTPS URL; tag is a required git
// tag. Unknown fields are rejected at both levels; any missing/invalid field
// fails validation with ProjectManifestException. Pure managed — no ABI.
using System.Text.Json;

namespace Engine.Editor;

/// <summary>One entry of the manifest <c>packages</c> array.</summary>
public sealed record PackageReference(string Name, string Url, string Tag);

/// <summary>Parsed project.json manifest.</summary>
public sealed record ProjectManifest(
    string Name,
    string? StartScene = null,
    string? GameAssembly = null,
    IReadOnlyList<PackageReference>? Packages = null)
{
    /// <summary>Package entries; never null (empty when the manifest has none).</summary>
    public IReadOnlyList<PackageReference> PackageList => Packages ?? Array.Empty<PackageReference>();
}

/// <summary>Validation failure for <see cref="ProjectManifestFile"/>.</summary>
public sealed class ProjectManifestException : Exception
{
    public ProjectManifestException(string message)
        : base(message)
    {
    }

    public ProjectManifestException(string message, Exception inner)
        : base(message, inner)
    {
    }
}

/// <summary>Load / parse / save for project.json (P7-05 schema).</summary>
public static class ProjectManifestFile
{
    private static readonly HashSet<string> RootFields = new(StringComparer.Ordinal)
    {
        "name", "startScene", "gameAssembly", "packages",
    };

    private static readonly HashSet<string> PackageFields = new(StringComparer.Ordinal)
    {
        "name", "url", "tag",
    };

    /// <summary>Parse manifest JSON text; throws <see cref="ProjectManifestException"/> when invalid.</summary>
    public static ProjectManifest Parse(string json)
    {
        JsonDocument doc;
        try
        {
            doc = JsonDocument.Parse(json);
        }
        catch (JsonException ex)
        {
            throw new ProjectManifestException($"project.json is not valid JSON: {ex.Message}", ex);
        }

        using (doc)
        {
            if (doc.RootElement.ValueKind != JsonValueKind.Object)
            {
                throw new ProjectManifestException("project.json root must be an object.");
            }

            foreach (var prop in doc.RootElement.EnumerateObject())
            {
                if (!RootFields.Contains(prop.Name))
                {
                    throw new ProjectManifestException($"project.json has unknown field '{prop.Name}'.");
                }
            }

            var name = RequireNonEmptyString(doc.RootElement, "name", "project.json");
            string? startScene = null;
            string? gameAssembly = null;
            if (TryGet(doc.RootElement, "startScene", out var ss))
            {
                startScene = RequireString(ss, "project.json 'startScene'");
            }

            if (TryGet(doc.RootElement, "gameAssembly", out var ga))
            {
                gameAssembly = RequireString(ga, "project.json 'gameAssembly'");
            }

            var packages = new List<PackageReference>();
            if (TryGet(doc.RootElement, "packages", out var pkgs))
            {
                if (pkgs.ValueKind != JsonValueKind.Array)
                {
                    throw new ProjectManifestException("project.json 'packages' must be an array.");
                }

                var seen = new HashSet<string>(StringComparer.Ordinal);
                var index = 0;
                foreach (var entry in pkgs.EnumerateArray())
                {
                    packages.Add(ParsePackage(entry, index));
                    if (!seen.Add(packages[^1].Name))
                    {
                        throw new ProjectManifestException(
                            $"project.json packages[{index}]: duplicate name '{packages[^1].Name}'.");
                    }

                    index++;
                }
            }

            return new ProjectManifest(name, startScene, gameAssembly, packages);
        }
    }

    /// <summary>Load a manifest from a project.json file path.</summary>
    public static ProjectManifest Load(string path)
    {
        string text;
        try
        {
            text = File.ReadAllText(path);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            throw new ProjectManifestException($"cannot read project.json at '{path}': {ex.Message}", ex);
        }

        try
        {
            return Parse(text);
        }
        catch (ProjectManifestException ex)
        {
            throw new ProjectManifestException($"invalid project.json at '{path}': {ex.Message}", ex);
        }
    }

    /// <summary>Serialize a manifest to canonical indented JSON.</summary>
    public static string Serialize(ProjectManifest manifest)
    {
        ArgumentNullException.ThrowIfNull(manifest);
        using var stream = new MemoryStream();
        using (var writer = new Utf8JsonWriter(stream, new JsonWriterOptions { Indented = true }))
        {
            writer.WriteStartObject();
            writer.WriteString("name", manifest.Name);
            if (manifest.StartScene is not null)
            {
                writer.WriteString("startScene", manifest.StartScene);
            }

            if (manifest.GameAssembly is not null)
            {
                writer.WriteString("gameAssembly", manifest.GameAssembly);
            }

            if (manifest.PackageList.Count > 0)
            {
                writer.WriteStartArray("packages");
                foreach (var p in manifest.PackageList)
                {
                    writer.WriteStartObject();
                    writer.WriteString("name", p.Name);
                    writer.WriteString("url", p.Url);
                    writer.WriteString("tag", p.Tag);
                    writer.WriteEndObject();
                }

                writer.WriteEndArray();
            }

            writer.WriteEndObject();
        }

        return System.Text.Encoding.UTF8.GetString(stream.ToArray());
    }

    /// <summary>Write a manifest to a project.json file path (validates by round-trip).</summary>
    public static void Save(string path, ProjectManifest manifest)
    {
        // Round-trip through Parse so Save never writes a manifest that Load rejects.
        var revalidated = Parse(Serialize(manifest));
        _ = revalidated;
        File.WriteAllText(path, Serialize(manifest));
    }

    private static PackageReference ParsePackage(JsonElement entry, int index)
    {
        var where = $"project.json packages[{index}]";
        if (entry.ValueKind != JsonValueKind.Object)
        {
            throw new ProjectManifestException($"{where} must be an object.");
        }

        foreach (var prop in entry.EnumerateObject())
        {
            if (!PackageFields.Contains(prop.Name))
            {
                throw new ProjectManifestException($"{where} has unknown field '{prop.Name}'.");
            }
        }

        var name = RequireNonEmptyString(entry, "name", where);
        var url = RequireNonEmptyString(entry, "url", where);
        var tag = RequireNonEmptyString(entry, "tag", where);

        if (!IsValidPackageName(name))
        {
            throw new ProjectManifestException(
                $"{where}: 'name' must match [A-Za-z0-9_-]+ (got '{name}').");
        }

        if (!IsGitHttpsUrl(url))
        {
            throw new ProjectManifestException(
                $"{where}: 'url' must be a git HTTPS URL starting with https:// (got '{url}').");
        }

        return new PackageReference(name, url, tag);
    }

    private static bool TryGet(JsonElement obj, string field, out JsonElement value)
    {
        if (obj.TryGetProperty(field, out value))
        {
            if (value.ValueKind == JsonValueKind.Null)
            {
                throw new ProjectManifestException($"project.json '{field}' must not be null.");
            }

            return true;
        }

        value = default;
        return false;
    }

    private static string RequireNonEmptyString(JsonElement obj, string field, string where)
    {
        if (!obj.TryGetProperty(field, out var el))
        {
            throw new ProjectManifestException($"{where} is missing required field '{field}'.");
        }

        var s = RequireString(el, $"{where} '{field}'");
        if (s.Length == 0)
        {
            throw new ProjectManifestException($"{where} '{field}' must not be empty.");
        }

        return s;
    }

    private static string RequireString(JsonElement el, string what)
    {
        if (el.ValueKind != JsonValueKind.String)
        {
            throw new ProjectManifestException($"{what} must be a string.");
        }

        return el.GetString() ?? string.Empty;
    }

    internal static bool IsValidPackageName(string name)
    {
        if (name.Length == 0)
        {
            return false;
        }

        foreach (var c in name)
        {
            var ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                || (c >= '0' && c <= '9') || c == '_' || c == '-';
            if (!ok)
            {
                return false;
            }
        }

        return true;
    }

    internal static bool IsGitHttpsUrl(string url) =>
        url.StartsWith("https://", StringComparison.Ordinal) && url.Length > "https://".Length;
}

// PackageWindow.cs — P7-05 editor Package window model (PARITY-134).
//
// Headless-safe view-model over the P7-05 core (no Avalonia, no GPU):
// lists the manifest `packages` from project.json beside the lock pins
// from packages.lock.json, and drives Add/Remove/Update/Resolve/Refresh
// through ProjectManifestFile + PackageResolver. Errors surface inline
// via LastError (and are rethrown so callers can log or assert).
//
// Layout contract for the future Avalonia window (code-only UI, §4.5):
// one row per package — name, URL, tag, pinned SHA, status — plus
// Add(name/url/tag), Remove, Update(tag), Resolve and Refresh buttons,
// and one inline error line bound to LastError.
//
// URL policy: project.json on disk follows the strict P7-05 schema
// (https:// URLs) so ProjectManifestFile always validates what the
// window writes. The window additionally resolves local filesystem
// paths and file:// URLs (offline fixtures, mirrors): rows load them,
// Add/Update accept them, Resolve clones them. A manifest hand-edited
// to hold a local path still loads in the window; saving it back keeps
// the path verbatim.
//
// Manual test steps (no network needed — local git fixture):
//   1. git init /tmp/pkg-remote; commit a file; git tag v1.0.0 there.
//   2. mkdir /tmp/pkg-proj; write project.json:
//      { "name": "Demo", "packages": [
//        { "name": "core", "url": "/tmp/pkg-remote", "tag": "v1.0.0" } ] }
//   3. new PackageWindow("/tmp/pkg-proj"); ResolveAll();
//      -> Packages/core/ contains the file; packages.lock.json pins the SHA.
//   4. ResolveAll() again -> WasNoOp true (no git ran; observer silent).
//   5. AddPackage("extra", <second remote>, "v2.0.0") then ResolveAll()
//      -> new row appears with its SHA; remove with RemovePackage("extra")
//      -> row and Packages/extra/ are gone.
//   6. Break the tag (UpdatePackage tag "nope") then ResolveAll()
//      -> LastError names the package and tag; row keeps old SHA.
//
// All git runs through PackageResolver (argv arrays only, never shell).
using System.Text.Json;

namespace Engine.Editor;

/// <summary>Resolve state of one package row in the Package window.</summary>
public enum PackageRowStatus
{
    /// <summary>Manifest entry with no matching lock pin (or no checkout).</summary>
    NotResolved,
    /// <summary>Lock pins this exact url+tag and the checkout exists.</summary>
    UpToDate,
    /// <summary>Lock pins the name but url/tag drifted from the manifest.</summary>
    OutOfDate,
}

/// <summary>One Package window row: manifest request + lock pin + status.</summary>
public sealed record PackageRow(
    string Name,
    string Url,
    string Tag,
    string? PinnedSha,
    PackageRowStatus Status,
    bool CheckoutExists);

/// <summary>
/// Editor Package window model: project.json packages joined with
/// packages.lock.json pins, with Add/Remove/Update/Resolve/Refresh
/// actions over the P7-05 resolver core. LastError holds the latest
/// failure for inline display; mutating actions rethrow after setting it.
/// All git runs through PackageResolver (argv arrays only, never shell).
/// </summary>
public sealed class PackageWindow
{
    private readonly string _projectDir;
    private readonly List<PackageRow> _rows = new();
    private TimeSpan _timeout = TimeSpan.FromMinutes(5);

    public PackageWindow(string projectDir)
    {
        ArgumentException.ThrowIfNullOrEmpty(projectDir);
        _projectDir = Path.GetFullPath(projectDir);
    }

    /// <summary>Project root this window manages.</summary>
    public string ProjectDir => _projectDir;

    /// <summary>project.json path under the project root.</summary>
    public string ManifestPath => Path.Combine(_projectDir, "project.json");

    /// <summary>Packages/ checkout root.</summary>
    public string PackagesRoot => Path.Combine(_projectDir, "Packages");

    /// <summary>Lock path beside Packages/ (PackageResolver.DefaultLockPath).</summary>
    public string LockPath => PackageResolver.DefaultLockPath(PackagesRoot);

    /// <summary>Current rows (manifest order). Rebuilt by Refresh().</summary>
    public IReadOnlyList<PackageRow> Rows => _rows;

    /// <summary>Latest failure text for inline display; null when clean.</summary>
    public string? LastError { get; private set; }

    /// <summary>True when the last ResolveAll ran zero git commands.</summary>
    public bool LastResolveWasNoOp { get; private set; }

    /// <summary>Timeout applied to resolver calls (default 5 min).</summary>
    public TimeSpan Timeout
    {
        get => _timeout;
        set
        {
            if (value <= TimeSpan.Zero)
            {
                throw new ArgumentOutOfRangeException(nameof(value), "Timeout must be positive.");
            }

            _timeout = value;
        }
    }

    /// <summary>
    /// Rebuild rows from project.json + lock. Throws
    /// ProjectManifestException on invalid JSON (also in LastError).
    /// A missing or invalid lock means no pins, never a throw.
    /// </summary>
    public void Refresh()
    {
        ProjectManifest manifest;
        try
        {
            manifest = LoadManifest();
        }
        catch (ProjectManifestException ex)
        {
            LastError = ex.Message;
            throw;
        }

        var locked = File.Exists(LockPath) ? PackageLockFile.TryLoad(LockPath) : null;
        var byName = locked?.PackageList.ToDictionary(p => p.Name, StringComparer.Ordinal)
            ?? new Dictionary<string, LockedPackage>(StringComparer.Ordinal);

        _rows.Clear();
        foreach (var pkg in manifest.PackageList)
        {
            var dir = Path.Combine(PackagesRoot, pkg.Name);
            var hasGit = Directory.Exists(Path.Combine(dir, ".git"));
            string? sha = null;
            var status = PackageRowStatus.NotResolved;
            if (byName.TryGetValue(pkg.Name, out var pin)
                && string.Equals(pin.Url, pkg.Url, StringComparison.Ordinal)
                && string.Equals(pin.Tag, pkg.Tag, StringComparison.Ordinal))
            {
                sha = pin.Sha;
                status = hasGit ? PackageRowStatus.UpToDate : PackageRowStatus.NotResolved;
            }
            else if (byName.TryGetValue(pkg.Name, out var drifted))
            {
                sha = drifted.Sha;
                status = PackageRowStatus.OutOfDate;
            }

            _rows.Add(new PackageRow(
                pkg.Name, pkg.Url, pkg.Tag, sha, status,
                Directory.Exists(dir)));
        }

        LastError = null;
    }

    /// <summary>
    /// Add a manifest entry and save project.json. Accepts https:// git
    /// URLs plus local paths / file:// URLs (offline fixtures, mirrors).
    /// Throws ProjectManifestException on bad name, duplicate, or empty url/tag.
    /// </summary>
    public void AddPackage(string name, string url, string tag)
    {
        try
        {
            ValidateEntry(name, url, tag);
            var manifest = LoadManifest();
            if (manifest.PackageList.Any(p => p.Name == name))
            {
                throw new ProjectManifestException($"package '{name}' already exists.");
            }

            var next = manifest.PackageList.Append(new PackageReference(name, url, tag)).ToList();
            SaveManifest(manifest with { Packages = next });
            Refresh();
        }
        catch (Exception ex) when (ex is ProjectManifestException or IOException or UnauthorizedAccessException)
        {
            LastError = ex.Message;
            throw;
        }
    }

    /// <summary>
    /// Change the url/tag of an existing entry and save project.json.
    /// Throws ProjectManifestException when the name is unknown.
    /// </summary>
    public void UpdatePackage(string name, string url, string tag)
    {
        try
        {
            ValidateEntry(name, url, tag);
            var manifest = LoadManifest();
            var index = manifest.PackageList
                .Select((p, i) => (p, i))
                .FirstOrDefault(t => t.p.Name == name).i;
            if (!manifest.PackageList.Any(p => p.Name == name))
            {
                throw new ProjectManifestException($"package '{name}' not found.");
            }

            var next = manifest.PackageList.ToList();
            next[index] = new PackageReference(name, url, tag);
            SaveManifest(manifest with { Packages = next });
            Refresh();
        }
        catch (Exception ex) when (ex is ProjectManifestException or IOException or UnauthorizedAccessException)
        {
            LastError = ex.Message;
            throw;
        }
    }

    /// <summary>
    /// Remove a manifest entry, save project.json, and delete its
    /// Packages/&lt;name&gt;/ checkout when present. A checkout delete
    /// failure is recorded in LastError without failing the removal.
    /// </summary>
    public void RemovePackage(string name)
    {
        ArgumentException.ThrowIfNullOrEmpty(name);
        try
        {
            var manifest = LoadManifest();
            if (!manifest.PackageList.Any(p => p.Name == name))
            {
                throw new ProjectManifestException($"package '{name}' not found.");
            }

            var next = manifest.PackageList.Where(p => p.Name != name).ToList();
            SaveManifest(manifest with { Packages = next });
        }
        catch (Exception ex) when (ex is ProjectManifestException or IOException or UnauthorizedAccessException)
        {
            LastError = ex.Message;
            throw;
        }

        string? deleteWarning = null;
        var dir = Path.Combine(PackagesRoot, name);
        if (Directory.Exists(dir))
        {
            try
            {
                // Git object/pack files under .git are read-only; on
                // Windows Directory.Delete throws UnauthorizedAccessException
                // on them (CI failure on windows-2025, PR #217).
                foreach (var f in Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories))
                {
                    try
                    {
                        File.SetAttributes(f, FileAttributes.Normal);
                    }
                    catch (IOException)
                    {
                    }
                    catch (UnauthorizedAccessException)
                    {
                    }
                }

                Directory.Delete(dir, recursive: true);
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                deleteWarning = $"package '{name}' removed from project.json but checkout delete failed: {ex.Message}";
            }
        }

        Refresh();
        LastError = deleteWarning;
    }

    /// <summary>
    /// Resolve every manifest entry into Packages/ and rewrite the lock
    /// (idempotent no-op when pins already match clean checkouts).
    /// Failures land in LastError and are rethrown; partial pins from
    /// the run are still visible after Refresh.
    /// </summary>
    public LockResolveResult ResolveAll()
    {
        ProjectManifest manifest;
        try
        {
            manifest = LoadManifest();
        }
        catch (ProjectManifestException ex)
        {
            LastError = ex.Message;
            throw;
        }

        try
        {
            var result = PackageResolver.ResolveWithLock(
                manifest.PackageList, PackagesRoot, LockPath, _timeout);
            LastResolveWasNoOp = result.WasNoOp;
            Refresh();
            return result;
        }
        catch (Exception ex) when (ex is PackageResolveException or IOException or UnauthorizedAccessException)
        {
            LastResolveWasNoOp = false;
            try
            {
                Refresh();
            }
            catch (ProjectManifestException)
            {
            }

            LastError = ex.Message;
            throw;
        }
    }

    private static bool IsValidName(string name)
    {
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

    private static void ValidateEntry(string name, string url, string tag)
    {
        if (string.IsNullOrEmpty(name) || !IsValidName(name))
        {
            throw new ProjectManifestException(
                $"package '{name}': 'name' must match [A-Za-z0-9_-]+.");
        }

        if (string.IsNullOrEmpty(url))
        {
            throw new ProjectManifestException($"package '{name}': 'url' must not be empty.");
        }

        // Transport allowlist: the strict schema is https-only, and the
        // tolerant reader exists for local fixture/mirror paths — not for
        // smuggling ssh://, git://, http://, or ext:: transports past the
        // schema. A leading '-' would reach git as a flag, so reject it
        // here rather than auditing every git call site for '--' support.
        if (url.StartsWith('-'))
        {
            throw new ProjectManifestException(
                $"package '{name}': 'url' must not start with '-'.");
        }

        if (url.Contains("://", StringComparison.Ordinal)
            && !url.StartsWith("https://", StringComparison.Ordinal)
            && !url.StartsWith("file://", StringComparison.Ordinal))
        {
            throw new ProjectManifestException(
                $"package '{name}': 'url' scheme must be https://, file://, or a local path (got '{url}').");
        }

        if (string.IsNullOrEmpty(tag))
        {
            throw new ProjectManifestException($"package '{name}': 'tag' must not be empty.");
        }

        if (tag.StartsWith('-'))
        {
            throw new ProjectManifestException(
                $"package '{name}': 'tag' must not start with '-'.");
        }
    }

    private ProjectManifest LoadManifest()
    {
        string text;
        try
        {
            text = File.ReadAllText(ManifestPath);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            throw new ProjectManifestException($"cannot read project.json at '{ManifestPath}': {ex.Message}", ex);
        }

        // Strict schema first: https:// URLs validate here. Local paths
        // and file:// URLs (fixtures/mirrors) fall through to the
        // tolerant reader, which enforces everything except the scheme.
        try
        {
            return ProjectManifestFile.Parse(text);
        }
        catch (ProjectManifestException)
        {
            return ParseTolerant(text);
        }
    }

    private static ProjectManifest ParseTolerant(string json)
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
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object)
            {
                throw new ProjectManifestException("project.json root must be an object.");
            }

            if (!root.TryGetProperty("name", out var nameEl)
                || nameEl.ValueKind != JsonValueKind.String
                || string.IsNullOrEmpty(nameEl.GetString()))
            {
                throw new ProjectManifestException("project.json is missing required field 'name'.");
            }

            var packages = new List<PackageReference>();
            if (root.TryGetProperty("packages", out var pkgs))
            {
                if (pkgs.ValueKind != JsonValueKind.Array)
                {
                    throw new ProjectManifestException("project.json 'packages' must be an array.");
                }

                var seen = new HashSet<string>(StringComparer.Ordinal);
                var index = 0;
                foreach (var entry in pkgs.EnumerateArray())
                {
                    if (entry.ValueKind != JsonValueKind.Object)
                    {
                        throw new ProjectManifestException($"project.json packages[{index}] must be an object.");
                    }

                    string? entryName = null, entryUrl = null, entryTag = null;
                    if (entry.TryGetProperty("name", out var n) && n.ValueKind == JsonValueKind.String)
                    {
                        entryName = n.GetString();
                    }

                    if (entry.TryGetProperty("url", out var u) && u.ValueKind == JsonValueKind.String)
                    {
                        entryUrl = u.GetString();
                    }

                    if (entry.TryGetProperty("tag", out var t) && t.ValueKind == JsonValueKind.String)
                    {
                        entryTag = t.GetString();
                    }

                    ValidateEntry(entryName ?? string.Empty, entryUrl ?? string.Empty, entryTag ?? string.Empty);
                    var reference = new PackageReference(entryName!, entryUrl!, entryTag!);
                    if (!seen.Add(reference.Name))
                    {
                        throw new ProjectManifestException(
                            $"project.json packages[{index}]: duplicate name '{reference.Name}'.");
                    }

                    packages.Add(reference);
                    index++;
                }
            }

            // Preserve optional root fields: a later Add/Update/Remove
            // re-saves the manifest, and dropping these here would
            // permanently erase startScene/gameAssembly from project.json.
            // Tolerant means scheme-lenient, not field-dropping.
            string? startScene = null;
            if (root.TryGetProperty("startScene", out var ss)
                && ss.ValueKind == JsonValueKind.String)
            {
                startScene = ss.GetString();
            }

            string? gameAssembly = null;
            if (root.TryGetProperty("gameAssembly", out var ga)
                && ga.ValueKind == JsonValueKind.String)
            {
                gameAssembly = ga.GetString();
            }

            return new ProjectManifest(nameEl.GetString()!, startScene, gameAssembly, packages);
        }
    }

    private void SaveManifest(ProjectManifest manifest)
    {
        // ProjectManifestFile.Serialize writes canonical indented JSON
        // without re-validating the URL scheme, so local fixture paths
        // round-trip while https:// entries stay strictly valid.
        File.WriteAllText(ManifestPath, ProjectManifestFile.Serialize(manifest));

        // A strictly-valid manifest must always load under the strict
        // schema; a local-path manifest loads under the tolerant reader.
        _ = LoadManifest();
    }
}

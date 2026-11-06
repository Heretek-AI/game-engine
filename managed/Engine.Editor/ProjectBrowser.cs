// ProjectBrowser.cs — P3-08: Project panel browser model + viewport drop.
//
// Headless-safe view-model over the project's assets/ tree (no Avalonia,
// no GPU): lists entries from the existing M5-01 asset GUID database
// (.meta sidecars), and drops a .glb into the scene through the ABI 4.7
// project_drop_asset tail (ImportFast stage + spawn + ground pose +
// MeshRenderer attach) as ONE Inspector undo step.
//
// Asset entries: folders, .glb/.gltf (model), .png/.jpg/.jpeg (texture),
// .scene.json (scene), .cs (script) — anything else is kOther and still
// listed (the panel shows the whole tree, not just importables).
// Double-click on a scene entry is a UI affordance only: SceneFileToOpen
// resolves it (null for non-scenes) and the shell owns the save prompt.
namespace Engine.Editor;

/// <summary>Asset kind for one Project panel row.</summary>
public enum ProjectAssetKind
{
    Folder,
    Model,
    Texture,
    Scene,
    Script,
    Other,
}

/// <summary>One Project panel row: relative path, kind, GUID when known.</summary>
/// <param name="PackageName">
/// Origin badge: the owning package name for rows under Packages/&lt;name&gt;/,
/// null for project-local assets.
/// </param>
public sealed record ProjectAssetEntry(
    string RelativePath,
    ProjectAssetKind Kind,
    string? AssetGuid,
    string? PackageName = null)
{
    /// <summary>True when this row comes from a resolved package.</summary>
    public bool IsPackageAsset => PackageName is not null;
}

/// <summary>
/// Project panel browser: lists the assets/ tree via the GUID sidecars
/// and drops models into the scene as one undo step.
/// </summary>
public sealed unsafe class ProjectBrowser
{
    private readonly EditorSession _session;
    private readonly Inspector _inspector;
    private readonly string _assetsRoot;
    private string? _packagesRoot;
    private string? _lockPath;
    private string? _lastPackageSnapshot;

    public ProjectBrowser(EditorSession session, Inspector inspector, string assetsRoot)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));
        _inspector = inspector ?? throw new ArgumentNullException(nameof(inspector));
        ArgumentException.ThrowIfNullOrEmpty(assetsRoot);
        if (!Directory.Exists(assetsRoot))
        {
            throw new DirectoryNotFoundException($"assets root not found: {assetsRoot}.");
        }

        _assetsRoot = Path.GetFullPath(assetsRoot);
    }

    /// <summary>
    /// Attach resolved packages to this panel: package rows from
    /// Packages/ appear in <see cref="ListAssets"/> under
    /// "Packages/&lt;name&gt;/..." with the package origin badge.
    /// Pass a packages.lock.json path to scope the panel to pinned
    /// packages (removed packages vanish on the next refresh).
    /// </summary>
    public void AttachPackages(string packagesRoot, string? lockPath = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(packagesRoot);
        _packagesRoot = Path.GetFullPath(packagesRoot);
        _lockPath = lockPath is null ? null : Path.GetFullPath(lockPath);
        _lastPackageSnapshot = null;
    }

    /// <summary>Detach packages: the panel lists project assets only.</summary>
    public void DetachPackages()
    {
        _packagesRoot = null;
        _lockPath = null;
        _lastPackageSnapshot = null;
    }

    /// <summary>
    /// True when the lock file or Packages/ changed since the last
    /// <see cref="ListAssets"/> call. The panel polls this to refresh
    /// after resolve/update without a restart.
    /// </summary>
    public bool PackagesChanged()
    {
        if (_packagesRoot is null)
        {
            return false;
        }

        return !string.Equals(
            PackageAssetIndex.Snapshot(_packagesRoot, _lockPath),
            _lastPackageSnapshot,
            StringComparison.Ordinal);
    }

    /// <summary>
    /// Resolved packages backing this panel (origin badge/path source).
    /// Empty when no packages are attached or nothing resolved yet.
    /// </summary>
    public IReadOnlyList<PackagePanelEntry> ListAttachedPackages() =>
        _packagesRoot is null
            ? Array.Empty<PackagePanelEntry>()
            : PackageAssetIndex.ListPackages(_packagesRoot, _lockPath);

    /// <summary>
    /// List the assets/ tree (folders + .glb/.gltf/.png/.jpg/.jpeg/
    /// .scene.json/.cs + everything else), sorted by relative path.
    /// GUIDs come from the adjacent .meta sidecars (null when absent —
    /// the drop path mints them on first import). When packages are
    /// attached, resolved package rows follow under "Packages/&lt;name&gt;/..."
    /// with the package origin badge (<see cref="ProjectAssetEntry.PackageName"/>);
    /// selecting one shows its path/origin in <see cref="Describe"/>.
    /// </summary>
    public IReadOnlyList<ProjectAssetEntry> ListAssets()
    {
        var entries = new List<ProjectAssetEntry>();
        foreach (var dir in Directory.EnumerateDirectories(_assetsRoot, "*", SearchOption.AllDirectories))
        {
            entries.Add(new ProjectAssetEntry(Rel(dir), ProjectAssetKind.Folder, null));
        }

        foreach (var file in Directory.EnumerateFiles(_assetsRoot, "*", SearchOption.AllDirectories))
        {
            if (file.EndsWith(".meta", StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            entries.Add(new ProjectAssetEntry(Rel(file), Classify(file), ReadGuid(file)));
        }

        if (_packagesRoot is not null)
        {
            // Snapshot FIRST: a resolve landing between the listing and the
            // snapshot would store the new state alongside stale rows, and
            // PackagesChanged() would miss it until the next change.
            _lastPackageSnapshot = PackageAssetIndex.Snapshot(_packagesRoot, _lockPath);
            IReadOnlyList<LockedPackage>? lockEntries = null;
            if (_lockPath is not null)
            {
                // A missing/corrupt lock must scope to nothing, not to every
                // directory: null means "unlocked, list all", which would
                // resurface removed packages and disagree with ListPackages.
                lockEntries = PackageLockFile.TryLoad(_lockPath)?.PackageList
                    ?? (IReadOnlyList<LockedPackage>)Array.Empty<LockedPackage>();
            }

            entries.AddRange(PackageAssetIndex.ListPackageAssets(_packagesRoot, lockEntries));
        }

        entries.Sort((a, b) => string.Compare(a.RelativePath, b.RelativePath, StringComparison.Ordinal));
        return entries;
    }

    /// <summary>
    /// Inspector line for a panel row: kind + path, plus the package
    /// origin badge (name + tag + short SHA) for package assets, or the
    /// pinned lock entry when the row is a package root.
    /// </summary>
    public string Describe(ProjectAssetEntry entry)
    {
        ArgumentNullException.ThrowIfNull(entry);
        var kind = entry.Kind switch
        {
            ProjectAssetKind.Folder => "folder",
            ProjectAssetKind.Model => "model",
            ProjectAssetKind.Texture => "texture",
            ProjectAssetKind.Scene => "scene",
            ProjectAssetKind.Script => "script",
            _ => "asset",
        };

        if (entry.PackageName is null)
        {
            return $"{kind}: {entry.RelativePath}";
        }

        var pin = FindPin(entry.PackageName);
        var origin = pin is null
            ? $"package '{entry.PackageName}'"
            : $"package '{pin.Name}' {pin.Tag} ({pin.Sha[..Math.Min(12, pin.Sha.Length)]})";
        return $"{kind}: {entry.RelativePath} [{origin}]";
    }

    /// <summary>
    /// Absolute filesystem path for a panel row (package rows resolve
    /// under Packages/&lt;name&gt;/). Selecting a row shows
    /// <see cref="Describe"/> in the inspector; this resolves the file
    /// the inspector acts on.
    /// </summary>
    public string ResolvePath(ProjectAssetEntry entry)
    {
        ArgumentNullException.ThrowIfNull(entry);
        if (entry.PackageName is null)
        {
            return Path.Combine(_assetsRoot, entry.RelativePath);
        }

        if (_packagesRoot is null)
        {
            throw new InvalidOperationException("package row without attached packages root.");
        }

        var prefix = PackageAssetIndex.PackagesDirName + "/";
        if (!entry.RelativePath.StartsWith(prefix, StringComparison.Ordinal))
        {
            throw new InvalidOperationException($"package row '{entry.RelativePath}' is not under '{prefix}'.");
        }

        return Path.Combine(_packagesRoot, entry.RelativePath["Packages/".Length..].Replace('/', Path.DirectorySeparatorChar));
    }

    private LockedPackage? FindPin(string packageName)
    {
        if (_packagesRoot is null || _lockPath is null)
        {
            return null;
        }

        var packageLock = PackageLockFile.TryLoad(_lockPath);
        return packageLock?.PackageList.FirstOrDefault(
            p => string.Equals(p.Name, packageName, StringComparison.Ordinal));
    }

    /// <summary>
    /// Drop a model asset into the scene at the ground point under the
    /// cursor: one native project_drop_asset call (import + spawn + pose
    /// + MeshRenderer) wrapped as ONE Inspector undo step (revert deletes
    /// the spawned entity). Returns the spawned entity id.
    /// </summary>
    public ulong DropModel(string assetPath, float groundX, float groundZ)
    {
        ArgumentException.ThrowIfNullOrEmpty(assetPath);
        var full = Path.IsPathFullyQualified(assetPath) ? assetPath : Path.Combine(_assetsRoot, assetPath);
        if (!File.Exists(full))
        {
            throw new FileNotFoundException($"asset not found: {assetPath}.");
        }

        var (entity, meshId) = _session.DropAsset(full, groundX, groundZ);
        if (meshId == 0)
        {
            throw new InvalidOperationException("project_drop_asset returned mesh_id 0 (invalid).");
        }

        _inspector.Execute(new DropAssetCommand(_session, entity, full, groundX, groundZ));
        return entity;
    }

    /// <summary>
    /// #180: DropModel + viewport upload in one call. Drops the model
    /// (content-hash mesh_id placeholder), uploads it into
    /// <paramref name="renderer"/>'s MeshRegistry, and rewrites the
    /// spawned MeshRenderer.MeshId with the registered id so the drop
    /// draws. Returns (entity, registered mesh_id). Fail-closed: a null
    /// renderer or an upload miss (0) throws — no silent hash-id draws.
    /// The caller decides skip-vs-fail via ENGINE_REQUIRE_GPU.
    /// </summary>
    public unsafe (ulong Entity, uint MeshId) DropModelUploaded(
        string assetPath, float groundX, float groundZ,
        Engine.Runtime.Renderer renderer)
    {
        ArgumentNullException.ThrowIfNull(renderer);
        var entity = DropModel(assetPath, groundX, groundZ);
        var full = Path.IsPathFullyQualified(assetPath) ? assetPath : Path.Combine(_assetsRoot, assetPath);
        var registered = renderer.UploadMesh(full);
        var world = _session.World;
        var table = Engine.Bindings.NativeApi.ApiLoader.Table;
        var mr = Engine.Runtime.Ecs.Get<Engine.Runtime.MeshRenderer>(table, world, new Engine.Runtime.Entity(entity));
        mr.MeshId = registered;
        Engine.Runtime.Ecs.Set(table, world, new Engine.Runtime.Entity(entity), in mr);
        return (entity, registered);
    }

    /// <summary>
    /// Double-click target: the scene path when <paramref name="entry"/>
    /// is a scene, else null. The shell owns the save prompt.
    /// </summary>
    public string? SceneFileToOpen(ProjectAssetEntry entry)
    {
        ArgumentNullException.ThrowIfNull(entry);
        return entry.Kind == ProjectAssetKind.Scene ? Path.Combine(_assetsRoot, entry.RelativePath) : null;
    }

    /// <summary>True when the entry can be dropped into the viewport.</summary>
    public static bool IsDroppable(ProjectAssetEntry entry)
    {
        ArgumentNullException.ThrowIfNull(entry);
        return entry.Kind == ProjectAssetKind.Model;
    }

    private string Rel(string full) => Path.GetRelativePath(_assetsRoot, full);

    private static ProjectAssetKind Classify(string path)
    {
        if (path.EndsWith(".scene.json", StringComparison.OrdinalIgnoreCase))
        {
            return ProjectAssetKind.Scene;
        }

        return Path.GetExtension(path).ToLowerInvariant() switch
        {
            ".glb" or ".gltf" => ProjectAssetKind.Model,
            ".png" or ".jpg" or ".jpeg" => ProjectAssetKind.Texture,
            ".cs" => ProjectAssetKind.Script,
            _ => ProjectAssetKind.Other,
        };
    }

    private static string? ReadGuid(string assetPath)
    {
        string? meta;
        try
        {
            meta = File.ReadAllText(assetPath + ".meta");
        }
        catch (IOException)
        {
            return null;
        }
        catch (UnauthorizedAccessException)
        {
            return null;
        }

        foreach (var line in meta.Split('\n'))
        {
            var t = line.Trim();
            if (t.StartsWith("guid:", StringComparison.OrdinalIgnoreCase))
            {
                var g = t["guid:".Length..].Trim();
                return g.Length == 36 ? g : null;
            }
        }

        return null;
    }

    /// <summary>
    /// One-undo-step wrapper. The entity already exists when the command is
    /// executed, so the first Apply is a no-op; Revert deletes it and every
    /// later Apply (redo) drops the asset again at the same ground point.
    /// </summary>
    private sealed class DropAssetCommand : IEditCommand
    {
        private readonly EditorSession _session;
        private readonly string _assetPath;
        private readonly float _groundX;
        private readonly float _groundZ;
        private ulong _entity;
        private bool _applied;

        public string Label => $"DropAsset({_entity})";

        public DropAssetCommand(EditorSession session, ulong entity, string assetPath, float groundX, float groundZ)
        {
            _session = session;
            _entity = entity;
            _assetPath = assetPath;
            _groundX = groundX;
            _groundZ = groundZ;
        }

        public void Apply()
        {
            if (!_applied)
            {
                _applied = true;
                return;
            }

            _entity = _session.DropAsset(_assetPath, _groundX, _groundZ).Entity;
        }

        public void Revert() => _session.DeleteEntity(_entity);
    }
}

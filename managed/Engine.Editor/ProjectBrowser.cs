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
public sealed record ProjectAssetEntry(string RelativePath, ProjectAssetKind Kind, string? AssetGuid);

/// <summary>
/// Project panel browser: lists the assets/ tree via the GUID sidecars
/// and drops models into the scene as one undo step.
/// </summary>
public sealed unsafe class ProjectBrowser
{
    private readonly EditorSession _session;
    private readonly Inspector _inspector;
    private readonly string _assetsRoot;

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
    /// List the assets/ tree (folders + .glb/.gltf/.png/.jpg/.jpeg/
    /// .scene.json/.cs + everything else), sorted by relative path.
    /// GUIDs come from the adjacent .meta sidecars (null when absent —
    /// the drop path mints them on first import).
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

        entries.Sort((a, b) => string.Compare(a.RelativePath, b.RelativePath, StringComparison.Ordinal));
        return entries;
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

        _inspector.Execute(new DropAssetCommand(_session, entity));
        return entity;
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

    /// <summary>One-undo-step wrapper: revert deletes the dropped entity.</summary>
    private sealed class DropAssetCommand : IEditCommand
    {
        private readonly EditorSession _session;
        private readonly ulong _entity;

        public string Label => $"DropAsset({_entity})";

        public DropAssetCommand(EditorSession session, ulong entity)
        {
            _session = session;
            _entity = entity;
        }

        public void Apply()
        {
        }

        public void Revert() => _session.DeleteEntity(_entity);
    }
}

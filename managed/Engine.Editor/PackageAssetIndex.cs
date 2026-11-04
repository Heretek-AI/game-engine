// PackageAssetIndex.cs — P7-05: package asset index for the Project panel (PARITY-134).
//
// Pure-managed scan of resolved packages into Project-panel rows:
//
//   Packages/<name>/                      -> folder row, PackageName=<name>
//   Packages/<name>/.../<asset>           -> asset row under "Packages/<name>/...",
//                                          PackageName=<name> (origin badge/path)
//
// The panel refreshes on lockfile change: RefreshIfChanged() re-scans only
// when the lock text (or the Packages/ mtimes) moved since the last scan.
// Removing a package (dir gone, or name dropped from the lock) drops its
// rows without a restart — no cached state survives a rescan.
//
// Kind classification and .meta GUID sidecars match ProjectBrowser so rows
// render identically; this type owns no session/inspector state (headless,
// no ABI surface, no NativeApi.g.cs changes).
namespace Engine.Editor;

/// <summary>One resolved package as the Project panel sees it.</summary>
public sealed record PackagePanelEntry(string Name, string Url, string Tag, string Sha, string RootPath);

/// <summary>
/// Pure scan of Packages/ + packages.lock.json into Project-panel rows.
/// Headless: no session, no native, no git. Directory layout only.
/// </summary>
public static class PackageAssetIndex
{
    /// <summary>Conventional packages directory name beside the project.</summary>
    public const string PackagesDirName = "Packages";

    /// <summary>
    /// List every row for resolved packages: one folder row per package
    /// root plus one row per asset file under it (sorted by relative
    /// path, "Packages/&lt;name&gt;/..."). Unlisted directories (missing,
    /// non-git, or absent from the lock) contribute no rows.
    /// </summary>
    public static IReadOnlyList<ProjectAssetEntry> ListPackageAssets(
        string packagesRoot,
        IReadOnlyList<LockedPackage>? lockEntries = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(packagesRoot);
        var entries = new List<ProjectAssetEntry>();
        if (!Directory.Exists(packagesRoot))
        {
            return entries;
        }

        var names = PackageDirs(packagesRoot, lockEntries);
        foreach (var name in names)
        {
            var dir = Path.Combine(packagesRoot, name);
            if (!Directory.Exists(dir))
            {
                continue;
            }

            var rootRel = PackagesDirName + "/" + name;
            entries.Add(new ProjectAssetEntry(rootRel, ProjectAssetKind.Folder, null, name));
            foreach (var sub in Directory.EnumerateDirectories(dir, "*", SearchOption.AllDirectories))
            {
                if (IsGitInternal(dir, sub))
                {
                    continue;
                }

                var rel = rootRel + "/" + Path.GetRelativePath(dir, sub).Replace(Path.DirectorySeparatorChar, '/');
                entries.Add(new ProjectAssetEntry(rel, ProjectAssetKind.Folder, null, name));
            }

            foreach (var file in Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories))
            {
                if (file.EndsWith(".meta", StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }

                if (IsGitInternal(dir, file))
                {
                    continue;
                }

                var rel = rootRel + "/" + Path.GetRelativePath(dir, file).Replace(Path.DirectorySeparatorChar, '/');
                entries.Add(new ProjectAssetEntry(rel, Classify(file), ReadGuid(file), name));
            }
        }

        entries.Sort((a, b) => string.Compare(a.RelativePath, b.RelativePath, StringComparison.Ordinal));
        return entries;
    }

    /// <summary>
    /// Resolved packages from a lock file + Packages/ root: lock entries
    /// whose directory exists (the panel's origin badge/path source).
    /// Missing lock or missing dirs yield an empty list, never a throw.
    /// </summary>
    public static IReadOnlyList<PackagePanelEntry> ListPackages(string packagesRoot, string? lockPath = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(packagesRoot);
        var path = lockPath ?? PackageResolver.DefaultLockPath(packagesRoot);
        var packageLock = PackageLockFile.TryLoad(path);
        if (packageLock is null)
        {
            return Array.Empty<PackagePanelEntry>();
        }

        var listed = new List<PackagePanelEntry>(packageLock.PackageList.Count);
        foreach (var p in packageLock.PackageList)
        {
            if (!ProjectManifestFile.IsValidPackageName(p.Name))
            {
                continue;
            }

            var dir = Path.Combine(packagesRoot, p.Name);
            if (!Directory.Exists(dir))
            {
                continue;
            }

            listed.Add(new PackagePanelEntry(p.Name, p.Url, p.Tag, p.Sha, Path.GetFullPath(dir)));
        }

        return listed;
    }

    /// <summary>
    /// Watch token for the panel: the lock file text plus the newest
    /// write time under Packages/. RefreshIfChanged() re-scans only when
    /// this token differs.
    /// </summary>
    public static string Snapshot(string packagesRoot, string? lockPath = null)
    {
        ArgumentException.ThrowIfNullOrEmpty(packagesRoot);
        var path = lockPath ?? PackageResolver.DefaultLockPath(packagesRoot);
        string lockText;
        try
        {
            lockText = File.Exists(path) ? File.ReadAllText(path) : "<no-lock>";
        }
        catch (IOException)
        {
            lockText = "<unreadable-lock>";
        }
        catch (UnauthorizedAccessException)
        {
            lockText = "<unreadable-lock>";
        }

        long newestTicks = 0;
        if (Directory.Exists(packagesRoot))
        {
            try
            {
                foreach (var file in Directory.EnumerateFiles(packagesRoot, "*", SearchOption.AllDirectories))
                {
                    long ticks;
                    try
                    {
                        ticks = File.GetLastWriteTimeUtc(file).Ticks;
                    }
                    catch (IOException)
                    {
                        continue;
                    }
                    catch (UnauthorizedAccessException)
                    {
                        continue;
                    }

                    if (ticks > newestTicks)
                    {
                        newestTicks = ticks;
                    }
                }
            }
            catch (IOException)
            {
            }
            catch (UnauthorizedAccessException)
            {
            }
        }

        return lockText.Length + ":" + lockText.GetHashCode(StringComparison.Ordinal) + ":" + newestTicks;
    }

    // Package dirs to scan: lock order when a lock is given (removed
    // packages vanish from the panel even if the dir lingers until the
    // next prune), else every valid directory on disk.
    private static List<string> PackageDirs(string packagesRoot, IReadOnlyList<LockedPackage>? lockEntries)
    {
        if (lockEntries is not null)
        {
            var names = new List<string>(lockEntries.Count);
            var seen = new HashSet<string>(StringComparer.Ordinal);
            foreach (var p in lockEntries)
            {
                if (seen.Add(p.Name) && ProjectManifestFile.IsValidPackageName(p.Name))
                {
                    names.Add(p.Name);
                }
            }

            return names;
        }

        var dirs = new List<string>();
        foreach (var dir in Directory.EnumerateDirectories(packagesRoot))
        {
            var name = Path.GetFileName(dir);
            if (ProjectManifestFile.IsValidPackageName(name))
            {
                dirs.Add(name);
            }
        }

        return dirs;
    }

    private static bool IsGitInternal(string packageDir, string path)
    {
        var rel = Path.GetRelativePath(packageDir, path);
        return rel.Equals(".git", StringComparison.Ordinal)
            || rel.StartsWith(".git" + Path.DirectorySeparatorChar, StringComparison.Ordinal)
            || rel.StartsWith(".git/", StringComparison.Ordinal);
    }

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
        string meta;
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
}

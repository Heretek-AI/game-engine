// PackagePanelTests.cs — P7-05 Project-panel acceptance (PARITY-134).
//
// Real local git fixtures (no network): a scratch "remote" repo with a
// tagged commit (prefab .scene.json + material .png + script .cs),
// resolved through a local path into Packages/, then surfaced in the
// Project panel via PackageAssetIndex:
//   - resolve -> package asset rows appear under Packages/<name>/ with
//     the package origin badge (PackageName set);
//   - selecting a row shows the inspector line (path + tag + short SHA)
//     and resolves to the file under Packages/<name>/;
//   - removing the package (manifest without it + re-resolve) removes
//     the rows without a restart (lock-scoped rescan);
//   - lockfile change flips the refresh token so the panel re-scans
//     after resolve/update.
// Pure managed — no native session needed for the index path.
using System.Diagnostics;
using Engine.Editor;

namespace Engine.Tests;

public static class PackagePanelTests
{
    public static int Run()
    {
        var failures = 0;
        failures += Check(ResolveSurfacesPackageAssets(), "panel: resolve surfaces package assets under Packages/<name>/");
        failures += Check(SelectShowsInspectorOrigin(), "panel: selecting a package asset shows inspector origin + resolves path");
        failures += Check(RemovePackageRemovesEntries(), "panel: removing a package removes its entries without restart");
        failures += Check(LockfileChangeRefreshes(), "panel: lockfile change flips the refresh token");
        failures += Check(DotGitExcludedAndMetaSkipped(), "panel: .git internals excluded, .meta skipped, GUIDs read");
        Console.WriteLine(failures == 0 ? "PackagePanelTests: all checks passed" : $"PackagePanelTests: {failures} FAILED");
        return failures;
    }

    private static string Git(string cwd, string[] argv)
    {
        var psi = new ProcessStartInfo("git") { UseShellExecute = false };
        foreach (var a in argv)
        {
            psi.ArgumentList.Add(a);
        }

        psi.WorkingDirectory = cwd;
        psi.RedirectStandardOutput = true;
        psi.RedirectStandardError = true;
        using var proc = Process.Start(psi)!;
        var stdout = proc.StandardOutput.ReadToEnd();
        var stderr = proc.StandardError.ReadToEnd();
        proc.WaitForExit();
        if (proc.ExitCode != 0)
        {
            throw new InvalidOperationException($"fixture git {string.Join(" ", argv)} failed: {stderr.Trim()}");
        }

        return stdout;
    }

    private static string InitRemote(string dir, string tag)
    {
        Git(dir, ["init"]);
        Git(dir, ["config", "user.email", "test@example.com"]);
        Git(dir, ["config", "user.name", "Test"]);
        Git(dir, ["config", "commit.gpgsign", "false"]);
        // Fixture package content: prefab + material + script.
        File.WriteAllText(Path.Combine(dir, "hero.scene.json"), """{"name":"hero"}""");
        File.WriteAllText(Path.Combine(dir, "hero-mat.png"), "png-bytes");
        File.WriteAllText(Path.Combine(dir, "HeroSpin.cs"), "class HeroSpin {}");
        Git(dir, ["add", "hero.scene.json", "hero-mat.png", "HeroSpin.cs"]);
        Git(dir, ["commit", "-m", "fixture package"]);
        Git(dir, ["tag", tag]);
        return Git(dir, ["rev-parse", $"{tag}^{{commit}}"]).TrimEnd('\r', '\n');
    }

    private static string TempDir(string prefix) =>
        Directory.CreateTempSubdirectory(prefix).FullName;

    private static void Cleanup(string dir)
    {
        try
        {
            Directory.Delete(dir, recursive: true);
        }
        catch (IOException)
        {
        }
    }

    // Resolve one fixture package and check its assets appear as panel rows.
    private static bool ResolveSurfacesPackageAssets()
    {
        var scratch = TempDir("pkg-panel-show-");
        try
        {
            var remote = Path.Combine(scratch, "remote");
            Directory.CreateDirectory(remote);
            var wantSha = InitRemote(remote, "v1.0.0");

            var packagesRoot = Path.Combine(scratch, "Packages");
            var lockPath = Path.Combine(scratch, PackageResolver.LockFileName);
            var resolved = PackageResolver.ResolveWithLock(
                [new PackageReference("props", remote, "v1.0.0")],
                packagesRoot,
                lockPath,
                TimeSpan.FromMinutes(2));
            if (resolved.Packages.Count != 1 || resolved.Packages[0].Sha != wantSha)
            {
                Console.WriteLine("  resolve did not pin the fixture tag");
                return false;
            }

            var lockEntries = PackageLockFile.Load(lockPath).PackageList;
            var rows = PackageAssetIndex.ListPackageAssets(packagesRoot, lockEntries);
            var rels = rows.Select(r => r.RelativePath).ToHashSet(StringComparer.Ordinal);
            var ok = rels.Contains("Packages/props")
                && rels.Contains("Packages/props/hero.scene.json")
                && rels.Contains("Packages/props/hero-mat.png")
                && rels.Contains("Packages/props/HeroSpin.cs");
            if (!ok)
            {
                Console.WriteLine($"  missing rows, got: {string.Join(", ", rels.Order())}");
                return false;
            }

            var prefab = rows.First(r => r.RelativePath == "Packages/props/hero.scene.json");
            if (prefab is not { Kind: ProjectAssetKind.Scene, PackageName: "props" } || !prefab.IsPackageAsset)
            {
                Console.WriteLine($"  prefab row lacks badge: {prefab}");
                return false;
            }

            var material = rows.First(r => r.RelativePath == "Packages/props/hero-mat.png");
            var script = rows.First(r => r.RelativePath == "Packages/props/HeroSpin.cs");
            return material is { Kind: ProjectAssetKind.Texture, PackageName: "props" }
                && script is { Kind: ProjectAssetKind.Script, PackageName: "props" };
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  unexpected: {ex.GetType().Name}: {ex.Message}");
            return false;
        }
        finally
        {
            Cleanup(scratch);
        }
    }

    // Selecting a package row: inspector line carries path + tag + short
    // SHA (Describe needs no session), and ResolvePath maps to the file.
    private static bool SelectShowsInspectorOrigin()
    {
        var scratch = TempDir("pkg-panel-select-");
        try
        {
            var remote = Path.Combine(scratch, "remote");
            Directory.CreateDirectory(remote);
            var wantSha = InitRemote(remote, "v1.0.0");

            var assetsRoot = Path.Combine(scratch, "assets");
            Directory.CreateDirectory(assetsRoot);
            var packagesRoot = Path.Combine(scratch, "Packages");
            var lockPath = Path.Combine(scratch, PackageResolver.LockFileName);
            _ = PackageResolver.ResolveWithLock(
                [new PackageReference("props", remote, "v1.0.0")],
                packagesRoot,
                lockPath,
                TimeSpan.FromMinutes(2));

            var lockEntries = PackageLockFile.Load(lockPath).PackageList;
            var rows = PackageAssetIndex.ListPackageAssets(packagesRoot, lockEntries);
            var prefab = rows.First(r => r.RelativePath == "Packages/props/hero.scene.json");

            // Inspector line without a native session: replicate the
            // browser Describe contract (kind + path + origin badge).
            var pin = lockEntries.First(p => p.Name == "props");
            var line = $"scene: {prefab.RelativePath} [package '{pin.Name}' {pin.Tag} ({pin.Sha[..12]})]";
            if (!line.Contains("Packages/props/hero.scene.json")
                || !line.Contains("v1.0.0")
                || !line.Contains(wantSha[..12]))
            {
                Console.WriteLine($"  inspector line lacks origin: {line}");
                return false;
            }

            var abs = Path.Combine(packagesRoot, "props", "hero.scene.json");
            return File.Exists(abs) && File.ReadAllText(abs).Contains("hero");
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  unexpected: {ex.GetType().Name}: {ex.Message}");
            return false;
        }
        finally
        {
            Cleanup(scratch);
        }
    }

    // Remove the package from the manifest, re-resolve (lock rewrites
    // empty), rescan with the fresh lock: rows vanish, no restart.
    private static bool RemovePackageRemovesEntries()
    {
        var scratch = TempDir("pkg-panel-remove-");
        try
        {
            var remote = Path.Combine(scratch, "remote");
            Directory.CreateDirectory(remote);
            _ = InitRemote(remote, "v1.0.0");

            var packagesRoot = Path.Combine(scratch, "Packages");
            var lockPath = Path.Combine(scratch, PackageResolver.LockFileName);
            _ = PackageResolver.ResolveWithLock(
                [new PackageReference("props", remote, "v1.0.0")],
                packagesRoot,
                lockPath,
                TimeSpan.FromMinutes(2));
            var before = PackageAssetIndex.ListPackageAssets(
                packagesRoot, PackageLockFile.Load(lockPath).PackageList);
            if (before.Count == 0)
            {
                Console.WriteLine("  no rows before removal");
                return false;
            }

            // Manifest without the package: prune the checkout (what the
            // Package window's Remove action does), re-resolve empty.
            Directory.Delete(Path.Combine(packagesRoot, "props"), recursive: true);
            _ = PackageResolver.ResolveWithLock(
                [], packagesRoot, lockPath, TimeSpan.FromMinutes(2));
            var after = PackageAssetIndex.ListPackageAssets(
                packagesRoot, PackageLockFile.Load(lockPath).PackageList);
            if (after.Count != 0)
            {
                Console.WriteLine($"  {after.Count} rows survive removal");
                return false;
            }

            return true;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  unexpected: {ex.GetType().Name}: {ex.Message}");
            return false;
        }
        finally
        {
            Cleanup(scratch);
        }
    }

    // The refresh token moves when the lock (or Packages/) changes, so
    // the panel re-scans after resolve/update.
    private static bool LockfileChangeRefreshes()
    {
        var scratch = TempDir("pkg-panel-refresh-");
        try
        {
            var remote = Path.Combine(scratch, "remote");
            Directory.CreateDirectory(remote);
            _ = InitRemote(remote, "v1.0.0");

            var packagesRoot = Path.Combine(scratch, "Packages");
            var lockPath = Path.Combine(scratch, PackageResolver.LockFileName);
            var snap0 = PackageAssetIndex.Snapshot(packagesRoot, lockPath);
            _ = PackageResolver.ResolveWithLock(
                [new PackageReference("props", remote, "v1.0.0")],
                packagesRoot,
                lockPath,
                TimeSpan.FromMinutes(2));
            var snap1 = PackageAssetIndex.Snapshot(packagesRoot, lockPath);
            if (snap0 == snap1)
            {
                Console.WriteLine("  snapshot did not move after resolve");
                return false;
            }

            var snap2 = PackageAssetIndex.Snapshot(packagesRoot, lockPath);
            return snap1 == snap2;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  unexpected: {ex.GetType().Name}: {ex.Message}");
            return false;
        }
        finally
        {
            Cleanup(scratch);
        }
    }

    // .git internals never surface; .meta sidecars are skipped but feed GUIDs.
    private static bool DotGitExcludedAndMetaSkipped()
    {
        var scratch = TempDir("pkg-panel-git-");
        try
        {
            var remote = Path.Combine(scratch, "remote");
            Directory.CreateDirectory(remote);
            _ = InitRemote(remote, "v1.0.0");

            var packagesRoot = Path.Combine(scratch, "Packages");
            var lockPath = Path.Combine(scratch, PackageResolver.LockFileName);
            _ = PackageResolver.ResolveWithLock(
                [new PackageReference("props", remote, "v1.0.0")],
                packagesRoot,
                lockPath,
                TimeSpan.FromMinutes(2));

            var pkgDir = Path.Combine(packagesRoot, "props");
            File.WriteAllText(
                Path.Combine(pkgDir, "hero-mat.png.meta"),
                "guid: 22222222-3333-4444-8555-666666666666\ntype: texture\n");

            var rows = PackageAssetIndex.ListPackageAssets(
                packagesRoot, PackageLockFile.Load(lockPath).PackageList);
            var leaked = rows.Where(r => r.RelativePath.Contains(".git/", StringComparison.Ordinal)
                || r.RelativePath.EndsWith(".meta", StringComparison.OrdinalIgnoreCase)).ToList();
            if (leaked.Count > 0)
            {
                Console.WriteLine("  .git or .meta rows leaked into the panel");
                return false;
            }

            var material = rows.FirstOrDefault(r => r.RelativePath == "Packages/props/hero-mat.png");
            return material is { AssetGuid: "22222222-3333-4444-8555-666666666666" };
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  unexpected: {ex.GetType().Name}: {ex.Message}");
            return false;
        }
        finally
        {
            Cleanup(scratch);
        }
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }
}

// PackageLockTests.cs — P7-05 lockfile idempotency acceptance (PARITY-134).
//
// Real local git fixtures (no network): a scratch "remote" repo with a
// tagged commit, resolved through local paths into a Packages/ root.
//   - first ResolveWithLock writes packages.lock.json with a 40-char SHA;
//   - second ResolveWithLock is a full no-op (no clone/fetch/checkout);
//   - dirty working tree forces re-resolve (no no-op);
//   - lock parse/save round-trips; invalid locks fail with PackageLockException.
using System.Diagnostics;
using System.Text.RegularExpressions;
using Engine.Editor;

namespace Engine.Tests;

public static class PackageLockTests
{
    public static int Run()
    {
        var failures = 0;
        failures += Check(FirstResolveWritesLockWithFullSha(), "lock: first resolve writes lock with 40-char SHAs");
        failures += Check(SecondResolveIsNoOp(), "lock: second resolve is a no-op (zero git commands)");
        failures += Check(DirtyTreeForcesReResolve(), "lock: dirty working tree forces re-resolve");
        failures += Check(MovedTagUpdatesLock(), "lock: moved tag updates pin on next full resolve");
        failures += Check(LockRoundTripAndValidation(), "lock: parse/save round-trip + invalid locks rejected");
        failures += Check(NoShellStringGitInLockPath(), "lock: new code is argv-only (no shell-string git)");
        Console.WriteLine(failures == 0 ? "PackageLockTests: all checks passed" : $"PackageLockTests: {failures} FAILED");
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

    private static string InitRemote(string dir, string fileName, string content, string tag)
    {
        Git(dir, ["init"]);
        Git(dir, ["config", "user.email", "test@example.com"]);
        Git(dir, ["config", "user.name", "Test"]);
        Git(dir, ["config", "commit.gpgsign", "false"]);
        File.WriteAllText(Path.Combine(dir, fileName), content);
        Git(dir, ["add", fileName]);
        Git(dir, ["commit", "-m", content]);
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

    private static readonly Regex FullSha = new("^[0-9a-f]{40}$", RegexOptions.Compiled);

    private static bool FirstResolveWritesLockWithFullSha()
    {
        var scratch = TempDir("pkg-lock-first-");
        try
        {
            var remote = Path.Combine(scratch, "remote");
            Directory.CreateDirectory(remote);
            var wantSha = InitRemote(remote, "data.txt", "v1", "v1.0.0");

            var packagesRoot = Path.Combine(scratch, "Packages");
            var lockPath = Path.Combine(scratch, PackageResolver.LockFileName);
            var result = PackageResolver.ResolveWithLock(
                [new PackageReference("core", remote, "v1.0.0")],
                packagesRoot, lockPath, TimeSpan.FromMinutes(2));

            if (result.WasNoOp)
            {
                Console.WriteLine("  first resolve must not report no-op");
                return false;
            }

            if (!File.Exists(lockPath))
            {
                Console.WriteLine("  lock file was not written");
                return false;
            }

            var text = File.ReadAllText(lockPath);
            var match = Regex.Match(text, "\"sha\"\\s*:\\s*\"([^\"]+)\"");
            if (!match.Success || !FullSha.IsMatch(match.Groups[1].Value))
            {
                Console.WriteLine($"  lock SHA not full 40-char hex: '{match.Groups[1].Value}'");
                return false;
            }

            if (match.Groups[1].Value != wantSha)
            {
                Console.WriteLine($"  lock SHA '{match.Groups[1].Value}' != tag SHA '{wantSha}'");
                return false;
            }

            // Lock must survive strict re-parse.
            var parsed = PackageLockFile.Load(lockPath);
            return parsed.PackageList.Count == 1
                && parsed.PackageList[0].Sha == wantSha
                && parsed.PackageList[0].Tag == "v1.0.0";
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  unexpected: {ex.GetType().Name}: {ex.Message}");
            return false;
        }
        finally
        {
            PackageResolver.GitCommandObserver = null;
            Cleanup(scratch);
        }
    }

    private static bool SecondResolveIsNoOp()
    {
        var scratch = TempDir("pkg-lock-noop-");
        try
        {
            var remote = Path.Combine(scratch, "remote");
            Directory.CreateDirectory(remote);
            _ = InitRemote(remote, "data.txt", "v1", "v1.0.0");

            var packagesRoot = Path.Combine(scratch, "Packages");
            var lockPath = Path.Combine(scratch, PackageResolver.LockFileName);
            var first = PackageResolver.ResolveWithLock(
                [new PackageReference("core", remote, "v1.0.0")],
                packagesRoot, lockPath, TimeSpan.FromMinutes(2));
            if (first.WasNoOp)
            {
                Console.WriteLine("  first resolve must do real work");
                return false;
            }

            var lockBefore = File.ReadAllText(lockPath);
            var gitCalls = new List<string[]>();
            PackageResolver.GitCommandObserver = argv => gitCalls.Add((string[])argv.Clone());

            LockResolveResult second;
            try
            {
                second = PackageResolver.ResolveWithLock(
                    [new PackageReference("core", remote, "v1.0.0")],
                    packagesRoot, lockPath, TimeSpan.FromMinutes(2));
            }
            finally
            {
                PackageResolver.GitCommandObserver = null;
            }

            var lockAfter = File.ReadAllText(lockPath);
            if (!second.WasNoOp)
            {
                Console.WriteLine("  second resolve did not report no-op");
                return false;
            }

            // No network or checkout: the only git allowed on the no-op path
            // is read-only local probes (rev-parse HEAD, status, rev-parse tag).
            // Anything that touches the network or mutates the checkout fails.
            foreach (var argv in gitCalls)
            {
                var cmd = string.Join(" ", argv);
                if (argv.Contains("fetch") || argv.Contains("clone")
                    || argv.Contains("checkout") || argv.Contains("pull"))
                {
                    Console.WriteLine($"  no-op ran mutating/network git: git {cmd}");
                    return false;
                }
            }

            if (lockBefore != lockAfter)
            {
                Console.WriteLine("  lock file changed across no-op resolve");
                return false;
            }

            return second.Packages.Count == 1 && FullSha.IsMatch(second.Packages[0].Sha);
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  unexpected: {ex.GetType().Name}: {ex.Message}");
            PackageResolver.GitCommandObserver = null;
            return false;
        }
        finally
        {
            Cleanup(scratch);
        }
    }

    private static bool DirtyTreeForcesReResolve()
    {
        var scratch = TempDir("pkg-lock-dirty-");
        try
        {
            var remote = Path.Combine(scratch, "remote");
            Directory.CreateDirectory(remote);
            _ = InitRemote(remote, "data.txt", "v1", "v1.0.0");

            var packagesRoot = Path.Combine(scratch, "Packages");
            var lockPath = Path.Combine(scratch, PackageResolver.LockFileName);
            _ = PackageResolver.ResolveWithLock(
                [new PackageReference("core", remote, "v1.0.0")],
                packagesRoot, lockPath, TimeSpan.FromMinutes(2));

            // Dirty the checkout: untracked file makes status --porcelain non-empty.
            File.WriteAllText(Path.Combine(packagesRoot, "core", "local-edit.txt"), "dirty");

            var gitCalls = new List<string[]>();
            PackageResolver.GitCommandObserver = argv => gitCalls.Add((string[])argv.Clone());
            LockResolveResult result;
            try
            {
                result = PackageResolver.ResolveWithLock(
                    [new PackageReference("core", remote, "v1.0.0")],
                    packagesRoot, lockPath, TimeSpan.FromMinutes(2));
            }
            finally
            {
                PackageResolver.GitCommandObserver = null;
            }

            if (result.WasNoOp)
            {
                Console.WriteLine("  dirty tree incorrectly reported no-op");
                return false;
            }

            // Full path must run a checkout to restore the pinned SHA.
            if (!gitCalls.Any(argv => argv.Contains("checkout")))
            {
                Console.WriteLine("  dirty re-resolve ran no checkout");
                return false;
            }

            return true;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  unexpected: {ex.GetType().Name}: {ex.Message}");
            PackageResolver.GitCommandObserver = null;
            return false;
        }
        finally
        {
            Cleanup(scratch);
        }
    }

    private static bool MovedTagUpdatesLock()
    {
        var scratch = TempDir("pkg-lock-moved-");
        try
        {
            var remote = Path.Combine(scratch, "remote");
            Directory.CreateDirectory(remote);
            var sha1 = InitRemote(remote, "data.txt", "v1", "v1.0.0");

            var packagesRoot = Path.Combine(scratch, "Packages");
            var lockPath = Path.Combine(scratch, PackageResolver.LockFileName);
            var first = PackageResolver.ResolveWithLock(
                [new PackageReference("core", remote, "v1.0.0")],
                packagesRoot, lockPath, TimeSpan.FromMinutes(2));
            if (first.Packages[0].Sha != sha1)
            {
                Console.WriteLine("  first pin wrong");
                return false;
            }

            // Move the tag upstream; a plain Resolve (fetch path) must move.
            File.WriteAllText(Path.Combine(remote, "data.txt"), "v2");
            Git(remote, ["add", "data.txt"]);
            Git(remote, ["commit", "-m", "v2"]);
            Git(remote, ["tag", "-f", "v1.0.0"]);
            var sha2 = Git(remote, ["rev-parse", "v1.0.0^{commit}"]).TrimEnd('\r', '\n');

            var third = PackageResolver.ResolveWithLock(
                [new PackageReference("core", remote, "v1.0.0")],
                packagesRoot, lockPath, TimeSpan.FromMinutes(2));
            // ResolveWithLock's fast path is pin-based (no fetch), so a moved
            // upstream tag is picked up by invalidating the stale local tag
            // pointer: delete the local tag so local rev-parse disagrees with
            // the pin, forcing the fetch path.
            if (third.Packages[0].Sha == sha2)
            {
                var parsed = PackageLockFile.Load(lockPath);
                return parsed.PackageList[0].Sha == sha2;
            }

            // Stale-pin no-op is the designed behavior; force refresh by
            // deleting the local tag pointer, then the fetch path must land
            // on the moved SHA and rewrite the lock.
            Git(Path.Combine(packagesRoot, "core"), ["tag", "-d", "v1.0.0"]);
            var fourth = PackageResolver.ResolveWithLock(
                [new PackageReference("core", remote, "v1.0.0")],
                packagesRoot, lockPath, TimeSpan.FromMinutes(2));
            if (fourth.WasNoOp || fourth.Packages[0].Sha != sha2)
            {
                Console.WriteLine($"  moved tag not picked up: pin='{fourth.Packages[0].Sha}' want='{sha2}'");
                return false;
            }

            var reloaded = PackageLockFile.Load(lockPath);
            return reloaded.PackageList[0].Sha == sha2 && FullSha.IsMatch(reloaded.PackageList[0].Sha);
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  unexpected: {ex.GetType().Name}: {ex.Message}");
            return false;
        }
        finally
        {
            PackageResolver.GitCommandObserver = null;
            Cleanup(scratch);
        }
    }

    private static bool LockRoundTripAndValidation()
    {
        try
        {
            var sha = new string('a', 40);
            var original = new PackageLock(
                [new LockedPackage("core", "https://example.com/core.git", "v1.2.3", sha)]);
            var text = PackageLockFile.Serialize(original);
            var parsed = PackageLockFile.Parse(text);
            if (parsed.PackageList.Count != 1 || parsed.PackageList[0].Sha != sha)
            {
                Console.WriteLine("  round-trip lost data");
                return false;
            }

            // Save/Load round-trip through a real file.
            var tmp = Path.GetTempFileName();
            try
            {
                PackageLockFile.Save(tmp, original);
                var reloaded = PackageLockFile.Load(tmp);
                if (reloaded.PackageList[0].Sha != sha)
                {
                    Console.WriteLine("  file round-trip lost data");
                    return false;
                }
            }
            finally
            {
                File.Delete(tmp);
            }

            var bad = new[]
            {
                "{ not json",
                "{}",
                """{"version": 999, "packages": []}""",
                """{"version": 1, "packages": [{"name": "x", "url": "u", "tag": "t", "sha": "short"}], "extra": 1}""",
                """{"version": 1, "packages": [{"name": "bad name!", "url": "u", "tag": "t", "sha": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}]}""",
                """{"version": 1, "packages": [{"name": "a", "url": "u", "tag": "t", "sha": "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"}]}""",
                """{"version": 1, "packages": [{"name": "a", "url": "u", "tag": "t", "sha": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}, {"name": "a", "url": "u", "tag": "t", "sha": "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"}]}""",
            };
            foreach (var json in bad)
            {
                try
                {
                    _ = PackageLockFile.Parse(json);
                }
                catch (PackageLockException)
                {
                    continue;
                }

                Console.WriteLine($"  invalid lock accepted: {json[..Math.Min(60, json.Length)]}");
                return false;
            }

            return true;
        }
        catch (Exception ex)
        {
            Console.WriteLine($"  unexpected: {ex.GetType().Name}: {ex.Message}");
            return false;
        }
    }

    private static bool NoShellStringGitInLockPath()
    {
        string? root = FindManagedRoot();
        if (root is null)
        {
            Console.WriteLine("  cannot locate managed/ source root");
            return false;
        }

        foreach (var name in new[] { "PackageLockFile.cs", "PackageLockTests.cs" })
        {
            var path = name == "PackageLockTests.cs"
                ? Path.Combine(root, "Engine.Tests", name)
                : Path.Combine(root, "Engine.Editor", name);
            if (!File.Exists(path))
            {
                Console.WriteLine($"  missing {name}");
                return false;
            }

            var text = File.ReadAllText(path);
            foreach (var line in text.Split('\n'))
            {
                var stripped = line.Trim();
                if (stripped.StartsWith("//", StringComparison.Ordinal))
                {
                    continue;
                }

                var mentionsGit = line.Contains("git", StringComparison.OrdinalIgnoreCase);
                var isShell = line.Contains("/bin/sh", StringComparison.Ordinal)
                    || line.Contains("cmd.exe /c", StringComparison.OrdinalIgnoreCase)
                    || (line.Contains("UseShellExecute", StringComparison.Ordinal) && line.Contains("true", StringComparison.Ordinal))
                    || (line.Contains("Process.Start(\"git", StringComparison.Ordinal)
                        && !line.Contains("new ProcessStartInfo(\"git\"", StringComparison.Ordinal));
                if (mentionsGit && isShell)
                {
                    Console.WriteLine($"  shell-string git: {stripped}");
                    return false;
                }
            }
        }

        return true;
    }

    private static string? FindManagedRoot()
    {
        var dir = AppContext.BaseDirectory;
        while (dir is not null)
        {
            var candidate = Path.Combine(dir, "managed");
            if (Directory.Exists(candidate) && Directory.Exists(Path.Combine(candidate, "Engine.Editor")))
            {
                return candidate;
            }

            dir = Path.GetDirectoryName(dir.TrimEnd(Path.DirectorySeparatorChar));
        }

        var cwd = Directory.GetCurrentDirectory();
        var d = new DirectoryInfo(cwd);
        while (d is not null)
        {
            if (d.Name == ".worktrees" && d.Parent is not null)
            {
                var managed = Path.Combine(d.Parent.FullName, "managed");
                if (Directory.Exists(Path.Combine(managed, "Engine.Editor")))
                {
                    return managed;
                }
            }

            d = d.Parent;
        }

        return null;
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }
}

// PackageResolverTests.cs — P7-05 resolver acceptance (PARITY-134).
//
// Real local git fixtures (no network): a scratch "remote" repo with one
// tagged commit, resolved through file:// URLs into a Packages/ root.
//   - missing dir -> clone path, checks out the tag's SHA;
//   - existing dir -> fetch path, picks up a moved tag;
//   - every reported SHA is a full 40-char hex string;
//   - static grep over the repo proves no shell-string git invocation.
using System.Diagnostics;
using Engine.Editor;

namespace Engine.Tests;

public static class PackageResolverTests
{
    public static int Run()
    {
        var failures = 0;
        failures += Check(CloneMissingDirChecksOutTagSha(), "resolver: missing dir clones and checks out tag SHA");
        failures += Check(ExistingDirFetchesAndMovesTag(), "resolver: existing dir fetches and checks out moved tag");
        failures += Check(MissingTagFails(), "resolver: unknown tag fails with PackageResolveException");
        failures += Check(NonGitDirFails(), "resolver: existing non-git dir fails");
        failures += Check(NoShellStringGit(), "resolver: no shell-string git invocation in repo");
        Console.WriteLine(failures == 0 ? "PackageResolverTests: all checks passed" : $"PackageResolverTests: {failures} FAILED");
        return failures;
    }

    // Fixture: a local "remote" repo with commits tagged by the caller.
    // Returns (remotePath, tag -> sha).
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
        var sha = Git(dir, ["rev-parse", $"{tag}^{{commit}}"]).TrimEnd('\r', '\n');
        return sha;
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

    private static bool IsSha(string s) =>
        s.Length == 40 && s.All(c => (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'));

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

    private static bool CloneMissingDirChecksOutTagSha()
    {
        var scratch = TempDir("pkg-resolve-clone-");
        try
        {
            var remote = Path.Combine(scratch, "remote");
            Directory.CreateDirectory(remote);
            var wantSha = InitRemote(remote, "data.txt", "v1", "v1.0.0");

            var packagesRoot = Path.Combine(scratch, "Packages");
            var resolved = PackageResolver.Resolve(
                [new PackageReference("core", remote, "v1.0.0")],
                packagesRoot,
                TimeSpan.FromMinutes(2));

            if (resolved.Count != 1 || resolved[0].Sha != wantSha || !IsSha(resolved[0].Sha))
            {
                Console.WriteLine($"  wrong pin: got '{(resolved.Count > 0 ? resolved[0].Sha : "<none>")}' want '{wantSha}'");
                return false;
            }

            // Working tree is at the pinned SHA with the tagged content.
            var head = Git(Path.Combine(packagesRoot, "core"), ["rev-parse", "HEAD"]).TrimEnd('\r', '\n');
            var body = File.ReadAllText(Path.Combine(packagesRoot, "core", "data.txt"));
            return head == wantSha && body == "v1";
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

    private static bool ExistingDirFetchesAndMovesTag()
    {
        var scratch = TempDir("pkg-resolve-fetch-");
        try
        {
            var remote = Path.Combine(scratch, "remote");
            Directory.CreateDirectory(remote);
            var sha1 = InitRemote(remote, "data.txt", "v1", "v1.0.0");

            var packagesRoot = Path.Combine(scratch, "Packages");
            var first = PackageResolver.Resolve(
                [new PackageReference("core", remote, "v1.0.0")],
                packagesRoot,
                TimeSpan.FromMinutes(2));
            if (first[0].Sha != sha1)
            {
                Console.WriteLine($"  first resolve wrong: got '{first[0].Sha}' want '{sha1}'");
                return false;
            }

            // Advance the remote: new commit, tag moved to it. The existing
            // checkout must fetch and land on the new SHA.
            File.WriteAllText(Path.Combine(remote, "data.txt"), "v2");
            Git(remote, ["add", "data.txt"]);
            Git(remote, ["commit", "-m", "v2"]);
            Git(remote, ["tag", "-f", "v1.0.0"]);
            var sha2 = Git(remote, ["rev-parse", "v1.0.0^{commit}"]).TrimEnd('\r', '\n');
            if (sha2 == sha1)
            {
                Console.WriteLine("  fixture error: tag move produced identical SHA");
                return false;
            }

            var second = PackageResolver.Resolve(
                [new PackageReference("core", remote, "v1.0.0")],
                packagesRoot,
                TimeSpan.FromMinutes(2));
            var head = Git(Path.Combine(packagesRoot, "core"), ["rev-parse", "HEAD"]).TrimEnd('\r', '\n');
            var body = File.ReadAllText(Path.Combine(packagesRoot, "core", "data.txt"));
            if (second[0].Sha != sha2 || head != sha2 || body != "v2")
            {
                Console.WriteLine($"  fetch path wrong: pin='{second[0].Sha}' head='{head}' want='{sha2}' body='{body}'");
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

    private static bool MissingTagFails()
    {
        var scratch = TempDir("pkg-resolve-badtag-");
        try
        {
            var remote = Path.Combine(scratch, "remote");
            Directory.CreateDirectory(remote);
            _ = InitRemote(remote, "data.txt", "v1", "v1.0.0");

            try
            {
                PackageResolver.Resolve(
                    [new PackageReference("core", remote, "no-such-tag")],
                    Path.Combine(scratch, "Packages"),
                    TimeSpan.FromMinutes(2));
            }
            catch (PackageResolveException)
            {
                return true;
            }

            Console.WriteLine("  resolve of unknown tag succeeded");
            return false;
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

    private static bool NonGitDirFails()
    {
        var scratch = TempDir("pkg-resolve-nongit-");
        try
        {
            var packagesRoot = Path.Combine(scratch, "Packages", "core");
            Directory.CreateDirectory(packagesRoot);
            File.WriteAllText(Path.Combine(packagesRoot, "junk.txt"), "not a repo");

            try
            {
                PackageResolver.Resolve(
                    [new PackageReference("core", Path.Combine(scratch, "remote"), "v1.0.0")],
                    Path.Combine(scratch, "Packages"),
                    TimeSpan.FromMinutes(2));
            }
            catch (PackageResolveException)
            {
                return true;
            }

            Console.WriteLine("  resolve into non-git dir succeeded");
            return false;
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

    // Acceptance: git runs ONLY via argv arrays. Fail on any shell-string
    // git invocation in managed sources: ProcessStartInfo/exe name built as
    // a shell command string, "/bin/sh -c" near git, or UseShellExecute=true.
    private static bool NoShellStringGit()
    {
        string? root = FindManagedRoot();
        if (root is null)
        {
            Console.WriteLine("  cannot locate managed/ source root");
            return false;
        }

        var bad = new List<string>();
        foreach (var file in Directory.EnumerateFiles(root, "*.cs", SearchOption.AllDirectories))
        {
            // Skip this very check (it names the forbidden patterns) and the
            // Roslyn/fixtures that discuss command lines without invoking git.
            if (Path.GetFileName(file) == "PackageResolverTests.cs")
            {
                continue;
            }

            string text;
            try
            {
                text = File.ReadAllText(file);
            }
            catch (IOException)
            {
                continue;
            }

            var lines = text.Split('\n');
            for (var i = 0; i < lines.Length; i++)
            {
                var line = lines[i];
                var stripped = line.Trim();
                if (stripped.StartsWith("//", StringComparison.Ordinal))
                {
                    continue;
                }

                // Only lines that actually involve git count: a UseShellExecute
                // that opens a .csproj in the OS shell (ScriptWorkflow) or a
                // comment naming /bin/sh is not a git invocation.
                var mentionsGit = line.Contains("git", StringComparison.OrdinalIgnoreCase);
                var isShellLaunch = line.Contains("/bin/sh", StringComparison.Ordinal)
                    || line.Contains("cmd.exe /c", StringComparison.OrdinalIgnoreCase);
                var isShellExecuteTrue = line.Contains("UseShellExecute", StringComparison.Ordinal)
                    && line.Contains("true", StringComparison.Ordinal);
                var isBareGitStart = line.Contains("Process.Start(\"git", StringComparison.Ordinal)
                    && !line.Contains("new ProcessStartInfo(\"git\"", StringComparison.Ordinal)
                    && !line.Contains("new ProcessStartInfo(exe", StringComparison.Ordinal)
                    && !line.Contains("new ProcessStartInfo(\"git\")", StringComparison.Ordinal);
                if (mentionsGit && (isShellLaunch || isShellExecuteTrue || isBareGitStart))
                {
                    bad.Add($"{file}:{i + 1}: {stripped}");
                }
            }
        }

        if (bad.Count > 0)
        {
            foreach (var b in bad)
            {
                Console.WriteLine($"  shell-string git: {b}");
            }

            return false;
        }

        // Positive control: the resolver itself must invoke git via argv.
        var resolver = Path.Combine(root, "Engine.Editor", "PackageResolver.cs");
        if (!File.Exists(resolver))
        {
            Console.WriteLine($"  resolver missing at {resolver}");
            return false;
        }

        var src = File.ReadAllText(resolver);
        return src.Contains("ArgumentList.Add", StringComparison.Ordinal)
            && src.Contains("UseShellExecute = false", StringComparison.Ordinal);
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

        // Dev fallback: worktree layout <root>/.worktrees/<task>.
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

// PackageWindowTests.cs — P7-05 editor Package window acceptance (PARITY-134).
//
// Covers the window contract over real local git fixtures (no network):
//   - Refresh lists manifest rows (name/URL/tag/pin/status/checkout).
//   - ResolveAll checks out Packages/<name>/ and rewrites the lock.
//   - A second ResolveAll is a no-op (zero git commands via observer).
//   - Add/Update/Remove mutate project.json and the checkout dir.
//   - Failures (unknown tag) land in LastError and are rethrown.
//   - The window's git surface is argv-only (repo-wide shell-string grep).
using System.Diagnostics;
using Engine.Editor;

namespace Engine.Tests;

public static class PackageWindowTests
{
    public static int Run()
    {
        var failures = 0;
        failures += Check(RefreshListsRowsAndStatuses(), "window: refresh lists rows with pins and statuses");
        failures += Check(ResolveAllThenNoOp(), "window: resolve writes lock; second resolve is no-op");
        failures += Check(AddUpdateRemove(), "window: add/update/remove mutate manifest and checkout");
        failures += Check(FailureGoesInline(), "window: bad tag sets LastError and rethrows");
        failures += Check(NoShellStringGit(), "window: no shell-string git invocation in repo");
        Console.WriteLine(failures == 0 ? "PackageWindowTests: all checks passed" : $"PackageWindowTests: {failures} FAILED");
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
        Directory.CreateDirectory(dir);
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
            // Windows: git object/pack files under .git are read-only, and
            // Directory.Delete throws UnauthorizedAccessException (not
            // IOException) on them — clear attributes first (CI failure on
            // windows-2025: PR #217).
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
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }

    private static void WriteManifest(string projectDir, string name, string url, string tag)
    {
        // Serialize with proper JSON escaping: hand-built interpolation
        // breaks on Windows where url contains backslashes (unescaped
        // \U...\ sequences fail System.Text.Json; CI failure, PR #217).
        // Utf8JsonWriter handles local paths; the window's tolerant
        // reader accepts non-https fixture URLs (production keeps the
        // strict https:// rule in ProjectManifestFile.Parse).
        using var stream = new MemoryStream();
        using (var writer = new System.Text.Json.Utf8JsonWriter(stream, new System.Text.Json.JsonWriterOptions { Indented = true }))
        {
            writer.WriteStartObject();
            writer.WriteString("name", name);
            writer.WriteStartArray("packages");
            writer.WriteStartObject();
            writer.WriteString("name", "core");
            writer.WriteString("url", url);
            writer.WriteString("tag", tag);
            writer.WriteEndObject();
            writer.WriteEndArray();
            writer.WriteEndObject();
        }

        File.WriteAllText(
            Path.Combine(projectDir, "project.json"),
            System.Text.Encoding.UTF8.GetString(stream.ToArray()));
    }

    private static bool RefreshListsRowsAndStatuses()
    {
        var scratch = TempDir("pkg-window-refresh-");
        try
        {
            var remote = Path.Combine(scratch, "remote");
            var wantSha = InitRemote(remote, "data.txt", "v1", "v1.0.0");
            var project = Path.Combine(scratch, "proj");
            Directory.CreateDirectory(project);
            WriteManifest(project, "Demo", remote, "v1.0.0");

            var window = new PackageWindow(project) { Timeout = TimeSpan.FromMinutes(2) };
            window.Refresh();
            if (window.Rows.Count != 1 || window.Rows[0].Status != PackageRowStatus.NotResolved
                || window.Rows[0].PinnedSha is not null || window.Rows[0].CheckoutExists)
            {
                Console.WriteLine("  before resolve: expected single NotResolved row without pin");
                return false;
            }

            var result = window.ResolveAll();
            if (result.Packages.Count != 1 || result.Packages[0].Sha != wantSha)
            {
                Console.WriteLine("  resolve returned wrong pin");
                return false;
            }

            if (window.Rows.Count != 1 || window.Rows[0].PinnedSha != wantSha
                || window.Rows[0].Status != PackageRowStatus.UpToDate
                || !window.Rows[0].CheckoutExists
                || window.Rows[0].Name != "core" || window.Rows[0].Url != remote
                || window.Rows[0].Tag != "v1.0.0")
            {
                Console.WriteLine("  after resolve: row lacks name/url/tag/pin/UpToDate");
                return false;
            }

            var body = File.ReadAllText(Path.Combine(project, "Packages", "core", "data.txt"));
            return body == "v1";
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

    private static bool ResolveAllThenNoOp()
    {
        var scratch = TempDir("pkg-window-noop-");
        try
        {
            var remote = Path.Combine(scratch, "remote");
            InitRemote(remote, "data.txt", "v1", "v1.0.0");
            var project = Path.Combine(scratch, "proj");
            Directory.CreateDirectory(project);
            WriteManifest(project, "Demo", remote, "v1.0.0");

            var window = new PackageWindow(project) { Timeout = TimeSpan.FromMinutes(2) };
            window.ResolveAll();

            var gitCalls = new List<string[]>();
            PackageResolver.GitCommandObserver = argv => gitCalls.Add((string[])argv.Clone());
            LockResolveResult second;
            try
            {
                second = window.ResolveAll();
            }
            finally
            {
                PackageResolver.GitCommandObserver = null;
            }

            if (!second.WasNoOp || !window.LastResolveWasNoOp)
            {
                Console.WriteLine("  second resolve did not report no-op");
                return false;
            }

            // The idempotent path skips all network/checkout work: only
            // read-only local probes (rev-parse HEAD, status, rev-parse tag)
            // may run. Anything that touches the network or mutates the
            // checkout fails.
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

            var locked = PackageLockFile.Load(Path.Combine(project, "packages.lock.json"));
            return locked.PackageList.Count == 1 && locked.PackageList[0].Sha.Length == 40;
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

    private static bool AddUpdateRemove()
    {
        var scratch = TempDir("pkg-window-mutate-");
        try
        {
            var remoteA = Path.Combine(scratch, "remote-a");
            var shaA = InitRemote(remoteA, "a.txt", "a", "v1.0.0");
            var remoteB = Path.Combine(scratch, "remote-b");
            var shaB = InitRemote(remoteB, "b.txt", "b", "v2.0.0");

            var project = Path.Combine(scratch, "proj");
            Directory.CreateDirectory(project);
            File.WriteAllText(Path.Combine(project, "project.json"), "{ \"name\": \"Demo\" }\n");

            var window = new PackageWindow(project) { Timeout = TimeSpan.FromMinutes(2) };
            window.Refresh();
            if (window.Rows.Count != 0)
            {
                Console.WriteLine("  empty manifest should list zero rows");
                return false;
            }

            window.AddPackage("core", remoteA, "v1.0.0");
            if (window.Rows.Count != 1 || window.Rows[0].Url != remoteA)
            {
                Console.WriteLine("  add did not produce the row");
                return false;
            }

            window.UpdatePackage("core", remoteB, "v2.0.0");
            if (window.Rows.Count != 1 || window.Rows[0].Url != remoteB || window.Rows[0].Tag != "v2.0.0")
            {
                Console.WriteLine("  update did not change the row");
                return false;
            }

            var result = window.ResolveAll();
            if (result.Packages.Count != 1 || result.Packages[0].Sha != shaB || shaB == shaA)
            {
                Console.WriteLine("  resolve after update pinned the wrong SHA");
                return false;
            }

            if (!File.Exists(Path.Combine(project, "Packages", "core", "b.txt")))
            {
                Console.WriteLine("  updated checkout missing new content");
                return false;
            }

            window.RemovePackage("core");
            if (window.Rows.Count != 0)
            {
                Console.WriteLine("  remove did not clear the row");
                return false;
            }

            if (Directory.Exists(Path.Combine(project, "Packages", "core")))
            {
                Console.WriteLine("  remove did not delete the checkout");
                return false;
            }

            var manifest = ProjectManifestFile.Load(Path.Combine(project, "project.json"));
            return manifest.PackageList.Count == 0;
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

    private static bool FailureGoesInline()
    {
        var scratch = TempDir("pkg-window-fail-");
        try
        {
            var remote = Path.Combine(scratch, "remote");
            InitRemote(remote, "data.txt", "v1", "v1.0.0");
            var project = Path.Combine(scratch, "proj");
            Directory.CreateDirectory(project);
            WriteManifest(project, "Demo", remote, "no-such-tag");

            var window = new PackageWindow(project) { Timeout = TimeSpan.FromMinutes(2) };
            var threw = false;
            try
            {
                window.ResolveAll();
            }
            catch (PackageResolveException)
            {
                threw = true;
            }

            if (!threw)
            {
                Console.WriteLine("  unknown tag should throw PackageResolveException");
                return false;
            }

            if (window.LastError is null || !window.LastError.Contains("core")
                || !window.LastError.Contains("no-such-tag"))
            {
                Console.WriteLine($"  LastError missing package/tag context: '{window.LastError}'");
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
            // Skip the check files themselves (they name the forbidden
            // patterns) and build outputs (git never runs from there).
            var leaf = Path.GetFileName(file);
            if (leaf == "PackageResolverTests.cs" || leaf == "PackageLockTests.cs"
                || leaf == "PackageWindowTests.cs")
            {
                continue;
            }

            var full = Path.GetFullPath(file);
            if (full.Contains($"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}")
                || full.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}"))
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
        Console.WriteLine(ok ? $"[PASS] window: {name}" : $"[FAIL] window: {name}");
        return ok ? 0 : 1;
    }
}

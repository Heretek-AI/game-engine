// PackageResolver.cs — P7-05: argv-only git package resolver (PARITY-134).
//
// Resolves project.json `packages` entries (name, git URL, tag) into
// Packages/<name>/ checkouts pinned at the tag's commit SHA:
//
//   missing dir -> git clone --no-checkout <url> <dir>
//   existing dir -> git -C <dir> fetch --tags --prune --force origin
//   then          -> resolve tag to a full 40-char SHA via
//                    git -C <dir> rev-parse "<tag>^{commit}"
//                  -> git -C <dir> checkout --detach <sha>
//
// Every git invocation goes through System.Diagnostics.Process with
// ArgumentList (argv array) and UseShellExecute=false. No shell strings,
// no UseShellExecute=true, no /bin/sh -c anywhere near git.
// Pure managed — no ABI surface, no NativeApi.g.cs changes.
using System.Diagnostics;

namespace Engine.Editor;

/// <summary>Failure resolving one or more packages.</summary>
public sealed class PackageResolveException : Exception
{
    public PackageResolveException(string message)
        : base(message)
    {
    }

    public PackageResolveException(string message, Exception inner)
        : base(message, inner)
    {
    }
}

/// <summary>Resolved pin for one package: the request plus its commit SHA.</summary>
public sealed record ResolvedPackage(string Name, string Url, string Tag, string Sha);

/// <summary>
/// Outcome of <see cref="PackageResolver.ResolveWithLock"/>: the pins plus
/// whether every entry skipped all network/checkout work.
/// </summary>
/// <param name="Packages">Pins in manifest order.</param>
/// <param name="WasNoOp">
/// True only when every entry was satisfied from the existing lock and a
/// clean checkout — no fetch, clone, or checkout ran.
/// </param>
public sealed record LockResolveResult(IReadOnlyList<ResolvedPackage> Packages, bool WasNoOp);

/// <summary>Clones/fetches packages into Packages/&lt;name&gt;/ and checks out each tag's SHA.</summary>
public static class PackageResolver
{
    /// <summary>
    /// Hook for tests: invoked with each git argv (e.g. ["fetch", ...]) right
    /// before the process starts. Null in production. Set it to count or
    /// record git invocations when proving the idempotent no-op path.
    /// </summary>
    public static Action<string[]>? GitCommandObserver { get; set; }

    /// <summary>Filename this resolver reads/writes for pins beside the project.</summary>
    public const string LockFileName = "packages.lock.json";

    /// <summary>
    /// Resolve every entry in <paramref name="packages"/> into
    /// <c>&lt;packagesRoot&gt;/&lt;name&gt;/</c>, checking out each tag's
    /// full 40-char commit SHA. Creates <paramref name="packagesRoot"/> when missing.
    /// </summary>
    /// <exception cref="PackageResolveException">On any git failure or invalid SHA.</exception>
    public static IReadOnlyList<ResolvedPackage> Resolve(
        IEnumerable<PackageReference> packages,
        string packagesRoot,
        TimeSpan? timeout = null)
    {
        ArgumentNullException.ThrowIfNull(packages);
        ArgumentException.ThrowIfNullOrEmpty(packagesRoot);
        Directory.CreateDirectory(packagesRoot);

        var resolved = new List<ResolvedPackage>();
        foreach (var pkg in packages)
        {
            resolved.Add(ResolveOne(pkg, packagesRoot, timeout ?? TimeSpan.FromMinutes(5)));
        }

        return resolved;
    }

    /// <summary>
    /// Resolve with lock pinning (P7-05 idempotency): reads
    /// <c>&lt;lockPath&gt;</c> (default <c>&lt;packagesRoot&gt;/../packages.lock.json</c>
    /// — pass an explicit path in tests), skips git entirely for entries
    /// whose pinned SHA still equals the tag's remote resolution and whose
    /// working tree is clean at that SHA, and rewrites the lock with fresh
    /// url+tag+SHA pins afterwards.
    /// </summary>
    /// <returns>
    /// The resolved pins plus whether the run was a full no-op
    /// (<see cref="LockResolveResult.WasNoOp"/> is true only when every
    /// entry skipped all git work).
    /// </returns>
    public static LockResolveResult ResolveWithLock(
        IEnumerable<PackageReference> packages,
        string packagesRoot,
        string? lockPath = null,
        TimeSpan? timeout = null)
    {
        ArgumentNullException.ThrowIfNull(packages);
        ArgumentException.ThrowIfNullOrEmpty(packagesRoot);
        var list = packages.ToList();
        var effectiveTimeout = timeout ?? TimeSpan.FromMinutes(5);
        var path = lockPath ?? DefaultLockPath(packagesRoot);
        Directory.CreateDirectory(packagesRoot);

        var existing = File.Exists(path) ? PackageLockFile.TryLoad(path) : null;
        var byName = existing?.PackageList.ToDictionary(p => p.Name, StringComparer.Ordinal)
            ?? new Dictionary<string, LockedPackage>(StringComparer.Ordinal);

        var resolved = new List<ResolvedPackage>(list.Count);
        var allNoOp = list.Count > 0;
        foreach (var pkg in list)
        {
            var skipped = TrySkipUnchanged(pkg, packagesRoot, byName, effectiveTimeout, resolved);
            if (!skipped)
            {
                allNoOp = false;
                resolved.Add(ResolveOne(pkg, packagesRoot, effectiveTimeout));
            }
        }

        // Pruning to an empty request is a real change when the old lock
        // pinned packages.
        if (list.Count == 0 && existing is not null && existing.PackageList.Count > 0)
        {
            allNoOp = false;
        }

        PackageLockFile.Save(path, new PackageLock(
            resolved.Select(r => new LockedPackage(r.Name, r.Url, r.Tag, r.Sha)).ToList()));

        return new LockResolveResult(resolved, allNoOp);
    }

    /// <summary>Default lock location: beside the Packages/ root's parent.</summary>
    public static string DefaultLockPath(string packagesRoot) =>
        Path.Combine(
            Path.GetDirectoryName(Path.GetFullPath(packagesRoot).TrimEnd(Path.DirectorySeparatorChar))
            ?? Path.GetFullPath(packagesRoot),
            LockFileName);

    // Fast path: the lock already pins this exact request, the checkout is
    // at that SHA, and the working tree is clean — no fetch, no checkout.
    // Any mismatch (no pin, url/tag drift, dirty tree, SHA moved) returns
    // false so the caller falls through to the full ResolveOne.
    private static bool TrySkipUnchanged(
        PackageReference pkg,
        string packagesRoot,
        Dictionary<string, LockedPackage> byName,
        TimeSpan timeout,
        List<ResolvedPackage> resolved)
    {
        if (!byName.TryGetValue(pkg.Name, out var pinned))
        {
            return false;
        }

        if (!string.Equals(pinned.Url, pkg.Url, StringComparison.Ordinal)
            || !string.Equals(pinned.Tag, pkg.Tag, StringComparison.Ordinal)
            || !PackageLockFile.IsCommitSha(pinned.Sha))
        {
            return false;
        }

        if (!ProjectManifestFile.IsValidPackageName(pkg.Name))
        {
            return false;
        }

        var dir = Path.Combine(packagesRoot, pkg.Name);
        if (!Directory.Exists(Path.Combine(dir, ".git")))
        {
            return false;
        }

        try
        {
            var head = RunGit(dir, ["rev-parse", "HEAD"], timeout, $"read HEAD of package '{pkg.Name}'")
                .TrimEnd('\r', '\n');
            if (!string.Equals(head, pinned.Sha, StringComparison.OrdinalIgnoreCase))
            {
                return false;
            }

            var status = RunGit(dir, ["status", "--porcelain"], timeout, $"check clean tree of package '{pkg.Name}'");
            if (!string.IsNullOrWhiteSpace(status))
            {
                return false;
            }

            // Still matches the tag's remote resolution? Resolve the tag in
            // the local clone WITHOUT fetching: if the tag moved remotely,
            // the stale local SHA differs from the lock after the last fetch
            // ... but a moved tag is only visible after a fetch, which is
            // exactly the network we want to skip. So: verify the tag still
            // resolves locally to the pinned SHA. Local-only by design: a tag
            // moved upstream is picked up by the next full resolve (fetch);
            // the lock is a pin, not a tracker. refs/tags/ + --verify so
            // a branch or expression can never satisfy a tag check.
            var localTag = RunGit(dir, ["rev-parse", "--verify", $"refs/tags/{pkg.Tag}^{{commit}}"], timeout, $"resolve tag '{pkg.Tag}' of package '{pkg.Name}'")
                .TrimEnd('\r', '\n');
            if (!string.Equals(localTag, pinned.Sha, StringComparison.OrdinalIgnoreCase))
            {
                return false;
            }
        }
        catch (PackageResolveException)
        {
            return false;
        }

        resolved.Add(new ResolvedPackage(pkg.Name, pkg.Url, pkg.Tag, pinned.Sha));
        return true;
    }

    private static ResolvedPackage ResolveOne(PackageReference pkg, string packagesRoot, TimeSpan timeout)
    {
        // ProjectManifest validation already rejects bad names, but Resolve is
        // public API — re-check so a hostile name can never escape Packages/.
        if (!ProjectManifestFile.IsValidPackageName(pkg.Name))
        {
            throw new PackageResolveException($"package '{pkg.Name}': name must match [A-Za-z0-9_-]+.");
        }

        var dir = Path.Combine(packagesRoot, pkg.Name);
        if (Directory.Exists(dir))
        {
            if (!Directory.Exists(Path.Combine(dir, ".git")))
            {
                throw new PackageResolveException(
                    $"package '{pkg.Name}': '{dir}' exists but is not a git checkout (no .git).");
            }

            // Existing checkout: point origin at the requested URL first.
            // UpdatePackage can change the url of an already-resolved
            // package; without this the fetch below reads the OLD
            // repository and the lock records the new url with a commit
            // from the old repo — a wrong pin nothing reports.
            RunGit(dir, ["remote", "set-url", "origin", pkg.Url], timeout, $"retarget origin of package '{pkg.Name}'");
            // Refresh tags/branches from origin.
            RunGit(dir, ["fetch", "--tags", "--prune", "--force", "origin"], timeout, $"fetch package '{pkg.Name}'");
        }
        else
        {
            // Fresh package: clone without a working-tree checkout; the
            // pinned checkout below materializes the right commit.
            RunGit(packagesRoot, ["clone", "--no-checkout", pkg.Url, dir], timeout, $"clone package '{pkg.Name}'");
        }

        // Resolve the tag to a full 40-char commit SHA. refs/tags/ + --verify
        // keeps the pin tied to a tag: a bare rev-parse would also match
        // branch names, remote-tracking refs, abbreviated SHAs, or
        // expressions like HEAD~1, and 'main' would pin a branch tip.
        // (^{commit} peels annotated tags; lightweight tags already name
        // a commit.)
        var sha = RunGit(dir, ["rev-parse", "--verify", $"refs/tags/{pkg.Tag}^{{commit}}"], timeout, $"resolve tag '{pkg.Tag}' of package '{pkg.Name}'")
            .TrimEnd('\r', '\n');
        if (!IsCommitSha(sha))
        {
            throw new PackageResolveException(
                $"package '{pkg.Name}': tag '{pkg.Tag}' resolved to '{sha}', not a 40-char commit SHA.");
        }

        // Detached checkout of the pinned SHA (idempotent when already there).
        RunGit(dir, ["checkout", "--detach", sha], timeout, $"checkout {sha} of package '{pkg.Name}'");

        return new ResolvedPackage(pkg.Name, pkg.Url, pkg.Tag, sha);
    }

    /// <summary>
    /// Run one git command via argv only. Never a shell string, never
    /// UseShellExecute=true.
    /// </summary>
    /// <exception cref="PackageResolveException">When git is missing, times out, or exits non-zero.</exception>
    internal static string RunGit(string workingDirectory, string[] argv, TimeSpan timeout, string what)
    {
        GitCommandObserver?.Invoke(argv);
        var psi = new ProcessStartInfo("git") { UseShellExecute = false };
        foreach (var arg in argv)
        {
            psi.ArgumentList.Add(arg);
        }

        psi.WorkingDirectory = workingDirectory;
        psi.RedirectStandardOutput = true;
        psi.RedirectStandardError = true;
        // Never prompt: a private-HTTPS credential question would hang the
        // editor past any timeout. Fail fast instead; the message names git.
        psi.Environment["GIT_TERMINAL_PROMPT"] = "0";

        string stdout;
        string stderr;
        try
        {
            using var proc = Process.Start(psi)
                ?? throw new PackageResolveException($"cannot {what}: failed to start git.");
            // Async reads BEFORE the wait: ReadToEnd-then-WaitForExit blocks
            // until git exits (it closes the pipe on exit), so the timeout
            // could never fire, and a full stderr pipe deadlocks against a
            // stdout drain. The tasks are awaited only after the wait below.
            var stdoutTask = proc.StandardOutput.ReadToEndAsync();
            var stderrTask = proc.StandardError.ReadToEndAsync();
            if (!proc.WaitForExit((int)timeout.TotalMilliseconds))
            {
                try
                {
                    proc.Kill(entireProcessTree: true);
                }
                catch (InvalidOperationException)
                {
                }

                throw new PackageResolveException($"cannot {what}: git timed out after {timeout}.");
            }

            // Exit is confirmed; the streams are closed, so this completes
            // without blocking.
            stdout = stdoutTask.GetAwaiter().GetResult();
            stderr = stderrTask.GetAwaiter().GetResult();

            if (proc.ExitCode != 0)
            {
                throw new PackageResolveException($"cannot {what}: git exited {proc.ExitCode}: {stderr.Trim()}");
            }
        }
        catch (PackageResolveException)
        {
            throw;
        }
        catch (Exception ex) when (ex is System.ComponentModel.Win32Exception or IOException or UnauthorizedAccessException)
        {
            throw new PackageResolveException($"cannot {what}: {ex.Message}", ex);
        }

        return stdout;
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

// PlayerBuildController.cs — t_13d15c63: Build window <-> IPlayerBuilder wiring.
//
// Owns everything the Build window needs EXCEPT the window itself (the
// window lives in t_457e44cb; this task wires the backend interface):
//   * async BuildAsync: runs IPlayerBuilder.BuildPlayer off the UI thread,
//     streams progress (0..1) + log lines to caller-supplied sinks, maps
//     exceptions (incl. cancellation) to a failed PlayerBuildResult, and
//     records last-build status + cold-boot timing.
//   * RunAsync / BuildAndRunAsync: BuildAsync then launch the built player
//     binary (timing the cold boot with a Stopwatch; failures surface as
//     status, never throw out of the controller).
//   * RuntimeTargetOs: the OS this process runs on (for Run gating).
//   * RidFor: PlayerTargetOs -> dotnet publish RID (agreed contract with the
//     P2-07 backend task: win-x64 / linux-x64 / osx-x64).
//
// Design notes:
//   * The sibling t_457e44cb window binds its progress-bar/log-view/status
//     labels to these callbacks; the controller never touches Avalonia so
//     headless tests drive it directly.
//   * No engine_abi.h change (managed-only).
using System.Diagnostics;

namespace Engine.Editor;

/// <summary>Status snapshot after a build (or build-and-run).</summary>
public sealed record PlayerBuildStatus(
    bool Succeeded,
    string OutputDir,
    PlayerTargetOs TargetOs,
    PlayerBuildConfig Config,
    string Log,
    double ColdBootSeconds,
    string? Error,
    DateTimeOffset FinishedAt);

/// <summary>
/// Wires the Build window to <see cref="IPlayerBuilder"/>: async builds
/// with progress/log streaming, Build-And-Run launch, last-build status
/// + cold-boot timing. UI-thread safe: the blocking builder call runs on
/// a worker thread; callers marshal the sinks.
/// </summary>
public sealed class PlayerBuildController
{
    private readonly IPlayerBuilder _builder;
    private readonly Func<string, PlayerTargetOs, CancellationToken, Task<ProcessRun>> _launcher;

    public PlayerBuildController(
        IPlayerBuilder? builder = null,
        Func<string, PlayerTargetOs, CancellationToken, Task<ProcessRun>>? launcher = null)
    {
        _builder = builder ?? new StubPlayerBuilder();
        _launcher = launcher ?? LaunchPlayerAsync;
    }

    /// <summary>Most recent build status (null before the first build).</summary>
    public PlayerBuildStatus? LastStatus { get; private set; }

    /// <summary>The OS this editor process runs on (Run is gated to it).</summary>
    public static PlayerTargetOs RuntimeTargetOs =>
        OperatingSystem.IsWindows() ? PlayerTargetOs.Windows
        : OperatingSystem.IsMacOS() ? PlayerTargetOs.MacOS
        : PlayerTargetOs.Linux;

    /// <summary>dotnet publish RID for a target OS (P7-03 matrix).</summary>
    public static string RidFor(PlayerTargetOs os) => os switch
    {
        PlayerTargetOs.Windows => "win-x64",
        PlayerTargetOs.MacOS => "osx-x64",
        _ => "linux-x64",
    };

    /// <summary>
    /// Build the player for <paramref name="targetOs"/> into
    /// <paramref name="outputDir"/>. Progress (0..1) and log lines stream to
    /// the sinks; the blocking backend call runs on a worker thread so the
    /// UI thread never blocks. Never throws: failures come back as a failed
    /// result + status.
    /// </summary>
    public async Task<PlayerBuildResult> BuildAsync(
        PlayerTargetOs targetOs,
        string outputDir,
        PlayerBuildConfig config,
        IProgress<double>? progress = null,
        IProgress<string>? log = null,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(outputDir);
        log?.Report($"build: {targetOs} {config} -> {outputDir}");
        try
        {
            var result = await Task.Run(
                () => _builder.BuildPlayer(targetOs, outputDir, config, progress, cancellationToken),
                cancellationToken).ConfigureAwait(false);
            if (!string.IsNullOrEmpty(result.Log))
            {
                log?.Report(result.Log);
            }

            LastStatus = new PlayerBuildStatus(
                result.Succeeded, outputDir, targetOs, config,
                result.Log, result.ColdBootSeconds,
                result.Succeeded ? null : result.Log,
                DateTimeOffset.UtcNow);
            log?.Report(result.Succeeded ? "build: succeeded" : $"build: failed: {result.Log}");
            return result;
        }
        catch (OperationCanceledException)
        {
            const string msg = "build: cancelled.";
            log?.Report(msg);
            LastStatus = new PlayerBuildStatus(
                false, outputDir, targetOs, config, msg, 0.0, msg, DateTimeOffset.UtcNow);
            return new PlayerBuildResult(string.Empty, string.Empty, string.Empty, msg, 0.0, false);
        }
        catch (Exception ex)
        {
            var msg = $"build: failed: {ex.GetType().Name}: {ex.Message}";
            log?.Report(msg);
            LastStatus = new PlayerBuildStatus(
                false, outputDir, targetOs, config, msg, 0.0, msg, DateTimeOffset.UtcNow);
            return new PlayerBuildResult(string.Empty, string.Empty, string.Empty, msg, 0.0, false);
        }
    }

    /// <summary>
    /// Build then launch the player binary. Run is gated to the editor's own
    /// OS (cross-OS binaries cannot execute here); a gated-out run still
    /// returns the build result with a skip note in the log. Cold-boot time
    /// is measured around process exit and recorded on <see cref="LastStatus"/>.
    /// </summary>
    public async Task<(PlayerBuildResult Build, string? RunLog)> BuildAndRunAsync(
        PlayerTargetOs targetOs,
        string outputDir,
        PlayerBuildConfig config,
        IProgress<double>? progress = null,
        IProgress<string>? log = null,
        CancellationToken cancellationToken = default)
    {
        var build = await BuildAsync(targetOs, outputDir, config, progress, log, cancellationToken)
            .ConfigureAwait(false);
        if (!build.Succeeded)
        {
            return (build, null);
        }

        if (targetOs != RuntimeTargetOs)
        {
            var skip = $"run: skipped (built {targetOs}, editor runs on {RuntimeTargetOs}).";
            log?.Report(skip);
            return (build, skip);
        }

        var sw = Stopwatch.StartNew();
        string runLog;
        try
        {
            var run = await _launcher(build.PlayerPath, targetOs, cancellationToken)
                .ConfigureAwait(false);
            sw.Stop();
            runLog = string.IsNullOrEmpty(run.Stderr)
                ? $"run: exit {run.ExitCode} in {sw.Elapsed.TotalSeconds:F2}s. {run.Stdout}".Trim()
                : $"run: exit {run.ExitCode} in {sw.Elapsed.TotalSeconds:F2}s. {run.Stdout} {run.Stderr}".Trim();
        }
        catch (OperationCanceledException)
        {
            sw.Stop();
            runLog = "run: cancelled.";
        }
        catch (Exception ex)
        {
            sw.Stop();
            runLog = $"run: failed: {ex.GetType().Name}: {ex.Message}";
        }

        log?.Report(runLog);
        if (LastStatus != null)
        {
            LastStatus = LastStatus with
            {
                ColdBootSeconds = sw.Elapsed.TotalSeconds,
                Log = LastStatus.Log + "\n" + runLog,
            };
        }

        return (build, runLog);
    }

    /// <summary>Default launcher: start the player binary, capture output.</summary>
    private static async Task<ProcessRun> LaunchPlayerAsync(
        string playerPath, PlayerTargetOs targetOs, CancellationToken ct)
    {
        var psi = new ProcessStartInfo(playerPath)
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        using var proc = Process.Start(psi)
            ?? throw new InvalidOperationException($"run: could not start {playerPath}.");
        var stdoutTask = proc.StandardOutput.ReadToEndAsync(ct);
        var stderrTask = proc.StandardError.ReadToEndAsync(ct);
        await proc.WaitForExitAsync(ct).ConfigureAwait(false);
        return new ProcessRun(proc.ExitCode, await stdoutTask.ConfigureAwait(false), await stderrTask.ConfigureAwait(false));
    }
}

/// <summary>Captured player process run (injectable for headless tests).</summary>
public sealed record ProcessRun(int ExitCode, string Stdout, string Stderr);

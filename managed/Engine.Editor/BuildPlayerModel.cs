// BuildPlayerModel.cs — P7-03: headless-testable view-model for the editor Build window.
//
// Holds target OS / config / output dir selection, runs IPlayerBuilder on a
// background thread, and exposes progress/log/last-build status for the
// Avalonia BuildPlayerWindow to bind to. No Avalonia dependency: covered by
// Engine.Tests (PlayerBuildModelTests) so the window logic is verified
// without a UI thread.
//
// States: Idle -> Building -> Succeeded | Failed. Build and Build-And-Run
// are disabled while Building. Build-And-Run launches the built player via
// an injectable launcher (default: Process.Start) so tests can observe the
// launch without spawning a process.
using System.Diagnostics;

namespace Engine.Editor;

/// <summary>Build window state machine state.</summary>
public enum BuildPlayerState
{
    Idle,
    Building,
    Succeeded,
    Failed,
}

/// <summary>
/// Headless view-model for the Build Player window. Wraps an
/// <see cref="IPlayerBuilder"/> with selection + progress + status.
/// Thread-safety: all members are called on the editor/UI thread except
/// the progress callback, which the builder may invoke from any thread —
/// this model only stores the latest value (a double write is atomic).
/// </summary>
public sealed class BuildPlayerModel : IDisposable
{
    private readonly IPlayerBuilder _builder;
    private readonly Func<string, bool> _launcher;
    private readonly CancellationTokenSource _cts = new();
    private bool _disposed;

    private int _building; // 0 = idle, 1 = build in flight (re-entrancy guard)

    /// <summary>Selected target OS. Default: host OS.</summary>
    public PlayerTargetOs TargetOs { get; set; } = DefaultOs();

    /// <summary>Selected build configuration.</summary>
    public PlayerBuildConfig Config { get; set; } = PlayerBuildConfig.Release;

    /// <summary>Output folder chosen via the folder picker.</summary>
    public string OutputDir { get; set; } = string.Empty;

    /// <summary>0..1 build progress (latest report).</summary>
    public double Progress { get; private set; }

    /// <summary>Accumulated build log lines.</summary>
    public List<string> LogLines { get; } = [];

    /// <summary>Current state-machine state.</summary>
    public BuildPlayerState State { get; private set; } = BuildPlayerState.Idle;

    /// <summary>Last build result (null until the first build finishes).</summary>
    public PlayerBuildResult? LastResult { get; private set; }

    /// <summary>Last error message (empty unless <see cref="State"/> is Failed).</summary>
    public string LastError { get; private set; } = string.Empty;

    /// <summary>True when a launch was requested via Build-And-Run.</summary>
    public bool LaunchRequested { get; private set; }

    /// <summary>Player path the launcher was invoked with (test hook).</summary>
    public string? LaunchedPath { get; private set; }

    /// <summary>Raised on the calling thread after each state/progress/log change.</summary>
    public event Action? Changed;

    public BuildPlayerModel(IPlayerBuilder? builder = null, Func<string, bool>? launcher = null)
    {
        _builder = builder ?? new StubPlayerBuilder();
        _launcher = launcher ?? DefaultLaunch;
    }

    /// <summary>Build + Build-And-Run buttons are enabled only when idle with an output dir.</summary>
    public bool CanBuild => State != BuildPlayerState.Building
        && !string.IsNullOrWhiteSpace(OutputDir);

    /// <summary>Validation message for the current selection (empty when valid).</summary>
    public string ValidationError
    {
        get
        {
            if (string.IsNullOrWhiteSpace(OutputDir))
            {
                return "Choose an output folder.";
            }

            return string.Empty;
        }
    }

    /// <summary>Human-readable last-build status line for the status bar.</summary>
    public string StatusLine => State switch
    {
        BuildPlayerState.Idle => LastResult is null
            ? "No build yet."
            : Describe(LastResult),
        BuildPlayerState.Building => $"Building {TargetOs} {Config}… {Progress:P0}",
        BuildPlayerState.Succeeded => LastResult is null ? "Build succeeded." : Describe(LastResult),
        BuildPlayerState.Failed => string.IsNullOrEmpty(LastError) ? "Build failed." : $"Build failed: {LastError}",
        _ => string.Empty,
    };

    /// <summary>Run a build. Returns the result; updates State/Progress/LogLines/LastResult.</summary>
    public async Task<PlayerBuildResult?> BuildAsync(bool andRun = false)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (Interlocked.CompareExchange(ref _building, 1, 0) != 0)
        {
            return null; // re-entrant call while a build is in flight
        }

        try
        {
            var validation = ValidationError;
            if (!string.IsNullOrEmpty(validation))
            {
                Fail(validation);
                return null;
            }

            BeginBuild();
            var os = TargetOs;
            var dir = OutputDir;
            var cfg = Config;
            var progress = new Progress<double>(v =>
            {
                Progress = Math.Clamp(v, 0.0, 1.0);
                Changed?.Invoke();
            });
            PlayerBuildResult result;
            try
            {
                result = await Task.Run(
                    () => _builder.BuildPlayer(os, dir, cfg, progress, _cts.Token),
                    _cts.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                Fail("Build cancelled.");
                return null;
            }
            catch (Exception ex)
            {
                Fail(ex.Message);
                return null;
            }

            FinishBuild(result);
            if (andRun && result.Succeeded)
            {
                LaunchRequested = true;
                LaunchedPath = result.PlayerPath;
                try
                {
                    _launcher(result.PlayerPath);
                }
                catch (Exception ex)
                {
                    Fail($"Build succeeded but launch failed: {ex.Message}");
                    return result;
                }

                Changed?.Invoke();
            }

            return result;
        }
        finally
        {
            Volatile.Write(ref _building, 0);
        }
    }

    /// <summary>Cancel the in-flight build (cooperative; the stub checks the token).</summary>
    public void Cancel() => _cts.Cancel();

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _cts.Cancel();
        _cts.Dispose();
    }

    private void BeginBuild()
    {
        State = BuildPlayerState.Building;
        LastError = string.Empty;
        LaunchRequested = false;
        LaunchedPath = null;
        Progress = 0.0;
        LogLines.Clear();
        LogLines.Add($"Build started: {TargetOs} {Config} -> {OutputDir}");
        Changed?.Invoke();
    }

    private void FinishBuild(PlayerBuildResult result)
    {
        LastResult = result;
        Progress = 1.0;
        if (!string.IsNullOrEmpty(result.Log))
        {
            LogLines.Add(result.Log);
        }

        if (result.Succeeded)
        {
            State = BuildPlayerState.Succeeded;
            LogLines.Add($"Build succeeded in {result.ColdBootSeconds:F2}s (cold boot).");
        }
        else
        {
            State = BuildPlayerState.Failed;
            LastError = string.IsNullOrEmpty(result.Log) ? "Builder reported failure." : result.Log;
            LogLines.Add($"Build failed: {LastError}");
        }

        Changed?.Invoke();
    }

    private void Fail(string message)
    {
        State = BuildPlayerState.Failed;
        LastError = message;
        LogLines.Add($"Build failed: {message}");
        Changed?.Invoke();
    }

    private static string Describe(PlayerBuildResult r) =>
        $"Last build: {(r.Succeeded ? "succeeded" : "failed")} ({r.PlayerPath}) · cold boot {r.ColdBootSeconds:F2}s.";

    private static PlayerTargetOs DefaultOs() =>
        OperatingSystem.IsWindows() ? PlayerTargetOs.Windows
        : OperatingSystem.IsMacOS() ? PlayerTargetOs.MacOS
        : PlayerTargetOs.Linux;

    private static bool DefaultLaunch(string playerPath)
    {
        Process.Start(new ProcessStartInfo(playerPath) { UseShellExecute = true });
        return true;
    }
}

// PlayerBuildControllerTests.cs — t_13d15c63: Build window wiring acceptance.
//
// CPU-only (no native, no Avalonia): drives PlayerBuildController directly
// against the StubPlayerBuilder + injected fakes:
//   * every PlayerTargetOs builds to the chosen folder (outputs land,
//     progress reaches 1.0, log lines stream, status recorded);
//   * RidFor maps each OS to the P7-03 publish RID;
//   * failing/cancelling builders surface as failed results + error status
//     (no throw out of the controller — the UI thread never blocks);
//   * BuildAndRun launches on the runtime OS (injected launcher, cold-boot
//     timing recorded) and skips cross-OS runs.
using Engine.Editor;

namespace Engine.Tests;

public static class PlayerBuildControllerTests
{
    public static int Run()
    {
        var failures = 0;

        // --- contract: every OS builds into the chosen folder ------------------
        foreach (var os in new[] { PlayerTargetOs.Windows, PlayerTargetOs.Linux, PlayerTargetOs.MacOS })
        {
            var dir = Path.Combine(Path.GetTempPath(), $"playerbuild-{Environment.ProcessId}-{os}");
            try
            {
                var ctl = new PlayerBuildController(new StubPlayerBuilder());
                var seen = new List<double>();
                var lines = new List<string>();
                var result = ctl.BuildAsync(
                    os, dir, PlayerBuildConfig.Release,
                    new SyncProgress<double>(seen.Add),
                    new SyncProgress<string>(lines.Add)).GetAwaiter().GetResult();

                failures += Check(result.Succeeded, $"build-window: {os} succeeds");
                failures += Check(File.Exists(result.PlayerPath), $"build-window: {os} player lands in folder");
                failures += Check(File.Exists(result.PakPath), $"build-window: {os} pak lands in folder");
                failures += Check(File.Exists(result.AssemblyPath), $"build-window: {os} assembly lands in folder");
                failures += Check(seen.Count > 0 && seen.Max() >= 1.0, $"build-window: {os} progress reaches 1.0");
                failures += Check(lines.Count > 0, $"build-window: {os} log lines stream");
                failures += Check(
                    ctl.LastStatus != null && ctl.LastStatus.Succeeded && ctl.LastStatus.OutputDir == dir,
                    $"build-window: {os} last-build status recorded");
            }
            finally
            {
                try { Directory.Delete(dir, recursive: true); } catch (Exception) { }
            }
        }

        // --- RID matrix (agreed contract with the P2-07 backend task) -----------
        failures += Check(PlayerBuildController.RidFor(PlayerTargetOs.Windows) == "win-x64", "build-window: rid windows");
        failures += Check(PlayerBuildController.RidFor(PlayerTargetOs.Linux) == "linux-x64", "build-window: rid linux");
        failures += Check(PlayerBuildController.RidFor(PlayerTargetOs.MacOS) == "osx-x64", "build-window: rid macos");

        // --- failures surface as failed results, never throw -------------------
        // NOTE: synchronous log sink (no Progress<T> thread-hop): Progress<T>
        // posts through the captured SynchronizationContext, which is absent
        // in the headless test runner — on some runtimes the callback lands
        // after the assertion. The controller accepts any IProgress<string>.
        {
            var ctl = new PlayerBuildController(new FailingBuilder());
            var lines = new List<string>();
            var result = ctl.BuildAsync(
                PlayerTargetOs.Linux, Path.GetTempPath(), PlayerBuildConfig.Debug,
                log: new SyncProgress<string>(lines.Add)).GetAwaiter().GetResult();
            failures += Check(!result.Succeeded, "build-window: backend error -> failed result");
            failures += Check(ctl.LastStatus != null && !ctl.LastStatus.Succeeded && ctl.LastStatus.Error != null,
                "build-window: backend error -> error status");
            failures += Check(lines.Any(l => l.Contains("failed")), "build-window: backend error -> error log line");
        }

        // --- cancellation maps to a failed result ------------------------------
        {
            var ctl = new PlayerBuildController(new HangingBuilder());
            using var cts = new CancellationTokenSource();
            cts.Cancel();
            var result = ctl.BuildAsync(
                PlayerTargetOs.Linux, Path.GetTempPath(), PlayerBuildConfig.Debug,
                cancellationToken: cts.Token).GetAwaiter().GetResult();
            failures += Check(!result.Succeeded, "build-window: cancel -> failed result");
        }

        // --- Build-And-Run: runtime OS launches, cross-OS skips ----------------
        {
            var launched = new List<string>();
            Task<ProcessRun> Launcher(string path, PlayerTargetOs os, CancellationToken ct)
            {
                launched.Add(path);
                return Task.FromResult(new ProcessRun(0, "PLAYER-OK", string.Empty));
            }

            var dir = Path.Combine(Path.GetTempPath(), $"playerbuild-run-{Environment.ProcessId}");
            try
            {
                var ctl = new PlayerBuildController(new StubPlayerBuilder(), Launcher);
                var self = PlayerBuildController.RuntimeTargetOs;
                var (build, runLog) = ctl.BuildAndRunAsync(self, dir, PlayerBuildConfig.Release)
                    .GetAwaiter().GetResult();
                failures += Check(build.Succeeded && launched.Count == 1 && launched[0] == build.PlayerPath,
                    "build-window: build-and-run launches on runtime OS");
                failures += Check(runLog != null && runLog.Contains("exit 0"), "build-window: run log shows exit code");
                failures += Check(ctl.LastStatus != null && ctl.LastStatus.ColdBootSeconds >= 0,
                    "build-window: cold-boot time recorded");

                var other = self == PlayerTargetOs.Linux ? PlayerTargetOs.Windows : PlayerTargetOs.Linux;
                launched.Clear();
                var (_, skipLog) = ctl.BuildAndRunAsync(other, dir, PlayerBuildConfig.Release)
                    .GetAwaiter().GetResult();
                failures += Check(launched.Count == 0, "build-window: cross-OS run skipped");
                failures += Check(skipLog != null && skipLog.Contains("skipped"), "build-window: cross-OS skip logged");
            }
            finally
            {
                try { Directory.Delete(dir, recursive: true); } catch (Exception) { }
            }
        }

        return failures;
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }

    private sealed class FailingBuilder : IPlayerBuilder
    {
        public PlayerBuildResult BuildPlayer(
            PlayerTargetOs targetOs, string outputDir, PlayerBuildConfig config,
            IProgress<double>? progress = null, CancellationToken cancellationToken = default) =>
            throw new InvalidOperationException("backend exploded");
    }

    private sealed class HangingBuilder : IPlayerBuilder
    {
        public PlayerBuildResult BuildPlayer(
            PlayerTargetOs targetOs, string outputDir, PlayerBuildConfig config,
            IProgress<double>? progress = null, CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            throw new InvalidOperationException("should have cancelled first");
        }
    }

    /// <summary>Synchronous IProgress: invokes the handler inline (no
    /// SynchronizationContext hop), so assertions observe every report.</summary>
    private sealed class SyncProgress<T>(Action<T> handler) : IProgress<T>
    {
        public void Report(T value) => handler(value);
    }
}

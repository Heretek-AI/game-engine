// PlayerBuildModelTests.cs — P7-03: headless coverage for the Build window model.
//
// Exercises BuildPlayerModel without Avalonia: stub builds for each target
// OS, validation errors, error propagation, Build-And-Run launch hook, and
// the in-progress re-entrancy guard.
using Engine.Editor;

namespace Engine.Tests;

public static class PlayerBuildModelTests
{
    public static int Run()
    {
        var failures = 0;
        failures += CheckBuildsAllOses();
        failures += CheckValidationError();
        failures += CheckBuilderError();
        failures += CheckBuildAndRunLaunches();
        failures += CheckReentrancyGuard();
        return failures;
    }

    private static int CheckBuildsAllOses()
    {
        var failures = 0;
        foreach (var os in Enum.GetValues<PlayerTargetOs>())
        {
            var dir = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
            try
            {
                using var model = new BuildPlayerModel(new StubPlayerBuilder())
                {
                    TargetOs = os,
                    Config = PlayerBuildConfig.Release,
                    OutputDir = dir,
                };
                var progressSeen = new List<double>();
                model.Changed += () => progressSeen.Add(model.Progress);
                var task = model.BuildAsync();
                var ok = task.Wait(TimeSpan.FromSeconds(30));
                failures += Check(ok, $"model: build task completes ({os})");
                var result = ok ? task.Result : null;
                failures += Check(result?.Succeeded == true, $"model: stub build succeeds ({os})");
                failures += Check(model.State == BuildPlayerState.Succeeded, $"model: state Succeeded ({os})");
                failures += Check(File.Exists(result?.PlayerPath), $"model: player output lands in folder ({os})");
                failures += Check(File.Exists(result?.PakPath), $"model: pak output lands in folder ({os})");
                failures += Check(model.Progress >= 1.0, $"model: progress reaches 1 ({os})");
                failures += Check(model.LogLines.Count > 0, $"model: log has lines ({os})");
                failures += Check(model.StatusLine.Contains("succeeded", StringComparison.OrdinalIgnoreCase),
                    $"model: status shows success ({os})");
                failures += Check(model.LastResult?.ColdBootSeconds == 0.0, $"model: stub cold boot is 0 ({os})");
            }
            finally
            {
                try { Directory.Delete(dir, recursive: true); } catch { /* best effort */ }
            }
        }

        return failures;
    }

    private static int CheckValidationError()
    {
        var failures = 0;
        using var model = new BuildPlayerModel(new StubPlayerBuilder()) { OutputDir = string.Empty };
        var task = model.BuildAsync();
        var ok = task.Wait(TimeSpan.FromSeconds(30));
        failures += Check(ok, "model: empty-output build task completes");
        failures += Check(task.Result is null, "model: empty output dir yields no result");
        failures += Check(model.State == BuildPlayerState.Failed, "model: empty output dir fails");
        failures += Check(!string.IsNullOrEmpty(model.LastError), "model: validation error message set");
        failures += Check(!model.CanBuild, "model: CanBuild false without output dir");
        return failures;
    }

    private sealed class FailingBuilder : IPlayerBuilder
    {
        public PlayerBuildResult BuildPlayer(
            PlayerTargetOs targetOs, string outputDir, PlayerBuildConfig config,
            IProgress<double>? progress = null, CancellationToken cancellationToken = default) =>
            throw new InvalidOperationException("boom: fake backend failure");
    }

    private static int CheckBuilderError()
    {
        var failures = 0;
        var dir = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        using var model = new BuildPlayerModel(new FailingBuilder()) { OutputDir = dir };
        var task = model.BuildAsync();
        var ok = task.Wait(TimeSpan.FromSeconds(30));
        failures += Check(ok, "model: failing-backend task completes");
        failures += Check(task.Result is null, "model: failing backend yields no result");
        failures += Check(model.State == BuildPlayerState.Failed, "model: backend exception -> Failed");
        failures += Check(model.LastError.Contains("boom"), "model: backend error surfaces in LastError");
        failures += Check(model.StatusLine.Contains("boom"), "model: status line shows error");
        return failures;
    }

    private static int CheckBuildAndRunLaunches()
    {
        var failures = 0;
        var dir = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        try
        {
            string? launched = null;
            using var model = new BuildPlayerModel(
                new StubPlayerBuilder(),
                playerPath => { launched = playerPath; return true; })
            {
                TargetOs = PlayerTargetOs.Linux,
                Config = PlayerBuildConfig.Debug,
                OutputDir = dir,
            };
            var task = model.BuildAsync(andRun: true);
            var ok = task.Wait(TimeSpan.FromSeconds(30));
            failures += Check(ok, "model: build-and-run completes");
            failures += Check(model.LaunchRequested, "model: Build-And-Run requests launch");
            failures += Check(launched == task.Result?.PlayerPath, "model: launcher gets player path");
            failures += Check(model.LaunchedPath == launched, "model: LaunchedPath recorded");
        }
        finally
        {
            try { Directory.Delete(dir, recursive: true); } catch { /* best effort */ }
        }

        return failures;
    }

    private sealed class GatedBuilder : IPlayerBuilder
    {
        private readonly TaskCompletionSource _gate = new(TaskCreationOptions.RunContinuationsAsynchronously);

        public void Release() => _gate.TrySetResult();

        public PlayerBuildResult BuildPlayer(
            PlayerTargetOs targetOs, string outputDir, PlayerBuildConfig config,
            IProgress<double>? progress = null, CancellationToken cancellationToken = default)
        {
            _gate.Task.Wait(cancellationToken);
            Directory.CreateDirectory(outputDir);
            var player = Path.Combine(outputDir, "player");
            File.WriteAllText(player, "gated\n");
            return new PlayerBuildResult(player, Path.Combine(outputDir, "game.pak"),
                Path.Combine(outputDir, "Game.dll"), "gated build", 0.0, true);
        }
    }

    private static int CheckReentrancyGuard()
    {
        var failures = 0;
        var dir = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        try
        {
            var gate = new GatedBuilder();
            using var model = new BuildPlayerModel(gate) { OutputDir = dir };
            var first = model.BuildAsync();
            var second = model.BuildAsync(); // must be rejected while first is in flight
            var secondResult = second.Wait(TimeSpan.FromSeconds(30)) ? second.Result : new object();
            failures += Check(secondResult is null, "model: second build while busy returns null");
            gate.Release();
            var ok = first.Wait(TimeSpan.FromSeconds(30));
            failures += Check(ok && first.Result?.Succeeded == true, "model: gated build finishes after release");
            failures += Check(model.State == BuildPlayerState.Succeeded, "model: state Succeeded after gated build");
        }
        finally
        {
            try { Directory.Delete(dir, recursive: true); } catch { /* best effort */ }
        }

        return failures;
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine(ok ? $"[PASS] {name}" : $"[FAIL] {name}");
        return ok ? 0 : 1;
    }
}

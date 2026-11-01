// BuildService.cs — P3-10: headless-testable `dotnet build` over the game project.
//
// Every subprocess starts via ProcessStartInfo.ArgumentList, never a shell
// string (gotcha 22). Failures surface as ConsoleEntry errors naming the
// offending file (MSBuild `path(line,col): error CODE: message` parse).
using System.Diagnostics;
using System.Text.RegularExpressions;

namespace Engine.Editor;

/// <summary>Result of one `dotnet build`: exit code + parsed error entries.</summary>
public sealed record BuildResult(int ExitCode, IReadOnlyList<ConsoleEntry> Errors, string LogTail);

public static partial class BuildService
{
    /// <summary>
    /// Run `dotnet build` on the game project, routing errors into
    /// <paramref name="console"/>. Returns nonzero exit on failure.
    /// </summary>
    public static BuildResult Build(string gameProject, EditorConsole console, string configuration = "Release")
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(gameProject);
        ArgumentNullException.ThrowIfNull(console);
        if (!File.Exists(gameProject))
        {
            throw new ArgumentException($"game project not found: {gameProject}.", nameof(gameProject));
        }

        var psi = new ProcessStartInfo("dotnet")
        {
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        psi.ArgumentList.Add("build");
        psi.ArgumentList.Add(gameProject);
        psi.ArgumentList.Add("-c");
        psi.ArgumentList.Add(configuration);
        psi.ArgumentList.Add("--nologo");

        string stdout, stderr;
        int exit;
        using (var proc = Process.Start(psi) ?? throw new InvalidOperationException("build: could not start dotnet."))
        {
            // Async reads first: sync ReadToEnd before WaitForExit deadlocks
            // when the child fills a pipe (same pattern as ExportPipeline).
            var stdoutTask = proc.StandardOutput.ReadToEndAsync();
            var stderrTask = proc.StandardError.ReadToEndAsync();
            if (!proc.WaitForExit(TimeSpan.FromMinutes(10)))
            {
                try
                {
                    proc.Kill(entireProcessTree: true);
                }
                catch (Exception)
                {
                }

                throw new InvalidOperationException("build: dotnet build timed out.");
            }

            stdout = stdoutTask.GetAwaiter().GetResult();
            stderr = stderrTask.GetAwaiter().GetResult();
            exit = proc.ExitCode;
        }

        var errors = ParseErrors(stdout + "\n" + stderr);
        foreach (var e in errors)
        {
            console.Error(e.Text, e.File);
        }

        var tail = (stdout + "\n" + stderr).Trim();
        if (tail.Length > 2000)
        {
            tail = tail[^2000..];
        }

        return new BuildResult(exit, errors, tail);
    }

    /// <summary>Parse MSBuild `path(line,col): error CODE: message` lines (Windows + Unix paths).</summary>
    public static IReadOnlyList<ConsoleEntry> ParseErrors(string log)
    {
        var list = new List<ConsoleEntry>();
        if (string.IsNullOrEmpty(log))
        {
            return list;
        }

        foreach (var line in log.Split('\n'))
        {
            var m = MsBuildError().Match(line.Trim());
            if (!m.Success)
            {
                continue;
            }

            var file = m.Groups["file"].Value.Trim();
            var code = m.Groups["code"].Value.Trim();
            var msg = m.Groups["msg"].Value.Trim();
            list.Add(new ConsoleEntry(ConsoleLevel.Error, $"{code}: {msg}", string.IsNullOrEmpty(file) ? null : file));
        }

        return list;
    }

    [GeneratedRegex(@"^(?<file>.+?)\(\d+,\d+\):\s*error\s+(?<code>\S+):\s*(?<msg>.*)$", RegexOptions.Compiled)]
    private static partial Regex MsBuildError();
}

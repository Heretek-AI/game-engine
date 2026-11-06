// PlayerBuild.cs — P7-03 preflight: BuildPlayer contract for the editor Build window.
//
// Status: CONTRACT ONLY, no UI, no pipeline. Defines the exact surface the
// Build window (t_457e44cb) and backend wiring (t_13d15c63) will code against
// once P3-10 (#96, ProjectService) and P2-07 (#85, engine-player) merge.
// Follows M5-04 ExportPipeline.cs conventions (ArgumentList subprocesses,
// CliExportResult-style result, no engine_abi.h change).
namespace Engine.Editor;

/// <summary>Player build target OS (matches dotnet publish RIDs + P7-03 matrix).</summary>
public enum PlayerTargetOs
{
    Windows,
    Linux,
    MacOS,
}

/// <summary>Player build configuration.</summary>
public enum PlayerBuildConfig
{
    Debug,
    Release,
}

/// <summary>Result of <see cref="IPlayerBuilder.BuildPlayer"/>.</summary>
public sealed record PlayerBuildResult(
    string PlayerPath,
    string PakPath,
    string AssemblyPath,
    string Log,
    double ColdBootSeconds,
    bool Succeeded);

/// <summary>
/// Contract the editor Build window codes against. The real backend wires
/// this to the P2-07 engine-player + .pak pipeline; until then
/// <see cref="StubPlayerBuilder"/> satisfies it so the UI compiles and tests pass.
/// </summary>
public interface IPlayerBuilder
{
    PlayerBuildResult BuildPlayer(
        PlayerTargetOs targetOs,
        string outputDir,
        PlayerBuildConfig config,
        IProgress<double>? progress = null,
        CancellationToken cancellationToken = default);
}

/// <summary>
/// Filesystem stub behind <see cref="IPlayerBuilder"/>: writes placeholder
/// outputs into <c>outputDir</c> and reports progress, so the Build window
/// builds and headless tests pass before the real backend lands.
/// </summary>
public sealed class StubPlayerBuilder : IPlayerBuilder
{
    public PlayerBuildResult BuildPlayer(
        PlayerTargetOs targetOs,
        string outputDir,
        PlayerBuildConfig config,
        IProgress<double>? progress = null,
        CancellationToken cancellationToken = default)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(outputDir);
        Directory.CreateDirectory(outputDir);

        var suffix = targetOs switch
        {
            PlayerTargetOs.Windows => ".exe",
            PlayerTargetOs.MacOS => ".app",
            _ => string.Empty,
        };
        var playerPath = Path.Combine(outputDir, $"player{suffix}");
        var pakPath = Path.Combine(outputDir, "game.pak");
        var assemblyPath = Path.Combine(outputDir, "Game.dll");

        progress?.Report(0.0);
        cancellationToken.ThrowIfCancellationRequested();
        File.WriteAllText(playerPath, $"stub player {targetOs} {config}\n");
        progress?.Report(0.5);
        cancellationToken.ThrowIfCancellationRequested();
        File.WriteAllBytes(pakPath, []);
        File.WriteAllText(assemblyPath, "stub assembly\n");
        progress?.Report(1.0);

        return new PlayerBuildResult(
            playerPath, pakPath, assemblyPath,
            $"stub build {targetOs} {config}: backend not yet merged (P2-07 #85, P3-10 #96)",
            ColdBootSeconds: 0.0,
            Succeeded: true);
    }
}

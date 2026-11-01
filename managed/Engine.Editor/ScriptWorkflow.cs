// ScriptWorkflow.cs — P3-11: script workflow and hot reload in the editor.
//
// Watches `assets/**/*.cs`; on save rebuilds the game scripts in memory
// (Roslyn emit to byte[], no file locks — the compiler never fights the
// running editor) and reloads them through the existing collectible-ALC
// hot-reload (`ScriptHostManager`: M2-04, barrier <100ms, no state loss).
// Compile errors go to the console sink and the old assembly keeps running.
//
// "Open C# Project" shells the `.csproj` through the OS default handler
// (`ProcessStartInfo { UseShellExecute = true }`) — VS/Rider/VSCode pick up
// whatever the user registered.
//
// Editor-thread affinity: TriggerRebuild (watcher or test) marshals through
// the same barrier guard `ScriptHostManager` enforces (BeginFrame/EndFrame),
// so a reload never swaps code under running lanes.
using System.Diagnostics;
using System.Reflection;
using Engine.Runtime;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

namespace Engine.Editor;

/// <summary>One console line from the script workflow (Console panel model).</summary>
public enum ScriptLogLevel
{
    Info = 0,
    Warning = 1,
    Error = 2,
}

/// <summary>Console entry: level + text + optional source file.</summary>
public sealed record ScriptLogEntry(ScriptLogLevel Level, string Text, string? File = null);

/// <summary>
/// Rebuilds + hot-reloads `assets/**/*.cs` game scripts in the editor.
/// Stateless compile: each build emits fresh bytes from current sources.
/// </summary>
public sealed class ScriptBuilder
{
    private readonly ScriptHostManager _host;
    private readonly List<(string TypeName, string MethodName)> _systems;
    private bool _loaded;

    /// <summary>Console sink: every Info/Warning/Error lands here (Console panel).</summary>
    public List<ScriptLogEntry> Log { get; } = new();

    public ScriptBuilder(
        ScriptHostManager? host = null,
        IReadOnlyList<(string TypeName, string MethodName)>? systems = null)
    {
        _host = host ?? new ScriptHostManager();
        _systems = systems?.ToList() ?? new List<(string, string)>();
    }

    /// <summary>Bound systems (slot order = list order).</summary>
    public IReadOnlyList<(string TypeName, string MethodName)> Systems => _systems;

    /// <summary>Currently loaded generation (-1 = nothing loaded).</summary>
    public int Generation => _host.Generation;

    /// <summary>
    /// Register one chunk-runner entry point (call before <see cref="Rebuild"/>).
    /// </summary>
    public void AddSystem(string typeName, string methodName)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(typeName);
        ArgumentException.ThrowIfNullOrWhiteSpace(methodName);
        _systems.Add((typeName, methodName));
    }

    /// <summary>
    /// Rebuild all `.cs` sources under <paramref name="assetsDir"/> and
    /// hot-reload them. First call Loads, later calls Reload. On compile
    /// failure returns false, logs each Error, and the old assembly keeps
    /// running untouched. Thread-safe against frame dispatch: skipped while
    /// a frame is in flight (caller retries on the next barrier).
    /// </summary>
    public bool Rebuild(string assetsDir)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(assetsDir);
        var (ok, bytes, errors) = CompileDirectory(assetsDir);
        foreach (var e in errors)
        {
            Log.Add(e);
        }

        if (!ok || bytes == null)
        {
            return false; // old assembly keeps running
        }

        try
        {
            if (!_loaded)
            {
                var result = _host.Load(bytes, null, _systems);
                _loaded = true;
                Log.Add(new ScriptLogEntry(ScriptLogLevel.Info,
                    $"scripts loaded: generation {result.Generation}, {result.Bindings.Count} system(s)."));
            }
            else
            {
                var result = _host.Reload(bytes, null, _systems);
                Log.Add(new ScriptLogEntry(ScriptLogLevel.Info,
                    $"scripts reloaded: generation {result.Generation} in {result.BarrierTime.TotalMilliseconds:F1}ms."));
            }

            return true;
        }
        catch (Exception ex)
        {
            // Bind failure (missing runner, schema guard): old code stays bound.
            Log.Add(new ScriptLogEntry(ScriptLogLevel.Error, $"hot-reload failed, old assembly still running: {ex.Message}"));
            return false;
        }
    }

    /// <summary>
    /// Compile every `.cs` file under <paramref name="assetsDir"/> to one
    /// in-memory assembly. Returns (ok, bytes, logEntries). Diagnostics map
    /// to Warning/Error entries carrying the source file name.
    /// </summary>
    public static (bool Ok, byte[]? Bytes, List<ScriptLogEntry> Entries) CompileDirectory(string assetsDir)
    {
        var entries = new List<ScriptLogEntry>();
        if (!Directory.Exists(assetsDir))
        {
            entries.Add(new ScriptLogEntry(ScriptLogLevel.Error, $"assets dir not found: {assetsDir}"));
            return (false, null, entries);
        }

        var files = Directory.GetFiles(assetsDir, "*.cs", SearchOption.AllDirectories);
        Array.Sort(files, StringComparer.Ordinal);
        if (files.Length == 0)
        {
            entries.Add(new ScriptLogEntry(ScriptLogLevel.Error, $"no .cs sources under {assetsDir}"));
            return (false, null, entries);
        }

        var trees = new List<SyntaxTree>(files.Length);
        foreach (var f in files)
        {
            trees.Add(CSharpSyntaxTree.ParseText(File.ReadAllText(f), path: f));
        }

        var refs = AppDomain.CurrentDomain.GetAssemblies()
            .Where(a => !a.IsDynamic && !string.IsNullOrEmpty(a.Location))
            .Select(a => (MetadataReference)MetadataReference.CreateFromFile(a.Location))
            .ToList();

        var compilation = CSharpCompilation.Create(
            "GameScripts",
            trees,
            refs,
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary, allowUnsafe: true));

        using var ms = new MemoryStream();
        var emit = compilation.Emit(ms);
        foreach (var d in emit.Diagnostics.Where(d => d.Severity is DiagnosticSeverity.Warning or DiagnosticSeverity.Error))
        {
            var file = d.Location.SourceTree?.FilePath;
            entries.Add(new ScriptLogEntry(
                d.Severity == DiagnosticSeverity.Error ? ScriptLogLevel.Error : ScriptLogLevel.Warning,
                d.ToString(),
                file));
        }

        if (!emit.Success)
        {
            return (false, null, entries);
        }

        return (true, ms.ToArray(), entries);
    }

    /// <summary>
    /// Open the game `.csproj` with the OS default handler (IDE).
    /// Returns false + logs when the path is missing (no process launched).
    /// </summary>
    public bool OpenCSharpProject(string csprojPath)
    {
        if (string.IsNullOrWhiteSpace(csprojPath) || !File.Exists(csprojPath))
        {
            Log.Add(new ScriptLogEntry(ScriptLogLevel.Error, $"project not found: {csprojPath}"));
            return false;
        }

        try
        {
            Process.Start(new ProcessStartInfo { FileName = csprojPath, UseShellExecute = true });
            Log.Add(new ScriptLogEntry(ScriptLogLevel.Info, $"opened {csprojPath}"));
            return true;
        }
        catch (Exception ex)
        {
            Log.Add(new ScriptLogEntry(ScriptLogLevel.Error, $"open project failed: {ex.Message}", csprojPath));
            return false;
        }
    }
}

/// <summary>
/// `FileSystemWatcher` over `assets/**/*.cs`, debounced 200ms: bursts of
/// saves (multi-file IDE writes, atomic-save rename pairs) coalesce into one
/// rebuild callback. Dispose stops the watch.
/// </summary>
public sealed class ScriptWatcher : IDisposable
{
    private readonly FileSystemWatcher _watcher;
    private readonly Timer _debounce;
    private readonly Action _onSave;
    private bool _disposed;

    /// <summary>Debounce window (spec: 200ms).</summary>
    public static readonly TimeSpan DebounceDelay = TimeSpan.FromMilliseconds(200);

    public ScriptWatcher(string assetsDir, Action onSave)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(assetsDir);
        _onSave = onSave ?? throw new ArgumentNullException(nameof(onSave));
        if (!Directory.Exists(assetsDir))
        {
            throw new DirectoryNotFoundException($"assets dir not found: {assetsDir}");
        }

        _debounce = new Timer(_ => Fire(), null, Timeout.Infinite, Timeout.Infinite);
        _watcher = new FileSystemWatcher(assetsDir, "*.cs")
        {
            IncludeSubdirectories = true,
            NotifyFilter = NotifyFilters.LastWrite | NotifyFilters.FileName | NotifyFilters.CreationTime,
            EnableRaisingEvents = true,
        };
        _watcher.Changed += OnTouched;
        _watcher.Created += OnTouched;
        _watcher.Renamed += OnTouched;
    }

    private void OnTouched(object sender, FileSystemEventArgs e) => Arm();
    private void OnTouched(object sender, RenamedEventArgs e) => Arm();

    private void Arm()
    {
        if (_disposed)
        {
            return;
        }

        try
        {
            _debounce.Change(DebounceDelay, Timeout.InfiniteTimeSpan);
        }
        catch (ObjectDisposedException)
        {
        }
    }

    private void Fire()
    {
        if (_disposed)
        {
            return;
        }

        _onSave();
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _watcher.EnableRaisingEvents = false;
        _watcher.Dispose();
        _debounce.Dispose();
    }
}

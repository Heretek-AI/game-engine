// EditorConsole.cs — P3-10: level-filtered in-memory console over the build/export path.
//
// The P3-09 log-sink ABI (log_set_sink) does not exist yet; this sink is fed
// by BuildService/ExportPipeline callers and the File-menu handlers so the
// console panel has a headless-testable model now, with the native sink to
// follow in P3-09 without changing the entry shape.
namespace Engine.Editor;

/// <summary>Console entry severity (mirrors the future native sink levels).</summary>
public enum ConsoleLevel
{
    Info = 0,
    Warning = 1,
    Error = 2,
}

/// <summary>One console line: severity + text (+ optional source file).</summary>
public sealed record ConsoleEntry(ConsoleLevel Level, string Text, string? File = null);

/// <summary>Thread-safe in-memory console with level filters + collapse of repeats.</summary>
public sealed class EditorConsole
{
    private readonly List<ConsoleEntry> _entries = new();
    private readonly object _gate = new();

    public bool ShowInfo { get; set; } = true;

    public bool ShowWarnings { get; set; } = true;

    public bool ShowErrors { get; set; } = true;

    public void Clear()
    {
        lock (_gate)
        {
            _entries.Clear();
        }
    }

    public void Write(ConsoleLevel level, string text, string? file = null)
    {
        ArgumentNullException.ThrowIfNull(text);
        lock (_gate)
        {
            _entries.Add(new ConsoleEntry(level, text, file));
        }
    }

    public void Info(string text) => Write(ConsoleLevel.Info, text);

    public void Warning(string text, string? file = null) => Write(ConsoleLevel.Warning, text, file);

    public void Error(string text, string? file = null) => Write(ConsoleLevel.Error, text, file);

    /// <summary>Visible entries after level filters (collapse merges adjacent duplicates).</summary>
    public IReadOnlyList<ConsoleEntry> Visible(bool collapse = false)
    {
        List<ConsoleEntry> snap;
        bool info, warn, err;
        lock (_gate)
        {
            snap = new List<ConsoleEntry>(_entries);
            info = ShowInfo;
            warn = ShowWarnings;
            err = ShowErrors;
        }

        var list = new List<ConsoleEntry>(snap.Count);
        foreach (var e in snap)
        {
            var show = e.Level switch
            {
                ConsoleLevel.Info => info,
                ConsoleLevel.Warning => warn,
                ConsoleLevel.Error => err,
                _ => true,
            };
            if (!show)
            {
                continue;
            }

            if (collapse && list.Count > 0)
            {
                var last = list[^1];
                if (last.Level == e.Level && last.Text == e.Text && last.File == e.File)
                {
                    continue;
                }
            }

            list.Add(e);
        }

        return list;
    }

    /// <summary>All entries, unfiltered (tests + export diagnostics).</summary>
    public IReadOnlyList<ConsoleEntry> All()
    {
        lock (_gate)
        {
            return new List<ConsoleEntry>(_entries);
        }
    }

    public int ErrorCount()
    {
        lock (_gate)
        {
            var n = 0;
            foreach (var e in _entries)
            {
                if (e.Level == ConsoleLevel.Error)
                {
                    n++;
                }
            }

            return n;
        }
    }
}

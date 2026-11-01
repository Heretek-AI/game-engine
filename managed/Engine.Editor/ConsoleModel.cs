// ConsoleModel.cs — P3-09: console panel model over the native log ring.
//
// A headless-safe view-model (no GPU): mirrors the native ConsoleLog ring
// into immutable Entry records (level + text). Refresh() drains
// log_count/log_read through any live world handle (the ring is
// process-global, so the world is only a read key). Filters (level
// toggles + substring search), Clear, and collapse-duplicates are pure
// view state over the last Refresh — the docking ListBox binds Filtered.
//
// UnmanagedCallersOnly sink: InstallSink wires log_set_sink so native
// logs forward into the model live (used by the Avalonia console panel).
// The callback appends under a lock and never calls back into native
// (no re-entrancy: native documents the sink fires without its lock).
using System.Runtime.InteropServices;
using System.Text;
using Engine.Bindings;

namespace Engine.Editor;

/// <summary>One console line: severity + text.</summary>
public sealed record ConsoleEntry(LogLevel Level, string Text);

/// <summary>Console panel model: native ring mirror + filter/clear/collapse view state.</summary>
public sealed unsafe class ConsoleModel : IDisposable
{
    private readonly NativeApi.ApiTable* _table;
    private readonly NativeApi.EngineWorld* _world;
    private readonly object _gate = new();
    private readonly List<ConsoleEntry> _entries = new();
    private bool _disposed;

    /// <summary>Show Info lines (default true).</summary>
    public bool ShowInfo { get; set; } = true;

    /// <summary>Show Warning lines (default true).</summary>
    public bool ShowWarning { get; set; } = true;

    /// <summary>Show Error lines (default true).</summary>
    public bool ShowError { get; set; } = true;

    /// <summary>Substring filter (empty = no filtering). Case-insensitive.</summary>
    public string Search { get; set; } = string.Empty;

    /// <summary>Collapse adjacent duplicates into one row with a repeat count.</summary>
    public bool CollapseDuplicates { get; set; }

    public ConsoleModel(NativeApi.ApiTable* table, NativeApi.EngineWorld* world)
    {
        if (table == null)
        {
            throw new ArgumentNullException(nameof(table));
        }

        if (world == null)
        {
            throw new ArgumentNullException(nameof(world));
        }

        _table = table;
        _world = world;
    }

    /// <summary>All mirrored lines (oldest first).</summary>
    public IReadOnlyList<ConsoleEntry> Entries
    {
        get
        {
            lock (_gate)
            {
                return _entries.ToArray();
            }
        }
    }

    /// <summary>Filtered view: level toggles + search (+ collapse when enabled).</summary>
    public IReadOnlyList<(ConsoleEntry Entry, int Repeats)> Filtered
    {
        get
        {
            List<ConsoleEntry> snapshot;
            bool collapse;
            lock (_gate)
            {
                snapshot = new List<ConsoleEntry>(_entries);
                collapse = CollapseDuplicates;
            }

            var search = Search;
            var out_ = new List<(ConsoleEntry, int)>();
            ConsoleEntry? pending = null;
            var pendingCount = 0;
            void Flush()
            {
                if (pending != null)
                {
                    out_.Add((pending, pendingCount));
                    pending = null;
                    pendingCount = 0;
                }
            }

            foreach (var e in snapshot)
            {
                if ((e.Level == LogLevel.Info && !ShowInfo)
                    || (e.Level == LogLevel.Warning && !ShowWarning)
                    || (e.Level == LogLevel.Error && !ShowError))
                {
                    continue;
                }

                if (!string.IsNullOrEmpty(search)
                    && e.Text.Contains(search, StringComparison.OrdinalIgnoreCase) == false)
                {
                    continue;
                }

                if (collapse && pending != null && pending.Level == e.Level && pending.Text == e.Text)
                {
                    pendingCount++;
                    continue;
                }

                Flush();
                pending = e;
                pendingCount = 1;
            }

            Flush();
            return out_;
        }
    }

    /// <summary>Number of mirrored lines.</summary>
    public int Count
    {
        get
        {
            lock (_gate)
            {
                return _entries.Count;
            }
        }
    }

    /// <summary>Drain the native ring (index 0 = oldest) into the mirror.</summary>
    public void Refresh()
    {
        ThrowIfDisposed();
        var count = _table->EngineLogCount(_world);
        var fresh = new List<ConsoleEntry>((int)Math.Min(count, int.MaxValue));
        var buf = new byte[4096];
        fixed (byte* p = buf)
        {
            for (ulong i = 0; i < count; i++)
            {
                int level;
                var rc = _table->EngineLogRead(_world, i, &level, p, (uint)buf.Length);
                if (rc != 0)
                {
                    throw new InvalidOperationException($"log_read({i}) failed with code {rc}.");
                }

                var end = Array.IndexOf(buf, (byte)0);
                var text = Encoding.UTF8.GetString(buf, 0, end < 0 ? buf.Length : end);
                fresh.Add(new ConsoleEntry((LogLevel)level, text));
            }
        }

        lock (_gate)
        {
            _entries.Clear();
            _entries.AddRange(fresh);
        }
    }

    /// <summary>Drop the native ring and the mirror.</summary>
    public void Clear()
    {
        ThrowIfDisposed();
        _table->EngineLogClear(_world);
        lock (_gate)
        {
            _entries.Clear();
        }
    }

    /// <summary>
    /// Install the live forwarder: native log_write appends call this
    /// model's sink. The sink handle is a GCHandle to this model (freed on
    /// Dispose). Re-install replaces the previous sink (native holds one
    /// process-global slot); Dispose clears it.
    /// </summary>
    public void InstallSink()
    {
        ThrowIfDisposed();
        var handle = GCHandle.Alloc(this);
        var ctx = (void*)GCHandle.ToIntPtr(handle);
        var rc = _table->EngineLogSetSink(_world, &SinkAppend, ctx);
        if (rc != 0)
        {
            handle.Free();
            throw new InvalidOperationException($"log_set_sink failed with code {rc}.");
        }

        lock (_gate)
        {
            _sinkHandle = handle;
        }
    }

    private GCHandle _sinkHandle;

    [System.Runtime.InteropServices.UnmanagedCallersOnly(CallConvs = new[] { typeof(System.Runtime.CompilerServices.CallConvCdecl) })]
    private static void SinkAppend(int level, byte* msg, void* user)
    {
        if (msg == null || user == null)
        {
            return;
        }

        ConsoleModel? self;
        try
        {
            self = (ConsoleModel?)GCHandle.FromIntPtr((IntPtr)user).Target;
        }
        catch
        {
            return;
        }

        if (self == null)
        {
            return;
        }

        var len = 0;
        while (len < 4096 && msg[len] != 0)
        {
            len++;
        }

        string text;
        try
        {
            text = Encoding.UTF8.GetString(msg, len);
        }
        catch
        {
            return;
        }

        lock (self._gate)
        {
            if (self._disposed)
            {
                return;
            }

            self._entries.Add(new ConsoleEntry((LogLevel)level, text));
        }
    }

    private void ThrowIfDisposed() => ObjectDisposedException.ThrowIf(_disposed, this);

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        try
        {
            _table->EngineLogSetSink(_world, null, null);
        }
        catch
        {
        }

        lock (_gate)
        {
            if (_sinkHandle.IsAllocated)
            {
                _sinkHandle.Free();
            }
        }
    }
}

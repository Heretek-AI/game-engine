// EditorLog.cs — P3-09: C# log entry point over the native console ring.
//
// Debug.Log/Warning/Error write UTF-8 lines via EngineLogWrite (level
// 0/1/2). The native ConsoleLog ring is process-global; reads go through
// the world's log_count/log_read (any live world). log_set_sink installs
// a process-wide forwarder (see ConsoleModel.InstallSink): the sink fires
// synchronously on the logging thread — Debug never installs one itself,
// so a burst of managed logs cannot re-enter managed code.
using System.Text;
using Engine.Bindings;

namespace Engine.Editor;

/// <summary>Log severity. Mirrors ENGINE_ABI_LOG_* (Info = 0, Warning = 1, Error = 2).</summary>
public enum LogLevel
{
    Info = 0,
    Warning = 1,
    Error = 2,
}

/// <summary>C# log entry point over the native console ring (ABI 4.8).</summary>
public static unsafe class EditorLog
{
    /// <summary>Table used for log writes. Test seam: defaults to the loaded ABI table.</summary>
#pragma warning disable CA2211
    public static NativeApi.ApiTable* TableOverride = null;
#pragma warning restore CA2211

    private static NativeApi.ApiTable* Table =>
        TableOverride != null ? TableOverride : NativeApi.ApiLoader.Table;

    /// <summary>Info-level log line.</summary>
    public static void Log(string message) => Write(LogLevel.Info, message);

    /// <summary>Warning-level log line.</summary>
    public static void Warning(string message) => Write(LogLevel.Warning, message);

    /// <summary>Error-level log line.</summary>
    public static void Error(string message) => Write(LogLevel.Error, message);

    private static void Write(LogLevel level, string message)
    {
        ArgumentNullException.ThrowIfNull(message);
        var bytes = Encoding.UTF8.GetBytes(message + "\0");
        int rc;
        fixed (byte* p = bytes)
        {
            rc = Table->EngineLogWrite((int)level, p);
        }

        if (rc != 0)
        {
            throw new InvalidOperationException($"log_write failed with code {rc}.");
        }
    }
}

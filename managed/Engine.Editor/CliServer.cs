// CliServer.cs — M5-03: Unix-socket / named-pipe JSON-RPC server in Engine.Editor.
//
// CI pipelines, terminal scripts, and AI coding agents connect, send one
// JSON-RPC 2.0 object per line (NDJSON), and read back one response line
// per request. Transport:
//
//   Linux/macOS: Unix-domain socket at EngineIpc.SocketPathFor(name)
//     (default name "engine-cli": $XDG_RUNTIME_DIR or $TMPDIR fallback,
//     "engine-<name>.sock"). Windows: named pipe \\.\pipe\engine-<name>.
//
// Concurrency: one background listener thread accepts clients; each client
// gets a thread-pool work item that serves lines SEQUENTIALLY (the editor
// session is editor-thread-affine like the rest of the UI — concurrent
// requests on one connection still serialize; concurrent CONNECTIONS also
// serialize on a lock for V1, documented below).
//
// V1 scope note: requests dispatch on pool threads under a dispatch lock,
// NOT on the real editor UI thread (the docking UI owns that pump in
// M5-04). Safe because headless/CI/test hosts have no UI thread; the
// docking editor must route dispatch through its own sync context (TODO).
using System.Net.Sockets;
using System.Text;

namespace Engine.Editor;

/// <summary>
/// Socket/pipe endpoint resolution (shared by server + engine-cli client).
/// </summary>
public static class EngineIpc
{
    /// <summary>Filesystem path (Unix socket) or pipe name (Windows) for a server name.</summary>
    public static string SocketPathFor(string name)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(name);
        if (OperatingSystem.IsWindows())
        {
            return $@"\\.\pipe\engine-{name}";
        }

        var dir = Environment.GetEnvironmentVariable("XDG_RUNTIME_DIR")
            ?? Environment.GetEnvironmentVariable("TMPDIR")
            ?? "/tmp";
        return Path.Combine(dir.TrimEnd('/', '\\'), $"engine-{name}.sock");
    }
}

/// <summary>
/// Embedded JSON-RPC server: binds the endpoint, serves NDJSON lines via
/// a CliDispatcher bound to the editor session. Start/Stop idempotent;
/// Dispose stops. One server per session (V1).
/// </summary>
public sealed class CliServer : IDisposable
{
    private readonly CliDispatcher _dispatcher;
    private readonly object _dispatchLock = new();
    private Socket? _listener;
    private CancellationTokenSource? _cts;
    private Thread? _acceptThread;
    private bool _disposed;

    /// <summary>Endpoint this server listens on (socket path or pipe name).</summary>
    public string Endpoint { get; }

    public CliServer(EditorSession session, string name = "engine-cli")
    {
        ArgumentNullException.ThrowIfNull(session);
        ArgumentException.ThrowIfNullOrWhiteSpace(name);
        _dispatcher = new CliDispatcher(session);
        Endpoint = EngineIpc.SocketPathFor(name);
    }

    /// <summary>True between Start and Stop.</summary>
    public bool IsRunning => _acceptThread?.IsAlive == true;

    /// <summary>Bind and start accepting (idempotent; no-op when running).</summary>
    public void Start()
    {
        ThrowIfDisposed();
        if (IsRunning)
        {
            return;
        }

        if (OperatingSystem.IsWindows())
        {
            throw new PlatformNotSupportedException("Named-pipe transport lands with the M5-04 docking editor (Unix sockets only in V1).");
        }

        try
        {
            if (File.Exists(Endpoint))
            {
                File.Delete(Endpoint);
            }
        }
        catch (IOException)
        {
            // Stale socket unlink best-effort; bind below reports the real error.
        }

        var listener = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
        listener.Bind(new UnixDomainSocketEndPoint(Endpoint));
        listener.Listen(backlog: 8);

        var cts = new CancellationTokenSource();
        _listener = listener;
        _cts = cts;
        _acceptThread = new Thread(() => AcceptLoop(listener, cts.Token))
        {
            IsBackground = true,
            Name = "engine-cli-accept",
        };
        _acceptThread.Start();
    }

    /// <summary>Stop accepting and release the endpoint (idempotent).</summary>
    /// <remarks>
    /// The listener thread polls (200ms) and exits on cancel — Join is
    /// bounded and prompt on every OS. The socket file is unlinked
    /// best-effort so the next Start's Bind never trips on a stale .sock.
    /// </remarks>
    public void Stop()
    {
        var cts = Interlocked.Exchange(ref _cts, null);
        try
        {
            cts?.Cancel();
        }
        catch (ObjectDisposedException)
        {
        }
        finally
        {
            cts?.Dispose();
        }

        var listener = Interlocked.Exchange(ref _listener, null);
        var endpoint = Endpoint;
        try
        {
            listener?.Close();
        }
        catch (SocketException)
        {
        }
        finally
        {
            listener?.Dispose();
        }

        var thread = Interlocked.Exchange(ref _acceptThread, null);
        if (thread != null && thread != Thread.CurrentThread && thread.IsAlive)
        {
            thread.Join(TimeSpan.FromSeconds(10));
        }

        // Best-effort socket-file cleanup (a stale .sock breaks the next
        // Start's Bind when the listener thread already released it).
        try
        {
            if (!OperatingSystem.IsWindows() && File.Exists(endpoint))
            {
                File.Delete(endpoint);
            }
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }

    /// <summary>
    /// Serve one connected stream synchronously (NDJSON in, NDJSON out).
    /// Public for tests: feeds scripted request lines without a socket.
    /// </summary>
    public void ServeStream(Stream stream, CancellationToken ct = default)
    {
        ArgumentNullException.ThrowIfNull(stream);
        using var reader = new StreamReader(stream, Encoding.UTF8, leaveOpen: true);
        using var writer = new StreamWriter(stream, Encoding.UTF8, leaveOpen: true)
        {
            NewLine = "\n",
            AutoFlush = true,
        };

        while (!ct.IsCancellationRequested)
        {
            string? line;
            try
            {
                line = reader.ReadLine();
            }
            catch (IOException)
            {
                break;
            }

            if (line == null)
            {
                break;
            }

            if (line.Length == 0 || Encoding.UTF8.GetByteCount(line) > CliProtocol.MaxLineBytes)
            {
                var id = System.Text.Json.JsonDocument.Parse("null").RootElement;
                try
                {
                    writer.WriteLine(CliProtocol.EncodeError(id, CliErrorCodes.InvalidRequest, "empty or oversize line."));
                }
                catch (IOException)
                {
                    break;
                }

                continue;
            }

            string response;
            lock (_dispatchLock)
            {
                response = _dispatcher.DispatchLine(line);
            }

            try
            {
                writer.WriteLine(response);
            }
            catch (IOException)
            {
                break;
            }
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        Stop();
    }

    private void AcceptLoop(Socket listener, CancellationToken ct)
    {
        // Poll with a short timeout instead of blocking in Accept(): the
        // listener thread must wake promptly for Stop() on every OS. A
        // blocking Accept() is not reliably woken by Close() on macOS
        // runners (hang -> ctest timeout); Poll+Pending re-checks the
        // token every 200ms with zero steady-state cost (one syscall per
        // idle window, no clients connected).
        while (!ct.IsCancellationRequested)
        {
            bool pending;
            try
            {
                pending = listener.Poll(200_000, SelectMode.SelectRead);
            }
            catch (ObjectDisposedException)
            {
                break;
            }
            catch (SocketException)
            {
                break;
            }

            if (!pending)
            {
                continue;
            }

            Socket client;
            try
            {
                client = listener.Accept();
            }
            catch (ObjectDisposedException)
            {
                break;
            }
            catch (SocketException)
            {
                break;
            }

            ThreadPool.QueueUserWorkItem(static state =>
            {
                var (self, sock, token) = ((CliServer, Socket, CancellationToken))state!;
                try
                {
                    using var stream = new NetworkStream(sock, ownsSocket: false);
                    self.ServeStream(stream, token);
                }
                catch (Exception ex) when (ex is IOException || ex is SocketException || ex is ObjectDisposedException || ex is InvalidOperationException)
                {
                    // One faulty client must not terminate the host editor
                    // (unhandled pool-thread exceptions kill the process).
                }
                finally
                {
                    sock.Dispose();
                }
            }, (this, client, ct));
        }
    }

    private void ThrowIfDisposed() => ObjectDisposedException.ThrowIf(_disposed, this);
}

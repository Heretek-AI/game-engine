// Program.cs — M5-03: standalone engine-cli control-plane client.
//
// Talks to the Editor CliServer over the Unix-socket NDJSON transport
// (EngineIpc.SocketPathFor) with one JSON-RPC 2.0 request per invocation:
//
//   engine-cli scene tree [--socket name]
//   engine-cli entity get <id> [--socket name]
//   engine-cli entity set <id> <Component.Field> <val> [--socket name]
//   engine-cli snapshot capture --out frame.png --pass final [--socket name]
//   engine-cli profile [--frames 60] [--socket name]
//
// Every command prints the server's `result` payload as compact JSON to
// stdout and exits 0. Server-side errors print `error <code>: <message>`
// to stderr and exit 1; transport failures exit 2. No native dependency:
// the client only needs the socket path (it never loads engine_core).
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using Engine.Editor;

namespace Engine.Cli;

internal static class Program
{
    private static readonly JsonSerializerOptions Pretty = new() { WriteIndented = true };

    internal static int Main(string[] args)
    {
        string socket = "engine-cli";
        var rest = new List<string>(args.Length);
        for (var i = 0; i < args.Length; i++)
        {
            if ((args[i] == "--socket" || args[i] == "--socket-name") && i + 1 < args.Length)
            {
                socket = args[++i];
            }
            else
            {
                rest.Add(args[i]);
            }
        }

        if (rest.Count == 0 || rest[0] is "-h" or "--help" or "help")
        {
            PrintUsage();
            return rest.Count == 0 ? 2 : 0;
        }

        try
        {
            return Run(rest, socket);
        }
        catch (CliTransportException ex)
        {
            Console.Error.WriteLine($"engine-cli: transport: {ex.Message}");
            return 2;
        }
        catch (CliServerException ex)
        {
            Console.Error.WriteLine($"engine-cli: error {ex.Code}: {ex.Message}");
            return 1;
        }
    }

    private static int Run(List<string> rest, string socket)
    {
        var verb = rest[0].ToLowerInvariant();
        if (verb.Equals("scene", StringComparison.Ordinal))
            return RunScene(rest, socket);
        if (verb.Equals("entity", StringComparison.Ordinal))
            return RunEntity(rest, socket);
        if (verb.Equals("snapshot", StringComparison.Ordinal))
            return RunSnapshot(rest, socket);
        if (verb.Equals("profile", StringComparison.Ordinal))
            return RunProfile(rest, socket);
        return Fail($"unknown command '{rest[0]}'.");
    }

    // --- scene tree ----------------------------------------------------------

    private static int RunScene(List<string> rest, string socket)
    {
        if (rest.Count != 2 || !rest[1].Equals("tree", StringComparison.OrdinalIgnoreCase))
        {
            return Fail("usage: engine-cli scene tree [--socket name]");
        }

        var result = Send(socket, "scene.tree", null);
        Console.WriteLine(JsonSerializer.Serialize(result, Pretty));
        return 0;
    }

    // --- entity get/set ------------------------------------------------------

    private static int RunEntity(List<string> rest, string socket)
    {
        if (rest.Count < 2)
        {
            return Fail("usage: engine-cli entity (get <id> | set <id> <Component.Field> <val>)");
        }

        var sub = rest[1];
        if (sub.Equals("get", StringComparison.OrdinalIgnoreCase))
        {
            if (rest.Count != 3 || !ulong.TryParse(rest[2], out var id) || id == 0)
            {
                return Fail("usage: engine-cli entity get <id>");
            }

            var result = Send(socket, "entity.get", new Dictionary<string, object?> { ["id"] = id });
            Console.WriteLine(JsonSerializer.Serialize(result, Pretty));
            return 0;
        }

        if (sub.Equals("set", StringComparison.OrdinalIgnoreCase))
        {
            // engine-cli entity set <id> <Component.Field> <val>
            // <val> is one number, except LocalTransform.all which takes
            // 10 comma-separated numbers (px,py,pz,qx,qy,qz,qw,sx,sy,sz).
            if (rest.Count != 5 || !ulong.TryParse(rest[2], out var id) || id == 0)
            {
                return Fail("usage: engine-cli entity set <id> <Component.Field> <val>");
            }

            var field = rest[3];
            var comp = field.Contains('.') ? field[(field.LastIndexOf('.') + 1)..] : field;
            object value;
            if (comp.Equals("all", StringComparison.OrdinalIgnoreCase))
            {
                var parts = rest[4].Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
                if (parts.Length != 10)
                {
                    return Fail("LocalTransform.all needs 10 comma-separated numbers.");
                }

                var nums = new float[10];
                for (var i = 0; i < 10; i++)
                {
                    if (!float.TryParse(parts[i], System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out nums[i]))
                    {
                        return Fail($"'{parts[i]}' is not a number.");
                    }
                }

                value = nums;
            }
            else
            {
                if (!float.TryParse(rest[4], System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var f))
                {
                    return Fail($"'{rest[4]}' is not a number.");
                }

                value = f;
            }

            var result = Send(socket, "entity.set", new Dictionary<string, object?>
            {
                ["id"] = id,
                ["field"] = field,
                ["value"] = value,
            });
            Console.WriteLine(JsonSerializer.Serialize(result, Pretty));
            return 0;
        }

        return Fail("usage: engine-cli entity (get <id> | set <id> <Component.Field> <val>)");
    }

    // --- snapshot capture ----------------------------------------------------

    private static int RunSnapshot(List<string> rest, string socket)
    {
        // engine-cli snapshot capture --out frame.png --pass [final|...]
        if (rest.Count < 2 || !rest[1].Equals("capture", StringComparison.OrdinalIgnoreCase))
        {
            return Fail("usage: engine-cli snapshot capture --out frame.png --pass [final|albedo|normals|depth|wireframe]");
        }

        var outPath = "frame.png";
        var pass = "final";
        for (var i = 2; i < rest.Count; i++)
        {
            if (rest[i] == "--out" && i + 1 < rest.Count)
            {
                outPath = rest[++i];
            }
            else if (rest[i] == "--pass" && i + 1 < rest.Count)
            {
                pass = rest[++i];
            }
            else
            {
                return Fail("usage: engine-cli snapshot capture --out frame.png --pass [final|albedo|normals|depth|wireframe]");
            }
        }

        var result = Send(socket, "snapshot.capture", new Dictionary<string, object?>
        {
            ["out"] = outPath,
            ["pass"] = pass,
        });
        Console.WriteLine(JsonSerializer.Serialize(result, Pretty));
        return 0;
    }

    // --- profile -------------------------------------------------------------

    private static int RunProfile(List<string> rest, string socket)
    {
        // engine-cli profile [--frames 60]
        var frames = 60;
        for (var i = 1; i < rest.Count; i++)
        {
            if (rest[i] == "--frames" && i + 1 < rest.Count && int.TryParse(rest[i + 1], out var f))
            {
                frames = f;
                i++;
            }
            else
            {
                return Fail("usage: engine-cli profile [--frames 60]");
            }
        }

        var result = Send(socket, "profile.capture", new Dictionary<string, object?> { ["frames"] = frames });
        Console.WriteLine(JsonSerializer.Serialize(result, Pretty));
        return 0;
    }

    // --- transport ------------------------------------------------------------

    private static JsonElement Send(string socketName, string method, object? @params)
    {
        var path = EngineIpc.SocketPathFor(socketName);
        if (OperatingSystem.IsWindows())
        {
            throw new CliTransportException("named-pipe transport lands with M5-04 (Unix sockets only in V1).");
        }

        using var sock = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
        try
        {
            sock.Connect(new UnixDomainSocketEndPoint(path));
        }
        catch (SocketException ex)
        {
            throw new CliTransportException($"cannot connect to {path} (is the Editor running?): {ex.Message}");
        }

        sock.ReceiveTimeout = 30_000;
        sock.SendTimeout = 30_000;
        using var stream = new NetworkStream(sock, ownsSocket: true);
        var line = CliProtocol.EncodeRequest(method, @params, 1) + "\n";
        try
        {
            stream.Write(Encoding.UTF8.GetBytes(line));
            stream.Flush();
        }
        catch (IOException ex)
        {
            throw new CliTransportException($"write failed: {ex.Message}");
        }

        using var reader = new StreamReader(stream, Encoding.UTF8, leaveOpen: true);
        string? response;
        try
        {
            response = reader.ReadLine();
        }
        catch (IOException ex)
        {
            throw new CliTransportException($"read failed: {ex.Message}");
        }

        if (response == null)
        {
            throw new CliTransportException("server closed the connection without a response.");
        }

        var resp = CliProtocol.TryParseResponse(response);
        if (resp == null)
        {
            throw new CliTransportException("unparseable response from server.");
        }

        if (resp.Error != null)
        {
            throw new CliServerException(resp.Error.Code, resp.Error.Message);
        }

        return resp.Result.ValueKind == JsonValueKind.Undefined
            ? JsonDocument.Parse("null").RootElement
            : resp.Result;
    }

    private static int Fail(string message)
    {
        Console.Error.WriteLine($"engine-cli: {message}");
        PrintUsage();
        return 2;
    }

    private static void PrintUsage()
    {
        Console.Error.WriteLine("""
            usage: engine-cli [--socket name] <command>
              scene tree
              entity get <id>
              entity set <id> <Component.Field> <val>
              snapshot capture --out frame.png --pass [final|albedo|normals|depth|wireframe]
              profile [--frames 60]
            """);
    }

    private sealed class CliTransportException(string message) : Exception(message);

    private sealed class CliServerException(int code, string message) : Exception(message)
    {
        public int Code { get; } = code;
    }
}

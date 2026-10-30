// CliProtocol.cs — M5-03: engine-cli JSON-RPC control-plane wire protocol.
//
// One JSON-RPC 2.0 message per line (NDJSON) over a Unix-domain socket
// (Linux/macOS) or named pipe (Windows). The Engine.Editor CliServer owns
// the server half; the standalone engine-cli binary (Engine.Cli) owns the
// client half. Every AC verbs maps to a method:
//
//   scene.tree       -> { nodes: [{ id, name, prefab, children }] }
//   entity.get       -> { id, name, prefab, local: [10 floats] }
//   entity.set       -> set LocalTransform field(s), returns updated pose
//   snapshot.capture -> headless frame capture stub (native PNG path lands
//                       with the Vulkan viewport in M5-04)
//   profile.capture  -> per-system CPU timings + compositor pass results
//
// Framing: request/response objects are System.Text.Json-serialized with
// one trailing '\n' per message and a 64 KiB max line (editor scenes fit;
// oversize lines fail closed with a JSON-RPC -32600 error). Method names
// are exact-match (case-sensitive); unknown methods return -32601.
//
// Zero native dependency: the protocol layer only moves DTOs. Transport
// (sockets/pipes) lives in CliServer.cs; native reads go through
// EditorSession/HierarchyModel/Inspector (ABI 4.1 tail, zero new ABI).
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Engine.Editor;

/// <summary>JSON-RPC 2.0 error codes used by the CLI control plane.</summary>
public static class CliErrorCodes
{
    public const int ParseError = -32700;
    public const int InvalidRequest = -32600;
    public const int MethodNotFound = -32601;
    public const int InvalidParams = -32602;
    public const int InternalError = -32603;
}

/// <summary>Wire options: camelCase, compact, no trailing whitespace.</summary>
public static class CliJson
{
    public static readonly JsonSerializerOptions Options = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        WriteIndented = false,
    };
}

/// <summary>One hierarchy row on the wire (HierarchyModel shape + name).</summary>
public sealed record CliSceneNode(ulong Id, string? Name, bool Prefab, IReadOnlyList<CliSceneNode> Children);

/// <summary>Full entity state on the wire (pose = 10-float Local p/q/s).</summary>
public sealed record CliEntityState(ulong Id, string? Name, bool Prefab, float[] Local);

/// <summary>Snapshot capture result (headless stub until M5-04 viewport).</summary>
public sealed record CliSnapshotResult(string Out, string Pass, int Width, int Height, bool HeadlessStub);

/// <summary>One timed system sample (microseconds, Stopwatch-backed).</summary>
public sealed record CliSystemTiming(string Name, double Microseconds);

/// <summary>
/// Profile capture result: N timed frames over the native session plus
/// compositor pass outcomes. GPU pass timings are host-side CPU timings
/// until the Vulkan timestamp-query path lands (M5-04); the schema is
/// stable so CI parsers written now keep working.
/// </summary>
public sealed record CliProfileResult(
    int Frames,
    double TotalMilliseconds,
    double MeanFrameMicroseconds,
    IReadOnlyList<CliSystemTiming> Systems,
    IReadOnlyList<string> Passes);

/// <summary>JSON-RPC 2.0 request envelope (params stay a raw element).</summary>
public sealed class CliRequest
{
    public string Jsonrpc { get; set; } = "2.0";
    public string Method { get; set; } = string.Empty;
    public JsonElement Params { get; set; }
    public JsonElement Id { get; set; }
}

/// <summary>JSON-RPC 2.0 response envelope.</summary>
public sealed class CliResponse
{
    public string Jsonrpc { get; set; } = "2.0";
    public JsonElement Result { get; set; }
    public CliResponseError? Error { get; set; }
    public JsonElement Id { get; set; }
}

/// <summary>JSON-RPC 2.0 error payload.</summary>
public sealed class CliResponseError
{
    public int Code { get; set; }
    public string Message { get; set; } = string.Empty;
    public string? Data { get; set; }
}

/// <summary>
/// Builds JSON-RPC envelopes (requests, results, errors). Shared by the
/// server (CliServer) and the client (engine-cli) so both sides agree on
/// framing: one compact-JSON object + '\n' per message.
/// </summary>
public static class CliProtocol
{
    /// <summary>Max one NDJSON line in bytes (64 KiB; oversize fails closed).</summary>
    public const int MaxLineBytes = 64 * 1024;

    private static readonly JsonElement NullResult =
        JsonDocument.Parse("null").RootElement.Clone();

    public static string EncodeRequest(string method, object? @params, object? id)
    {
        var req = new Dictionary<string, object?>
        {
            ["jsonrpc"] = "2.0",
            ["method"] = method,
            ["id"] = id,
        };
        if (@params != null)
        {
            req["params"] = @params;
        }

        return JsonSerializer.Serialize(req, CliJson.Options);
    }

    public static string EncodeResult(JsonElement id, object? result)
    {
        var resultEl = result == null
            ? NullResult
            : JsonSerializer.SerializeToElement(result, CliJson.Options);
        var envelope = new Dictionary<string, object?>
        {
            ["jsonrpc"] = "2.0",
            ["result"] = resultEl,
            ["id"] = id.ValueKind == JsonValueKind.Undefined ? null : id.Clone(),
        };
        return JsonSerializer.Serialize(envelope, CliJson.Options);
    }

    public static string EncodeError(JsonElement id, int code, string message, string? data = null)
    {
        var envelope = new Dictionary<string, object?>
        {
            ["jsonrpc"] = "2.0",
            ["error"] = new Dictionary<string, object?>
            {
                ["code"] = code,
                ["message"] = message,
                ["data"] = data,
            },
            ["id"] = id.ValueKind == JsonValueKind.Undefined ? null : id.Clone(),
        };
        return JsonSerializer.Serialize(envelope, CliJson.Options);
    }

    public static CliRequest? TryParseRequest(string line, out string? error)
    {
        error = null;
        JsonDocument doc;
        try
        {
            doc = JsonDocument.Parse(line);
        }
        catch (JsonException ex)
        {
            error = ex.Message;
            return null;
        }

        using (doc)
        {
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object
                || !root.TryGetProperty("method", out var methodEl)
                || methodEl.ValueKind != JsonValueKind.String)
            {
                error = "missing string 'method'.";
                return null;
            }

            var req = new CliRequest
            {
                Method = methodEl.GetString() ?? string.Empty,
            };
            if (root.TryGetProperty("jsonrpc", out var ver) && ver.ValueKind == JsonValueKind.String)
            {
                req.Jsonrpc = ver.GetString() ?? "2.0";
            }

            if (root.TryGetProperty("params", out var @params))
            {
                req.Params = @params.Clone();
            }

            if (root.TryGetProperty("id", out var id))
            {
                req.Id = id.Clone();
            }

            return req;
        }
    }

    public static CliResponse? TryParseResponse(string line)
    {
        try
        {
            using var doc = JsonDocument.Parse(line);
            var root = doc.RootElement.Clone();
            var resp = new CliResponse();
            if (root.TryGetProperty("result", out var result))
            {
                resp.Result = result.Clone();
            }

            if (root.TryGetProperty("error", out var err) && err.ValueKind == JsonValueKind.Object)
            {
                var e = new CliResponseError();
                if (err.TryGetProperty("code", out var code) && code.TryGetInt32(out var c))
                {
                    e.Code = c;
                }

                if (err.TryGetProperty("message", out var msg) && msg.ValueKind == JsonValueKind.String)
                {
                    e.Message = msg.GetString() ?? string.Empty;
                }

                if (err.TryGetProperty("data", out var data) && data.ValueKind == JsonValueKind.String)
                {
                    e.Data = data.GetString();
                }

                resp.Error = e;
            }

            if (root.TryGetProperty("id", out var id))
            {
                resp.Id = id.Clone();
            }

            return resp;
        }
        catch (JsonException)
        {
            return null;
        }
    }
}

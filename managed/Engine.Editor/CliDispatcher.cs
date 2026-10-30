// CliDispatcher.cs — M5-03: JSON-RPC method dispatch over an EditorSession.
//
// Pure request -> response mapping (no sockets here): CliServer feeds it
// one NDJSON line and writes back the returned line. All native reads go
// through the ABI 4.1 editor tail via EditorSession/HierarchyModel/
// Inspector — zero new ABI surface, zero GPU dependency.
//
// Methods (case-sensitive, params as a JSON object):
//   scene.tree                     {} -> { nodes: CliSceneNode[] }
//   entity.get                     { id } -> CliEntityState
//   entity.set                     { id, field?, pose? } -> CliEntityState
//     field: "LocalTransform.<component>" where component is px/py/pz/
//       qx/qy/qz/qw/sx/sy/sz (case-insensitive) or "all" with value =
//       10-number array. pose: full 10-float array (alternative form).
//   snapshot.capture               { out?, pass? } -> CliSnapshotResult
//     pass in final|albedo|normals|depth|wireframe (default final).
//     Headless stub (no Vulkan viewport yet): records the request and
//     returns the scene stats a CI gate needs; the native PNG path lands
//     with the M5-04 viewport. out defaults to frame.png.
//   profile.capture                { frames? } -> CliProfileResult
//     Times N hierarchy-refresh + pose-read rounds (defaults 60) with
//     Stopwatch (microsecond resolution); systems[] breaks down the two
//     measured scopes (hierarchy.refresh_us, pose.read_us); passes[]
//     echoes the compositor pass names when a compositor is attached
//     (empty headless — schema stable for CI parsers).
using System.Diagnostics;
using System.Text.Json;

namespace Engine.Editor;

/// <summary>
/// Dispatches one JSON-RPC request line against a bound editor session.
/// Thread-affinity: call on the editor thread (EditorSession is not
/// thread-safe); CliServer serializes connections onto one thread.
/// </summary>
public sealed class CliDispatcher
{
    private static readonly string[] SnapshotPasses =
        ["final", "albedo", "normals", "depth", "wireframe"];

    private readonly EditorSession _session;
    private readonly HierarchyModel _hierarchy;
    private readonly Inspector _inspector;

    public CliDispatcher(EditorSession session)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));
        _hierarchy = new HierarchyModel(session);
        _inspector = new Inspector(session);
    }

    /// <summary>Dispatch one NDJSON line; always returns one NDJSON line.</summary>
    public string DispatchLine(string line)
    {
        var req = CliProtocol.TryParseRequest(line, out var parseError);
        if (req == null)
        {
            var id = JsonDocument.Parse("null").RootElement;
            var code = parseError?.Contains("method") == true
                ? CliErrorCodes.InvalidRequest
                : CliErrorCodes.ParseError;
            return CliProtocol.EncodeError(id, code, "invalid request.", parseError);
        }

        if (req.Jsonrpc != "2.0")
        {
            return CliProtocol.EncodeError(req.Id, CliErrorCodes.InvalidRequest, "jsonrpc must be \"2.0\".");
        }

        try
        {
            object result = req.Method switch
            {
                "scene.tree" => SceneTree(),
                "entity.get" => EntityGet(Params(req)),
                "entity.set" => EntitySet(Params(req)),
                "snapshot.capture" => SnapshotCapture(Params(req)),
                "profile.capture" => ProfileCapture(Params(req)),
                _ => throw new InvalidOperationException($"unknown method '{req.Method}'."),
            };
            return CliProtocol.EncodeResult(req.Id, result);
        }
        catch (ArgumentException ex)
        {
            return CliProtocol.EncodeError(req.Id, CliErrorCodes.InvalidParams, ex.Message);
        }
        catch (InvalidOperationException ex) when (ex.Message.StartsWith("unknown method", StringComparison.Ordinal))
        {
            return CliProtocol.EncodeError(req.Id, CliErrorCodes.MethodNotFound, ex.Message);
        }
        catch (InvalidOperationException ex)
        {
            return CliProtocol.EncodeError(req.Id, CliErrorCodes.InternalError, ex.Message);
        }
    }

    private static JsonElement Params(CliRequest req) =>
        req.Params.ValueKind == JsonValueKind.Undefined
            ? JsonDocument.Parse("{}").RootElement
            : req.Params;

    // --- scene.tree ---------------------------------------------------------

    private Dictionary<string, object?> SceneTree()
    {
        var roots = _hierarchy.Refresh();
        var nodes = new List<CliSceneNode>(roots.Count);
        foreach (var root in roots)
        {
            nodes.Add(ToNode(root));
        }

        return new Dictionary<string, object?> { ["nodes"] = nodes };
    }

    private static CliSceneNode ToNode(HierarchyNode node)
    {
        string? name = null;
        try
        {
            // Names are not exposed on the read path of ABI 4.1; identity is
            // the id. Keep the field (null when unnamed) so the schema is
            // stable when name reads land.
            name = null;
        }
        catch (InvalidOperationException)
        {
            name = null;
        }

        var kids = new List<CliSceneNode>(node.Children.Count);
        foreach (var child in node.Children)
        {
            kids.Add(ToNode(child));
        }

        return new CliSceneNode(node.Id, name, node.IsPrefab, kids);
    }

    // --- entity.get / entity.set --------------------------------------------

    private static ulong RequireId(JsonElement @params)
    {
        if (@params.ValueKind != JsonValueKind.Object
            || !@params.TryGetProperty("id", out var idEl)
            || idEl.ValueKind != JsonValueKind.Number
            || !idEl.TryGetUInt64(out var id)
            || id == 0)
        {
            throw new ArgumentException("params.id must be a non-zero entity id.");
        }

        return id;
    }

    private CliEntityState EntityGet(JsonElement @params)
    {
        var id = RequireId(@params);
        return ReadState(id);
    }

    private CliEntityState ReadState(ulong id)
    {
        var pose = new float[10];
        _session.GetLocal(id, pose);
        return new CliEntityState(id, null, _session.IsPrefabInstance(id), pose);
    }

    private CliEntityState EntitySet(JsonElement @params)
    {
        var id = RequireId(@params);

        float[] pose;
        if (@params.TryGetProperty("pose", out var poseEl))
        {
            pose = ReadPoseArray(poseEl, "params.pose");
        }
        else if (@params.TryGetProperty("field", out var fieldEl)
            && @params.TryGetProperty("value", out var valueEl))
        {
            var current = new float[10];
            _session.GetLocal(id, current);
            if (fieldEl.ValueKind != JsonValueKind.String)
            {
                throw new ArgumentException("params.field must be a string like \"LocalTransform.px\".");
            }

            ApplyField(current, fieldEl.GetString() ?? string.Empty, valueEl);
            pose = current;
        }
        else
        {
            throw new ArgumentException("params must carry pose[10] or field+value.");
        }

        _session.SetLocal(id, pose);
        return ReadState(id);
    }

    private static float[] ReadPoseArray(JsonElement el, string what)
    {
        if (el.ValueKind != JsonValueKind.Array)
        {
            throw new ArgumentException($"{what} must be an array of 10 numbers.");
        }

        var list = new List<float>(10);
        foreach (var item in el.EnumerateArray())
        {
            if (item.ValueKind != JsonValueKind.Number || !item.TryGetSingle(out var f))
            {
                throw new ArgumentException($"{what} must be an array of 10 numbers.");
            }

            list.Add(f);
        }

        if (list.Count != 10)
        {
            throw new ArgumentException($"{what} must be exactly 10 numbers (px,py,pz,qx,qy,qz,qw,sx,sy,sz).");
        }

        return [.. list];
    }

    private static void ApplyField(float[] pose, string field, JsonElement value)
    {
        // Accepted: "LocalTransform.px" | "LocalTransform.all" | bare "px".
        var dot = field.LastIndexOf('.');
        var comp = (dot >= 0 ? field[(dot + 1)..] : field).Trim().ToLowerInvariant();
        if (comp == "all")
        {
            var full = ReadPoseArray(value, "params.value");
            Array.Copy(full, pose, 10);
            return;
        }

        if (value.ValueKind != JsonValueKind.Number || !value.TryGetSingle(out var f))
        {
            throw new ArgumentException("params.value must be a number for single-component fields.");
        }

        var index = comp switch
        {
            "px" => 0,
            "py" => 1,
            "pz" => 2,
            "qx" => 3,
            "qy" => 4,
            "qz" => 5,
            "qw" => 6,
            "sx" => 7,
            "sy" => 8,
            "sz" => 9,
            _ => -1,
        };
        if (index < 0)
        {
            throw new ArgumentException($"unknown component '{field}' (want LocalTransform.px|py|pz|qx|qy|qz|qw|sx|sy|sz|all).");
        }

        pose[index] = f;
    }

    // --- snapshot.capture ----------------------------------------------------

    private static CliSnapshotResult SnapshotCapture(JsonElement @params)
    {
        var pass = "final";
        if (@params.ValueKind == JsonValueKind.Object
            && @params.TryGetProperty("pass", out var passEl)
            && passEl.ValueKind == JsonValueKind.String)
        {
            pass = (passEl.GetString() ?? "final").Trim().ToLowerInvariant();
        }

        if (Array.IndexOf(SnapshotPasses, pass) < 0)
        {
            throw new ArgumentException($"params.pass must be one of: {string.Join("|", SnapshotPasses)}.");
        }

        var outPath = "frame.png";
        if (@params.ValueKind == JsonValueKind.Object
            && @params.TryGetProperty("out", out var outEl)
            && outEl.ValueKind == JsonValueKind.String
            && !string.IsNullOrWhiteSpace(outEl.GetString()))
        {
            outPath = outEl.GetString()!;
        }

        // Headless stub: no Vulkan viewport in this process yet (M5-04).
        // CI visual-regression gates assert on the envelope (pass echo +
        // stub flag), not pixels; pixel assertions enable with the viewport.
        return new CliSnapshotResult(outPath, pass, 0, 0, HeadlessStub: true);
    }

    // --- profile.capture -----------------------------------------------------

    private CliProfileResult ProfileCapture(JsonElement @params)
    {
        var frames = 60;
        if (@params.ValueKind == JsonValueKind.Object
            && @params.TryGetProperty("frames", out var framesEl)
            && framesEl.ValueKind == JsonValueKind.Number
            && framesEl.TryGetInt32(out var f))
        {
            frames = f;
        }

        if (frames <= 0 || frames > 10_000)
        {
            throw new ArgumentException("params.frames must be 1..10000.");
        }

        // Two measured scopes per frame: hierarchy refresh (roots+children
        // walk) and a pose read of the first root (when any). Stopwatch
        // ticks -> microseconds (double, no per-frame allocation: scratch
        // buffers reused across frames, panels refresh on demand anyway).
        var scratch = new float[10];
        long hierarchyTicks = 0;
        long poseTicks = 0;
        var total = Stopwatch.StartNew();
        for (var i = 0; i < frames; i++)
        {
            var t0 = Stopwatch.GetTimestamp();
            var roots = _hierarchy.Refresh();
            hierarchyTicks += Stopwatch.GetTimestamp() - t0;

            if (roots.Count > 0)
            {
                var t1 = Stopwatch.GetTimestamp();
                _session.GetLocal(roots[0].Id, scratch);
                poseTicks += Stopwatch.GetTimestamp() - t1;
            }
        }

        total.Stop();
        var freq = (double)Stopwatch.Frequency;
        var systems = new List<CliSystemTiming>(2)
        {
            new("hierarchy.refresh_us", hierarchyTicks * 1_000_000.0 / freq),
            new("pose.read_us", poseTicks * 1_000_000.0 / freq),
        };

        return new CliProfileResult(
            frames,
            total.Elapsed.TotalMilliseconds,
            total.Elapsed.TotalMicroseconds / frames,
            systems,
            Passes: []);
    }
}

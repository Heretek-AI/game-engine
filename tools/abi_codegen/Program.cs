// abi_codegen — M1-04: parses native/bridge/engine_abi.h and emits
// managed/Engine.Bindings/NativeApi.g.cs.
//
// The tool is intentionally a SMALL line parser, not a C parser: the ABI
// header follows a strict machine-friendly subset (see FORMAT below) so
// that adding an entry is one line in the header + rebuild. Anything the
// parser does not understand is a hard error (fail-closed: never emit a
// half-correct binding).
//
// FORMAT (subset of C the parser accepts):
//   #define ENGINE_ABI_VERSION_MAJOR <int>  (likewise MINOR/PATCH)
//   #define ENGINE_ABI_COLUMN_STRIDE <int>
//   typedef struct EngineApiTable { uint64_t size; int32_t abi_version_*;
//     int32_t column_stride; <FnTypedef> <field>; ... } EngineApiTable;
//     -- the field ORDER is the emission order; every Fn typedef must be
//     referenced (stale typedefs are a hard error).
//   typedef <ret> (*<Name>Fn)(<params>);   -- function-pointer entries
//     where <ret>/<params> use only: void, int32_t, uint64_t, float,
//     const char*, EngineWorld*, const EcsChunkView*, const FrameDispatch*,
//     ManagedSystemCallback, void*.
//   typedef struct EcsChunkView { <ctype> <name>; ... } EcsChunkView;
//     ctype := const-qualified pointer | int32_t | void* (user_data)
//   Component structs (EngineLocalTransform/EngineWorldTransform) may use
//     only float scalars and float arrays; emission preserves header order.
//   ManagedSystemCallback typedef line is recognized and skipped
//     (the C# side declares the UnmanagedCallersOnly signature by hand).
//
// EMIT: NativeApi.g.cs with
//   * version constants + Expected* asserts,
//   * blittable LocalTransform / WorldTransform / EcsChunkView structs with
//     explicit Sequential layout + Pack, and a compile-time size guard,
//   * an unsafe ApiTable struct mirroring the header's field order, whose
//     function pointers use SELECTIVE suppression: only register_system
//     (a guaranteed non-blocking store) is
//     `delegate* unmanaged[Cdecl, SuppressGCTransition]`; run/propagate/
//     lifecycle calls are plain `Cdecl` because they may block on worker
//     completion, and suppressing the GC transition across a blocking
//     native call can stall the runtime,
//   * a loader that resolves engine_get_api_table via NativeLibrary and
//     validates size/version/stride before first use.
//
// Usage:
//   abi_codegen <repo-root>   (defaults to repo root inferred from CWD)
// Reads native/bridge/include/engine/bridge/engine_abi.h (or the legacy
// native/bridge/engine_abi.h), writes managed/Engine.Bindings/NativeApi.g.cs.
using System.Text;
using System.Text.RegularExpressions;

namespace AbiCodegen;

internal sealed record AbiVersion(int Major, int Minor, int Patch);
internal sealed record StructField(string CType, string Name, int ArrayLen, int ArrayLen2 = 0);
internal sealed record StructDef(string Name, List<StructField> Fields);
internal sealed record FnParam(string CType, string Name);
internal sealed record FnDef(string Name, string Return, List<FnParam> Params);

internal static class Program
{
    private static readonly Regex DefineRe = new(@"^#define\s+(ENGINE_ABI_VERSION_MAJOR|ENGINE_ABI_VERSION_MINOR|ENGINE_ABI_VERSION_PATCH|ENGINE_ABI_COLUMN_STRIDE)\s+(\d+)\s*$", RegexOptions.Compiled);
    private static readonly Regex FnTypedefRe = new(@"^typedef\s+(.+?)\s*\(\*(\w+)\)\((.*)\)\s*;\s*$", RegexOptions.Compiled);
    private static readonly Regex GenericDefineRe = new(@"^#define\s+ENGINE_ABI_(MAX_TERMS|ACCESS_IN|ACCESS_INOUT|ERR_NULL_ARG|ERR_BAD_ARG|ERR_SIZE_MISMATCH|ERR_NOT_FOUND|ERR_IN_RUN|ERR_INTERNAL)\s+(-?\d+)\s*$", RegexOptions.Compiled);
    private static readonly Regex FieldRe = new(@"^([\w\s\*]+?)\s+(\w+)((?:\s*\[\s*\d+\s*\]){1,2})?\s*$", RegexOptions.Compiled);
    private static readonly Regex DimRe = new(@"\[\s*(\d+)\s*\]", RegexOptions.Compiled);

    internal static int Main(string[] args)
    {
        try
        {
            Run(args.Length > 0 ? args[0] : FindRepoRoot());
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"abi_codegen: error: {ex.Message}");
            return 1;
        }
    }

    private static string FindRepoRoot()
    {
        var dir = new DirectoryInfo(Directory.GetCurrentDirectory());
        while (dir != null)
        {
            if (File.Exists(Path.Combine(dir.FullName, "Engine.slnx")))
            {
                return dir.FullName;
            }

            dir = dir.Parent;
        }

        throw new InvalidOperationException("cannot locate repo root (no Engine.slnx found); pass <repo-root> explicitly.");
    }

    private static void Run(string repoRoot)
    {
        var header = Path.Combine(repoRoot, "native", "bridge", "include", "engine", "bridge", "engine_abi.h");
        if (!File.Exists(header))
        {
            header = Path.Combine(repoRoot, "native", "bridge", "engine_abi.h");
        }

        if (!File.Exists(header))
        {
            throw new InvalidOperationException($"ABI header not found: {header}");
        }

        var lines = File.ReadAllLines(header);
        var version = ParseVersion(lines, header);
        var stride = ParseStride(lines, header);
        var structs = ParseStructs(lines, header);
        var fns = ParseFns(lines, header);

        var local = structs.Single(s => s.Name == "EngineLocalTransform");
        var world = structs.Single(s => s.Name == "EngineWorldTransform");
        var chunk = structs.Single(s => s.Name == "EcsChunkView");
        AssertComponentLayout(local, world);

        var generic = ParseGenericDefines(lines, header);
        var cs = Emit(header, version, stride, structs, chunk, fns, lines, generic);
        var outPath = Path.Combine(repoRoot, "managed", "Engine.Bindings", "NativeApi.g.cs");
        File.WriteAllText(outPath, cs, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
        Console.WriteLine($"abi_codegen: {fns.Count} entries -> {outPath}");
    }

    private static AbiVersion ParseVersion(string[] lines, string header)
    {
        var vals = new Dictionary<string, int>();
        foreach (var line in lines)
        {
            var m = DefineRe.Match(line.Trim());
            if (m.Success && m.Groups[1].Value.StartsWith("ENGINE_ABI_VERSION_", StringComparison.Ordinal))
            {
                vals[m.Groups[1].Value] = int.Parse(m.Groups[2].Value, System.Globalization.CultureInfo.InvariantCulture);
            }
        }

        foreach (var key in new[] { "ENGINE_ABI_VERSION_MAJOR", "ENGINE_ABI_VERSION_MINOR", "ENGINE_ABI_VERSION_PATCH" })
        {
            if (!vals.TryGetValue(key, out _))
            {
                throw new InvalidOperationException($"{header}: missing {key}");
            }
        }

        return new AbiVersion(vals["ENGINE_ABI_VERSION_MAJOR"], vals["ENGINE_ABI_VERSION_MINOR"], vals["ENGINE_ABI_VERSION_PATCH"]);
    }

    // ABI 4.2: the generic-tail #defines (MAX_TERMS, ACCESS_*, ERR_*). All
    // nine are required; emission order is fixed by GenericConstNames.
    private static Dictionary<string, int> ParseGenericDefines(string[] lines, string header)
    {
        var vals = new Dictionary<string, int>(StringComparer.Ordinal);
        foreach (var line in lines)
        {
            var m = GenericDefineRe.Match(line.Trim());
            if (m.Success)
            {
                vals[m.Groups[1].Value] = int.Parse(m.Groups[2].Value, System.Globalization.CultureInfo.InvariantCulture);
            }
        }

        foreach (var (key, _) in GenericConstNames)
        {
            if (!vals.ContainsKey(key))
            {
                throw new InvalidOperationException($"{header}: missing #define ENGINE_ABI_{key}");
            }
        }

        if (vals["MAX_TERMS"] != 8)
        {
            throw new InvalidOperationException($"{header}: ENGINE_ABI_MAX_TERMS must be 8 (EngineChunkColumns arrays are [8]).");
        }

        return vals;
    }

    private static readonly (string Key, string CsName)[] GenericConstNames =
    [
        ("MAX_TERMS", "MaxTerms"),
        ("ACCESS_IN", "AccessIn"),
        ("ACCESS_INOUT", "AccessInOut"),
        ("ERR_NULL_ARG", "ErrNullArg"),
        ("ERR_BAD_ARG", "ErrBadArg"),
        ("ERR_SIZE_MISMATCH", "ErrSizeMismatch"),
        ("ERR_NOT_FOUND", "ErrNotFound"),
        ("ERR_IN_RUN", "ErrInRun"),
        ("ERR_INTERNAL", "ErrInternal"),
    ];

    private static int ParseStride(string[] lines, string header)
    {
        foreach (var line in lines)
        {
            var m = DefineRe.Match(line.Trim());
            if (m.Success && m.Groups[1].Value == "ENGINE_ABI_COLUMN_STRIDE")
            {
                return int.Parse(m.Groups[2].Value, System.Globalization.CultureInfo.InvariantCulture);
            }
        }

        throw new InvalidOperationException($"{header}: missing ENGINE_ABI_COLUMN_STRIDE");
    }

    private static List<StructDef> ParseStructs(string[] lines, string header)
    {
        var structs = new List<StructDef>();
        for (var i = 0; i < lines.Length; i++)
        {
            var t = lines[i].Trim();
            if (!t.StartsWith("typedef struct", StringComparison.Ordinal))
            {
                continue;
            }

            // Multi-line: "typedef struct EcsChunkView {" ... "} Name;".
            // A struct body ends at the line containing "} Name;" — a bare
            // ';' also terminates single-field inline declarations, so scan
            // for the closing brace instead.
            // Opaque forward declarations ("typedef struct EngineWorld EngineWorld;")
            // carry no body: skip them before the brace scan.
            if (System.Text.RegularExpressions.Regex.IsMatch(
                    t, @"^typedef\s+struct\s+\w+\s+\w+\s*;$"))
            {
                continue;
            }

            var sb = new StringBuilder(StripComment(t));
            while (!sb.ToString().Contains('}') && i + 1 < lines.Length)
            {
                sb.Append(' ').Append(StripComment(lines[++i].Trim()));
            }
            var closed = sb.ToString();
            var braceAt = closed.IndexOf('}');
            if (braceAt >= 0)
            {
                // Keep through the terminating "; " of THIS struct; later
                // same-line typedefs belong to the next iteration.
                var semiAt = closed.IndexOf(';', braceAt);
                if (semiAt >= 0)
                {
                    sb.Length = 0;
                    sb.Append(closed[..(semiAt + 1)]);
                }
            }

            if (sb.ToString().Contains('}') && !sb.ToString().TrimEnd().EndsWith(';'))
            {
                // The closing "} Name;" may sit on the line after '}'.
                while (i + 1 < lines.Length)
                {
                    sb.Append(' ').Append(StripComment(lines[++i].Trim()));
                    if (sb.ToString().TrimEnd().EndsWith(';'))
                    {
                        break;
                    }
                }
            }

            // Strip an optional alignas(...) between `struct` and the tag: the
            // parser only needs the trailing typedef name.
            var body = Regex.Replace(
                sb.ToString(),
                @"^typedef\s+struct\s+(?:\w+\s*\(\s*\d+\s*\)\s*)?",
                "typedef struct ",
                RegexOptions.Singleline);
            var m = Regex.Match(body, @"^typedef\s+struct\s+(\w+)?\s*\{(.*)\}\s*(\w+)\s*;\s*$", RegexOptions.Singleline);
            if (!m.Success)
            {
                throw new InvalidOperationException($"{header}:{i + 1}: cannot parse struct: {body}");
            }

            var name = m.Groups[3].Value;
            var fields = new List<StructField>();
            foreach (var raw in m.Groups[2].Value.Split(';'))
            {
                var f = raw.Trim();
                if (f.Length == 0)
                {
                    continue;
                }

                var fm = FieldRe.Match(f);
                if (!fm.Success)
                {
                    throw new InvalidOperationException($"{header}:{i + 1}: cannot parse field: {f}");
                }

                var dims = DimRe.Matches(fm.Groups[3].Value).Select(d => int.Parse(d.Groups[1].Value, System.Globalization.CultureInfo.InvariantCulture)).ToArray();
                if (dims.Length > 2)
                {
                    throw new InvalidOperationException($"{header}:{i + 1}: field {fm.Groups[2].Value} has {dims.Length} dimensions (max 2).");
                }
                var arr = dims.Length > 0 ? dims[0] : 0;
                var arr2 = dims.Length > 1 ? dims[1] : 0;
                fields.Add(new StructField(NormType(fm.Groups[1].Value), fm.Groups[2].Value, arr, arr2));
            }

            structs.Add(new StructDef(name, fields));
        }

        foreach (var want in new[] { "EngineLocalTransform", "EngineWorldTransform", "EcsChunkView", "FrameDispatch", "EngineChunkColumns", "EngineInputState" })
        {
            if (structs.All(s => s.Name != want))
            {
                throw new InvalidOperationException($"{header}: missing struct {want}");
            }
        }

        var dispatch = structs.Single(s => s.Name == "FrameDispatch");
        AssertFrameDispatch(dispatch, header);

        return structs;
    }

    private static string StripComment(string line)
    {
        var idx = line.IndexOf("//", StringComparison.Ordinal);
        return idx < 0 ? line : line[..idx].TrimEnd();
    }

    private static string NormType(string c) => Regex.Replace(c.Trim(), @"\s+", " ");

    private static List<FnDef> ParseFns(string[] lines, string header)
    {
        var fns = new List<FnDef>();
        for (var i = 0; i < lines.Length; i++)
        {
            var t = lines[i].Trim();
            if (!t.StartsWith("typedef", StringComparison.Ordinal) || !t.Contains("(*", StringComparison.Ordinal))
            {
                continue;
            }

            var fb = new StringBuilder(StripComment(t));
            while (i + 1 < lines.Length && !FnTypedefRe.IsMatch(fb.ToString()))
            {
                fb.Append(' ').Append(StripComment(lines[++i].Trim()));
                if (fb.Length > 2000)
                {
                    break;
                }
            }

            var m = FnTypedefRe.Match(fb.ToString());
            if (!m.Success)
            {
                throw new InvalidOperationException($"{header}:{i + 1}: cannot parse fn typedef: {fb}");
            }

            var name = m.Groups[2].Value;
            if (name == "ManagedSystemCallback")
            {
                continue; // declared by hand on the C# side (UnmanagedCallersOnly).
            }

            var ret = NormType(m.Groups[1].Value);
            var plist = m.Groups[3].Value.Trim();
            var ps = new List<FnParam>();
            if (plist != "void" && plist.Length != 0)
            {
                foreach (var raw in plist.Split(','))
                {
                    var p = raw.Trim();
                    // Split "const EcsChunkView* view" -> type + last ident.
                    var pm = Regex.Match(p, @"^(.*\S)\s+(\w+)$");
                    if (!pm.Success)
                    {
                        throw new InvalidOperationException($"{header}:{i + 1}: cannot parse param: {p}");
                    }

                    ps.Add(new FnParam(NormType(pm.Groups[1].Value), pm.Groups[2].Value));
                }
            }

            ValidateCType(ret, header, i + 1);
            foreach (var p in ps)
            {
                ValidateCType(p.CType, header, i + 1);
            }

            fns.Add(new FnDef(name, ret, ps));
        }

        var required = new[]
        {
            "EngineWorldCreateFn", "EngineWorldDestroyFn", "EngineWorldBulkCreateFn",
            "EngineWorldPropagateFn", "EngineRegisterSystemFn", "EngineRunSystemsFn",
            "EngineRenderPassCreateFn", "EngineRenderPassDestroyFn",
            "EngineRenderPassAddImageFn", "EngineRenderPassAddPassFn",
            "EngineRenderPassCompileFn", "EngineRenderPassRunFrameFn",
            "EngineRenderPassPassResultFn", "EngineRenderPassFailedCountFn",
            "EnginePhysicsRaycastFn", "EnginePhysicsSphereCastFn",
            "EnginePhysicsOverlapSphereFn", "EnginePhysicsRaycastBatchFn",
            "EngineEditorCreateFn", "EngineEditorDestroyFn",
            "EngineEditorSpawnFn", "EngineEditorSetNameFn",
            "EngineEditorSetParentFn", "EngineEditorAddLocalFn",
            "EngineEditorSetLocalFn", "EngineEditorGetLocalFn",
            "EngineEditorCountChildrenFn", "EngineEditorGetChildFn",
            "EngineEditorCountRootsFn", "EngineEditorGetRootFn",
            "EngineEditorIsPrefabInstanceFn", "EngineEditorBeginEditFn",
            "EngineEditorEndEditFn", "EngineEditorSnapshotTakeFn",
            "EngineEditorSnapshotRestoreFn", "EngineEditorSnapshotDiscardFn",
            "EnginePhysics2DCreateFn", "EnginePhysics2DDestroyFn",
            "EnginePhysics2DBodyBoxFn", "EnginePhysics2DBodyCircleFn",
            "EnginePhysics2DBodyPolygonFn", "EnginePhysics2DBodyDestroyFn",
            "EnginePhysics2DStepFn", "EnginePhysics2DDrainFn",
            "EngineComponentRegisterFn", "EngineEntityCreateFn",
            "EngineEntitySetFn", "EngineEntityGetFn",
            "EngineSystemRegisterFn", "EngineSystemsRunFn",
            "EngineInputGetStateFn",
            "EngineRendererCreateFn", "EngineRendererDestroyFn",
            "EngineRendererFrameFn", "EngineRendererMeshCreateFn",
            "EngineRendererScreenshotFn",
            "EngineNavCreateFn", "EngineNavDestroyFn",
            "EngineNavBakeFn", "EngineNavAgentAddFn",
            "EngineNavAgentGotoFn", "EngineNavAgentUpdateFn",
            "EngineNavAgentPosFn", "EngineNavAgentReachedFn",
            "EngineRendererCreateOffscreenFn",
            "EngineRendererRenderToBufferFn", "EngineRendererPickFn",
            "EngineProjectDropAssetFn", "EngineEditorDeleteEntityFn",
            "EngineRendererGpuTimeFn",
            "EngineLogWriteFn", "EngineLogSetSinkFn", "EngineLogReadFn",
            "EngineLogCountFn", "EngineLogClearFn", "EngineProfileResetFn",
            "EngineProfileFrameFn", "EngineProfileCountFn",
            "EngineProfileFrameTimeFn", "EngineProfileSystemTimeFn",
            "EngineProfileSystemCountFn",
        };
        foreach (var r in required)
        {
            if (fns.All(f => f.Name != r))
            {
                throw new InvalidOperationException($"{header}: missing ABI entry {r}");
            }
        }

        return fns;
    }

    private static void ValidateCType(string ctype, string header, int line)
    {
        var ok = ctype is "void" or "int32_t" or "uint32_t" or "uint64_t" or "void*" or "float"
            or "const char*" or "const char**" or "const float*" or "float*"
            or "uint32_t*" or "uint64_t*" or "EngineWorld*" or "const EngineWorld*"
            or "EngineRenderPassRegistry*" or "const EngineRenderPassRegistry*"
            or "EnginePhysicsWorld*" or "const EnginePhysicsWorld*"
            or "const EngineRay*" or "EngineHit*"
            or "EnginePhysics2DWorld*" or "const EnginePhysics2DWorld*"
            or "const EngineRigidbody2D*" or "const EngineBoxCollider2D*"
            or "const EngineCircleCollider2D*"
            or "const EnginePolygonCollider2D*"
            or "EngineCollisionEvent2D*"
            or "EngineEditorSession*" or "const EngineEditorSession*"
            or "EngineEditorSnapshot*"
            or "EngineLocalTransform*"
            or "const EcsChunkView*" or "const FrameDispatch*"
            or "ManagedSystemCallback" or "EngineRenderPassCallback"
            or "const void*" or "const uint64_t*" or "const uint32_t*"
            or "const EngineChunkColumns*" or "EngineSystemCallback"
            or "EngineRenderer*" or "const EngineInputState*" or "uint8_t"
            or "EngineNavWorld*" or "const EngineNavWorld*"
            or "const EngineNavBox*" or "const EngineNavBakeConfig*"
            or "EngineNavStats*" or "uint8_t*"
            or "int32_t*" or "char*"
            or "EngineLogSinkCallback"
            or "EngineAudioWorld*" or "const EngineAudioWorld*"
            or "EngineUi*" or "const EngineUi*";
        if (!ok)
        {
            throw new InvalidOperationException($"{header}:{line}: unsupported ABI type '{ctype}' (extend the generator's type map deliberately).");
        }
    }

    private static void AssertComponentLayout(StructDef local, StructDef world)
    {
        // EngineLocalTransform must be exactly 16 floats = 64 bytes.
        var floats = 0;
        foreach (var f in local.Fields)
        {
            if (!f.CType.EndsWith("float", StringComparison.Ordinal))
            {
                throw new InvalidOperationException($"EngineLocalTransform: field {f.Name} must be float, got {f.CType}");
            }

            floats += f.ArrayLen == 0 ? 1 : f.ArrayLen;
        }

        if (floats != 16)
        {
            throw new InvalidOperationException($"EngineLocalTransform: must total 16 floats (64B), got {floats}");
        }

        if (world.Fields.Count != 1 || world.Fields[0].CType != "float" || world.Fields[0].ArrayLen != 16)
        {
            throw new InvalidOperationException("EngineWorldTransform: must be exactly 'float m[16]'");
        }
    }

    private static void AssertFrameDispatch(StructDef dispatch, string header)
    {
        // FrameDispatch is exactly one float (dt) = 4 bytes. A hand-emitted
        // 4B mirror covers it; anything wider is a deliberate ABI decision.
        if (dispatch.Fields.Count != 1 || dispatch.Fields[0].CType != "float" || dispatch.Fields[0].ArrayLen != 0)
        {
            throw new InvalidOperationException($"{header}: FrameDispatch must be exactly 'float dt'");
        }
    }

    // C type -> C# type (return/param position; void stays void).
    private static string CsType(string ctype) => ctype switch
    {
        "void" => "void",
        "int32_t" => "int",
        "uint32_t" => "uint",
        "uint64_t" => "ulong",
        "float" => "float",
        "void*" => "void*",
        "const char*" => "byte*",
        "const char**" => "byte**",
        "EngineWorld*" => "EngineWorld*",
        "const EngineWorld*" => "EngineWorld*",
        "EngineRenderPassRegistry*" => "EngineRenderPassRegistry*",
        "const EngineRenderPassRegistry*" => "EngineRenderPassRegistry*",
        "const EcsChunkView*" => "EcsChunkView*",
        "const FrameDispatch*" => "FrameDispatch*",
        "EnginePhysicsWorld*" => "EnginePhysicsWorld*",
        "const EnginePhysicsWorld*" => "EnginePhysicsWorld*",
        "EnginePhysics2DWorld*" => "EnginePhysics2DWorld*",
        "const EnginePhysics2DWorld*" => "EnginePhysics2DWorld*",
        "const EngineRigidbody2D*" => "Rigidbody2D*",
        "const EngineBoxCollider2D*" => "BoxCollider2D*",
        "const EngineCircleCollider2D*" => "CircleCollider2D*",
        "const EnginePolygonCollider2D*" => "PolygonCollider2D*",
        "EngineCollisionEvent2D*" => "CollisionEvent2D*",
        "const EngineRay*" => "Ray*",
        "EngineHit*" => "RaycastHit*",
        "uint32_t*" => "uint*",
        "uint64_t*" => "ulong*",
        "int32_t*" => "int*",
        "char*" => "byte*",
        "ManagedSystemCallback" => "delegate* unmanaged[Cdecl]<EcsChunkView*, float, void>",
        "EngineRenderPassCallback" => "delegate* unmanaged[Cdecl]<uint, void*, int>",
        "const float*" => "float*",
        "float*" => "float*",
        "EngineEditorSession*" => "EngineEditorSession*",
        "const EngineEditorSession*" => "EngineEditorSession*",
        "EngineEditorSnapshot*" => "EngineEditorSnapshot*",
        "EngineLocalTransform*" => "LocalTransform*",
        "const void*" => "void*",
        "const uint64_t*" => "ulong*",
        "const uint32_t*" => "uint*",
        "const EngineChunkColumns*" => "EngineChunkColumns*",
        "EngineSystemCallback" => "delegate* unmanaged[Cdecl]<EngineChunkColumns*, float, void>",
        "EngineLogSinkCallback" => "delegate* unmanaged[Cdecl]<int, byte*, void*, void>",
        "EngineRenderer*" => "EngineRenderer*",
        "const EngineInputState*" => "InputState*",
        "EngineNavWorld*" => "EngineNavWorld*",
        "const EngineNavWorld*" => "EngineNavWorld*",
        "const EngineNavBox*" => "NavBox*",
        "const EngineNavBakeConfig*" => "NavBakeConfig*",
        "EngineNavStats*" => "NavStats*",
        "uint8_t*" => "byte*",
        "EngineAudioWorld*" => "EngineAudioWorld*",
        "const EngineAudioWorld*" => "EngineAudioWorld*",
        "EngineUi*" => "EngineUi*",
        "const EngineUi*" => "EngineUi*",
        _ => throw new InvalidOperationException($"no C# mapping for '{ctype}'"),
    };

    private static string Pascal(string snake)
    {
        var parts = snake.Split('_', StringSplitOptions.RemoveEmptyEntries);
        var sb = new StringBuilder();
        foreach (var p in parts)
        {
            sb.Append(char.ToUpperInvariant(p[0]));
            if (p.Length > 1)
            {
                sb.Append(p[1..]);
            }
        }

        return sb.ToString();
    }

    private static string Emit(string header, AbiVersion v, int stride, List<StructDef> structs, StructDef chunk, List<FnDef> fns, string[] lines, Dictionary<string, int> generic)
    {
        var sb = new StringBuilder();
        sb.AppendLine("// <auto-generated>");
        sb.AppendLine("// NativeApi.g.cs — GENERATED by tools/abi_codegen from");
        sb.AppendLine("// native/bridge/include/engine/bridge/engine_abi.h. DO NOT EDIT.");
        sb.AppendLine(System.Globalization.CultureInfo.InvariantCulture, $"// ABI {v.Major}.{v.Minor}.{v.Patch}, column stride {stride}.");
        sb.AppendLine("// Regenerate: dotnet run --project tools/abi_codegen -- <repo-root>");
        sb.AppendLine("// </auto-generated>");
        sb.AppendLine("#nullable enable");
        sb.AppendLine("using System.Runtime.CompilerServices;");
        sb.AppendLine("using System.Runtime.InteropServices;");
        sb.AppendLine();
        sb.AppendLine("namespace Engine.Bindings;");
        sb.AppendLine();
        sb.AppendLine("/// <summary>");
        sb.AppendLine("/// Managed mirror of the C99 ABI (engine_abi.h). Function");
        sb.AppendLine("/// pointers use selective GC-transition suppression: only");
        sb.AppendLine("/// <c>register_system</c> (a guaranteed non-blocking store) is");
        sb.AppendLine("/// <c>delegate* unmanaged[Cdecl, SuppressGCTransition]</c>. Run,");
        sb.AppendLine("/// propagate, and lifecycle calls use plain <c>Cdecl</c> because");
        sb.AppendLine("/// they may block on worker completion, and suppressing the GC");
        sb.AppendLine("/// transition across a blocking native call can stall the runtime.");
        sb.AppendLine("/// Each entry JIT-compiles to a single raw <c>call</c> instruction");
        sb.AppendLine("/// with no P/Invoke stub and no marshalling.");
        sb.AppendLine("/// Blittable throughout: usable inside [UnmanagedCallersOnly] callbacks.");
        sb.AppendLine("/// </summary>");
        sb.AppendLine("public static unsafe class NativeApi");
        sb.AppendLine("{");
        sb.AppendLine(System.Globalization.CultureInfo.InvariantCulture, $"    public const int AbiVersionMajor = {v.Major};");
        sb.AppendLine(System.Globalization.CultureInfo.InvariantCulture, $"    public const int AbiVersionMinor = {v.Minor};");
        sb.AppendLine(System.Globalization.CultureInfo.InvariantCulture, $"    public const int AbiVersionPatch = {v.Patch};");
        sb.AppendLine(System.Globalization.CultureInfo.InvariantCulture, $"    public const int ColumnStride = {stride};");
        sb.AppendLine("    public const string NativeLibraryName = \"engine_core\";");
        foreach (var (key, csName) in GenericConstNames)
        {
            sb.AppendLine(System.Globalization.CultureInfo.InvariantCulture, $"    public const int {csName} = {generic[key]};");
        }

        sb.AppendLine();
        var table = structs.Single(s => s.Name == "EngineApiTable");
        EmitOpaqueHandles(sb);
        EmitPhysicsTypes(sb, structs);
        EmitNavTypes(sb, structs);
        EmitLocalTransform(sb, structs.Single(s => s.Name == "EngineLocalTransform"));
        EmitWorldTransform(sb, structs.Single(s => s.Name == "EngineWorldTransform"));
        EmitChunkView(sb, chunk);
        EmitChunkColumns(sb, structs.Single(s => s.Name == "EngineChunkColumns"));
        EmitInputState(sb, structs.Single(s => s.Name == "EngineInputState"));
        EmitCallback(sb, fns, header, lines);
        EmitApiTable(sb, table, fns);
        EmitLoader(sb);
        sb.AppendLine("}");
        return sb.ToString();
    }

    private static void EmitLocalTransform(StringBuilder sb, StructDef local)
    {
        sb.AppendLine("    /// <summary>Mirror of EngineLocalTransform: 16 floats, 64 bytes.</summary>");
        sb.AppendLine("    [StructLayout(LayoutKind.Sequential, Pack = 4, Size = 64)]");
        sb.AppendLine("    public struct LocalTransform");
        sb.AppendLine("    {");
        EmitComponentFields(sb, local, "LocalTransform");
        sb.AppendLine("    }");
        sb.AppendLine();
    }

    private static void EmitWorldTransform(StringBuilder sb, StructDef world)
    {
        sb.AppendLine("    /// <summary>Mirror of EngineWorldTransform: row-major 4x4, 64 bytes.</summary>");
        sb.AppendLine("    [StructLayout(LayoutKind.Sequential, Pack = 4, Size = 64)]");
        sb.AppendLine("    public unsafe struct WorldTransform");
        sb.AppendLine("    {");
        EmitComponentFields(sb, world, "WorldTransform");
        sb.AppendLine("    }");
        sb.AppendLine();
    }

    private static void EmitComponentFields(StringBuilder sb, StructDef def, string owner)
    {
        // One declaration per field, header order preserved. Single-field
        // lines keep future header diffs one-to-one with binding diffs.
        foreach (var f in def.Fields)
        {
            if (f.CType == "float" && f.ArrayLen == 0)
            {
                sb.AppendLine(System.Globalization.CultureInfo.InvariantCulture, $"        public float {Pascal(f.Name)};");
                continue;
            }

            if (f.CType == "float" && f.ArrayLen > 0)
            {
                // Field-name rule: keep the header name when already public
                // (WorldTransform.m -> M); single-letter buffers read wrong in
                // lowercase. Everything else is private padding (_pad, _m).
                var csName = f.Name switch
                {
                    "m" => "M",
                    _ => f.Name.StartsWith('_') ? f.Name : "_" + f.Name.ToLowerInvariant(),
                };
                var vis = csName == "M" ? "public" : "private";
                sb.AppendLine(System.Globalization.CultureInfo.InvariantCulture, $"        {vis} fixed float {csName}[{f.ArrayLen}];");
                continue;
            }

            throw new InvalidOperationException($"{owner}.{f.Name}: unsupported component field type '{f.CType}' (components are float scalars/arrays only — extend deliberately).");
        }
    }

    private static void EmitChunkView(StringBuilder sb, StructDef chunk)
    {
        sb.AppendLine("    /// <summary>");
        sb.AppendLine("    /// Mirror of EcsChunkView. The pointers alias native Flecs table");
        sb.AppendLine("    /// columns owned by the native world; valid only for the callback.");
        sb.AppendLine("    /// </summary>");
        sb.AppendLine("    [StructLayout(LayoutKind.Sequential, Pack = 8)]");
        sb.AppendLine("    public unsafe struct EcsChunkView");
        sb.AppendLine("    {");
        foreach (var f in chunk.Fields)
        {
            var decl = f.Name switch
            {
                "local" => "public LocalTransform* Local;",
                "world" => "public WorldTransform* World;",
                "entities" => "public ulong* Entities;",
                "count" => "public int Count;",
                "stride_local" => "public int StrideLocal;",
                "stride_world" => "public int StrideWorld;",
                "user_data" => "public void* UserData;",
                _ => throw new InvalidOperationException($"EcsChunkView: unknown field {f.Name} (update the emitter)."),
            };
            sb.AppendLine(System.Globalization.CultureInfo.InvariantCulture, $"        {decl}");
        }

        sb.AppendLine("    }");
        sb.AppendLine();
    }

    private static void EmitChunkColumns(StringBuilder sb, StructDef cols)
    {
        // Field list is ABI: fail closed on any drift from the 120-byte shape.
        string[] wantNames = ["columns", "sizes", "entities", "count", "term_count", "user_data"];
        string[] wantTypes = ["void*", "int32_t", "const uint64_t*", "int32_t", "int32_t", "void*"];
        int[] wantArr = [8, 8, 0, 0, 0, 0];
        if (cols.Fields.Count != wantNames.Length)
        {
            throw new InvalidOperationException($"EngineChunkColumns must have {wantNames.Length} fields (got {cols.Fields.Count}).");
        }

        for (var i = 0; i < wantNames.Length; i++)
        {
            var f = cols.Fields[i];
            if (f.Name != wantNames[i] || f.CType != wantTypes[i] || f.ArrayLen != wantArr[i])
            {
                throw new InvalidOperationException($"EngineChunkColumns field {i} must be '{wantTypes[i]} {wantNames[i]}{(wantArr[i] > 0 ? $"[{wantArr[i]}]" : string.Empty)}', got '{f.CType} {f.Name}{(f.ArrayLen > 0 ? $"[{f.ArrayLen}]" : string.Empty)}'.");
            }
        }

        sb.AppendLine("    /// <summary>");
        sb.AppendLine("    /// Mirror of EngineChunkColumns (ABI 4.2): one matched table slice for a");
        sb.AppendLine("    /// generic system. Columns[i] is the base address of term i's column");
        sb.AppendLine("    /// (cast to the term's component pointer); valid only for the callback.");
        sb.AppendLine("    /// </summary>");
        sb.AppendLine("    [StructLayout(LayoutKind.Sequential, Pack = 8, Size = 120)]");
        sb.AppendLine("    public unsafe struct EngineChunkColumns");
        sb.AppendLine("    {");
        sb.AppendLine("        public fixed ulong Columns[8];");
        sb.AppendLine("        public fixed int Sizes[8];");
        sb.AppendLine("        public ulong* Entities;");
        sb.AppendLine("        public int Count;");
        sb.AppendLine("        public int TermCount;");
        sb.AppendLine("        public void* UserData;");
        sb.AppendLine("    }");
        sb.AppendLine();
    }

    // P2-02: EngineInputState mirror. Field list is ABI: fail closed on any
    // drift from the 1668-byte shape (3x512 scancode tables + mouse floats +
    // button mask + 4x6 pad axes + 4 pad button masks).
    private static void EmitInputState(StringBuilder sb, StructDef state)
    {
        string[] wantNames = ["keys", "pressed", "released", "mouse_x", "mouse_y", "mouse_dx", "mouse_dy", "mouse_buttons", "pad_axes", "pad_buttons"];
        string[] wantTypes = ["uint8_t", "uint8_t", "uint8_t", "float", "float", "float", "float", "uint32_t", "float", "uint32_t"];
        int[] wantArr = [512, 512, 512, 0, 0, 0, 0, 0, 4, 4];
        int[] wantArr2 = [0, 0, 0, 0, 0, 0, 0, 0, 6, 0];
        if (state.Fields.Count != wantNames.Length)
        {
            throw new InvalidOperationException($"EngineInputState must have {wantNames.Length} fields (got {state.Fields.Count}).");
        }

        for (var i = 0; i < wantNames.Length; i++)
        {
            var f = state.Fields[i];
            if (f.Name != wantNames[i] || f.CType != wantTypes[i] || f.ArrayLen != wantArr[i] || f.ArrayLen2 != wantArr2[i])
            {
                throw new InvalidOperationException($"EngineInputState field {i} must be '{wantTypes[i]} {wantNames[i]}" + (wantArr[i] > 0 ? $"[{wantArr[i]}]" : string.Empty) + (wantArr2[i] > 0 ? $"[{wantArr2[i]}]" : string.Empty) + $"', got '{f.CType} {f.Name}" + (f.ArrayLen > 0 ? $"[{f.ArrayLen}]" : string.Empty) + (f.ArrayLen2 > 0 ? $"[{f.ArrayLen2}]" : string.Empty) + "'.");
            }
        }

        sb.AppendLine("    /// <summary>");
        sb.AppendLine("    /// Mirror of EngineInputState (ABI 4.3): event-fed input snapshot, 1668 bytes.");
        sb.AppendLine("    /// Keys/pressed/released index by SDL scancode (512 slots); pad_axes is");
        sb.AppendLine("    /// 4 pads x 6 axes normalised to [-1, 1]. Native owns the memory;");
        sb.AppendLine("    /// managed views it zero-copy via input_get_state.");
        sb.AppendLine("    /// </summary>");
        sb.AppendLine("    [StructLayout(LayoutKind.Sequential, Pack = 4, Size = 1668)]");
        sb.AppendLine("    public unsafe struct InputState");
        sb.AppendLine("    {");
        sb.AppendLine("        public fixed byte Keys[512];");
        sb.AppendLine("        public fixed byte Pressed[512];");
        sb.AppendLine("        public fixed byte Released[512];");
        sb.AppendLine("        public float MouseX;");
        sb.AppendLine("        public float MouseY;");
        sb.AppendLine("        public float MouseDx;");
        sb.AppendLine("        public float MouseDy;");
        sb.AppendLine("        public uint MouseButtons;");
        sb.AppendLine("        public fixed float PadAxes[4 * 6];");
        sb.AppendLine("        public fixed uint PadButtons[4];");
        sb.AppendLine("    }");
        sb.AppendLine();
    }

    private static void EmitCallback(StringBuilder sb, List<FnDef> fns, string header, string[] lines)
    {
        var cb = ParseManagedCallback(lines, header);
        var ps = string.Join(", ", cb.Params.Select(p => CsType(p.CType)));
        sb.AppendLine("    /// <summary>");
        sb.AppendLine("    /// Managed per-chunk system signature (ABI 2: view + frame dt by value).");
        sb.AppendLine("    /// Implement as a static method marked");
        sb.AppendLine("    /// [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])];");
        sb.AppendLine("    /// the body must not allocate, must not throw, and is thread-safe");
        sb.AppendLine("    /// (runs on native worker threads).");
        sb.AppendLine("    /// </summary>");
        sb.AppendLine("    public unsafe struct ManagedSystem");
        sb.AppendLine("    {");
        sb.AppendLine(System.Globalization.CultureInfo.InvariantCulture, $"        public delegate* unmanaged[Cdecl]<{ps}, void> Fn;");
        sb.AppendLine("    }");
        sb.AppendLine();
        sb.AppendLine("    /// <summary>Mirror of FrameDispatch: per-dispatch parameters (frame dt).</summary>");
        sb.AppendLine("    [StructLayout(LayoutKind.Sequential, Pack = 4, Size = 4)]");
        sb.AppendLine("    public struct FrameDispatch");
        sb.AppendLine("    {");
        sb.AppendLine("        public float Dt;");
        sb.AppendLine("    }");
        sb.AppendLine();
        _ = fns;
    }

    // ManagedSystemCallback is skipped by ParseFns (C# declares it by hand),
    // but its shape is ABI: verify the header typedef matches exactly
    // (const EcsChunkView*, float dt) so a silent header drift fails closed.
    private static FnDef ParseManagedCallback(string[] lines, string header)
    {
        for (var i = 0; i < lines.Length; i++)
        {
            var t = lines[i].Trim();
            if (!t.StartsWith("typedef", StringComparison.Ordinal) || !t.Contains("ManagedSystemCallback", StringComparison.Ordinal))
            {
                continue;
            }

            var fb = new StringBuilder(StripComment(t));
            while (i + 1 < lines.Length && !FnTypedefRe.IsMatch(fb.ToString()))
            {
                fb.Append(' ').Append(StripComment(lines[++i].Trim()));
                if (fb.Length > 2000)
                {
                    break;
                }
            }

            var m = FnTypedefRe.Match(fb.ToString());
            if (!m.Success || m.Groups[2].Value != "ManagedSystemCallback")
            {
                continue;
            }

            var ps = new List<FnParam>();
            foreach (var raw in m.Groups[3].Value.Split(','))
            {
                var p = raw.Trim();
                var pm = Regex.Match(p, @"^(.*\S)\s+(\w+)$");
                if (!pm.Success)
                {
                    throw new InvalidOperationException($"{header}:{i + 1}: cannot parse ManagedSystemCallback param: {p}");
                }

                ps.Add(new FnParam(NormType(pm.Groups[1].Value), pm.Groups[2].Value));
            }

            var want = new[] { "const EcsChunkView*", "float" };
            if (ps.Count != want.Length)
            {
                throw new InvalidOperationException($"{header}:{i + 1}: ManagedSystemCallback must take (const EcsChunkView*, float), got {ps.Count} params");
            }

            for (var k = 0; k < want.Length; k++)
            {
                if (ps[k].CType != want[k])
                {
                    throw new InvalidOperationException($"{header}:{i + 1}: ManagedSystemCallback param {k} must be '{want[k]}', got '{ps[k].CType}'");
                }
            }

            // The emitter hardcodes a void return in the delegate* shape, so
            // a non-void header typedef must fail closed here, not emit a
            // silently mismatched function pointer.
            var ret = NormType(m.Groups[1].Value);
            if (ret != "void")
            {
                throw new InvalidOperationException($"{header}:{i + 1}: ManagedSystemCallback must return void, got '{ret}'");
            }

            return new FnDef("ManagedSystemCallback", ret, ps);
        }

        throw new InvalidOperationException($"{header}: missing ManagedSystemCallback typedef");
    }

    private static void EmitPhysicsTypes(StringBuilder sb, List<StructDef> structs)
    {
        // EngineRay (24B) and EngineHit (48B) mirror Engine.Runtime.Physics
        // field order exactly; the loader pins sizes at startup.
        var ray = structs.Single(s => s.Name == "EngineRay");
        var hit = structs.Single(s => s.Name == "EngineHit");
        if (ray.Fields.Count != 6 || ray.Fields.Any(f => f.CType != "float"))
        {
            throw new InvalidOperationException("EngineRay must be exactly 6 floats (ox,oy,oz,dx,dy,dz).");
        }

        string[] wantNames = ["px", "py", "pz", "fraction", "user_data", "nx", "ny", "nz", "body_id", "hit"];
        string[] wantTypes = ["float", "float", "float", "float", "uint64_t", "float", "float", "float", "uint32_t", "uint32_t"];
        if (hit.Fields.Count != wantNames.Length)
        {
            throw new InvalidOperationException($"EngineHit must have {wantNames.Length} fields (got {hit.Fields.Count}).");
        }

        for (var i = 0; i < wantNames.Length; i++)
        {
            if (hit.Fields[i].Name != wantNames[i] || hit.Fields[i].CType != wantTypes[i])
            {
                throw new InvalidOperationException($"EngineHit field {i} must be '{wantTypes[i]} {wantNames[i]}', got '{hit.Fields[i].CType} {hit.Fields[i].Name}'.");
            }
        }

        sb.AppendLine("    /// <summary>Opaque native physics world handle (caller-owned engine::physics::PhysicsWorld, not the bare JPH::PhysicsSystem).</summary>");
        sb.AppendLine("    [StructLayout(LayoutKind.Sequential, Pack = 8)]");
        sb.AppendLine("    public unsafe struct EnginePhysicsWorld");
        sb.AppendLine("    {");
        sb.AppendLine("        public void* Handle;");
        sb.AppendLine("    }");
        sb.AppendLine();
        EmitPhysics2DTypes(sb, structs);
        sb.AppendLine("    /// <summary>Mirror of EngineRay: origin + direction*length, 24 bytes.</summary>");
        sb.AppendLine("    [StructLayout(LayoutKind.Sequential, Pack = 4, Size = 24)]");
        sb.AppendLine("    public struct Ray");
        sb.AppendLine("    {");
        sb.AppendLine("        public float Ox;");
        sb.AppendLine("        public float Oy;");
        sb.AppendLine("        public float Oz;");
        sb.AppendLine("        public float Dx;");
        sb.AppendLine("        public float Dy;");
        sb.AppendLine("        public float Dz;");
        sb.AppendLine("    }");
        sb.AppendLine();
        sb.AppendLine("    /// <summary>Mirror of EngineHit: query hit, 48 bytes.</summary>");
        sb.AppendLine("    [StructLayout(LayoutKind.Sequential, Pack = 8, Size = 48)]");
        sb.AppendLine("    public struct RaycastHit");
        sb.AppendLine("    {");
        sb.AppendLine("        public float Px;");
        sb.AppendLine("        public float Py;");
        sb.AppendLine("        public float Pz;");
        sb.AppendLine("        public float Fraction;");
        sb.AppendLine("        public ulong UserData;");
        sb.AppendLine("        public float Nx;");
        sb.AppendLine("        public float Ny;");
        sb.AppendLine("        public float Nz;");
        sb.AppendLine("        public uint BodyId;");
        sb.AppendLine("        public uint Hit;");
        sb.AppendLine("    }");
        sb.AppendLine();
    }

    private static void EmitNavTypes(StringBuilder sb, List<StructDef> structs)
    {
        // Navigation mirrors (P6-03): EngineNavBox (24B), EngineNavBakeConfig
        // (48B), EngineNavStats (8B), EngineNavAgent (64B). Field order must
        // match engine_abi.h exactly; the loader pins sizes at startup.
        var box = structs.Single(s => s.Name == "EngineNavBox");
        if (box.Fields.Count != 6 || box.Fields.Any(f => f.CType != "float"))
        {
            throw new InvalidOperationException("EngineNavBox must be exactly 6 floats (cx,cy,cz,hx,hy,hz).");
        }
        var cfg = structs.Single(s => s.Name == "EngineNavBakeConfig");
        if (cfg.Fields.Count != 12 || cfg.Fields.Any(f => f.CType != "float"))
        {
            throw new InvalidOperationException("EngineNavBakeConfig must be exactly 12 floats.");
        }
        var stats = structs.Single(s => s.Name == "EngineNavStats");
        string[] wantStatsNames = ["polys", "detail_tris"];
        if (stats.Fields.Count != wantStatsNames.Length)
        {
            throw new InvalidOperationException($"EngineNavStats must have {wantStatsNames.Length} fields (got {stats.Fields.Count}).");
        }
        for (var i = 0; i < wantStatsNames.Length; i++)
        {
            if (stats.Fields[i].Name != wantStatsNames[i] || stats.Fields[i].CType != "uint32_t")
            {
                throw new InvalidOperationException($"EngineNavStats field {i} must be 'uint32_t {wantStatsNames[i]}', got '{stats.Fields[i].CType} {stats.Fields[i].Name}'.");
            }
        }
        var agent = structs.Single(s => s.Name == "EngineNavAgent");
        string[] wantAgentNames = ["gx", "gy", "gz", "max_speed", "stop_radius", "arrived", "idle", "pad"];
        string[] wantAgentTypes = ["float", "float", "float", "float", "float", "int32_t", "int32_t", "float"];
        int[] wantAgentArr = [0, 0, 0, 0, 0, 0, 0, 9];
        if (agent.Fields.Count != wantAgentNames.Length)
        {
            throw new InvalidOperationException($"EngineNavAgent must have {wantAgentNames.Length} fields (got {agent.Fields.Count}).");
        }
        for (var i = 0; i < wantAgentNames.Length; i++)
        {
            if (agent.Fields[i].Name != wantAgentNames[i] || agent.Fields[i].CType != wantAgentTypes[i] || agent.Fields[i].ArrayLen != wantAgentArr[i])
            {
                throw new InvalidOperationException($"EngineNavAgent field {i} must be '{wantAgentTypes[i]} {wantAgentNames[i]}', got '{agent.Fields[i].CType} {agent.Fields[i].Name}'.");
            }
        }
        sb.AppendLine("    /// <summary>Mirror of EngineNavBox: one static box obstacle, 24 bytes.</summary>");
        sb.AppendLine("    [StructLayout(LayoutKind.Sequential, Pack = 4, Size = 24)]");
        sb.AppendLine("    public struct NavBox");
        sb.AppendLine("    {");
        sb.AppendLine("        public float Cx;");
        sb.AppendLine("        public float Cy;");
        sb.AppendLine("        public float Cz;");
        sb.AppendLine("        public float Hx;");
        sb.AppendLine("        public float Hy;");
        sb.AppendLine("        public float Hz;");
        sb.AppendLine("    }");
        sb.AppendLine();
        sb.AppendLine("    /// <summary>Mirror of EngineNavBakeConfig: floor + raster params, 48 bytes.</summary>");
        sb.AppendLine("    [StructLayout(LayoutKind.Sequential, Pack = 4, Size = 48)]");
        sb.AppendLine("    public struct NavBakeConfig");
        sb.AppendLine("    {");
        sb.AppendLine("        public float FloorMinX;");
        sb.AppendLine("        public float FloorMinY;");
        sb.AppendLine("        public float FloorMinZ;");
        sb.AppendLine("        public float FloorMaxX;");
        sb.AppendLine("        public float FloorMaxY;");
        sb.AppendLine("        public float FloorMaxZ;");
        sb.AppendLine("        public float CellSize;");
        sb.AppendLine("        public float CellHeight;");
        sb.AppendLine("        public float AgentHeight;");
        sb.AppendLine("        public float AgentRadius;");
        sb.AppendLine("        public float AgentMaxClimb;");
        sb.AppendLine("        public float AgentMaxSlopeDeg;");
        sb.AppendLine("    }");
        sb.AppendLine();
        sb.AppendLine("    /// <summary>Mirror of EngineNavStats: bake poly counts, 8 bytes.</summary>");
        sb.AppendLine("    [StructLayout(LayoutKind.Sequential, Pack = 4, Size = 8)]");
        sb.AppendLine("    public struct NavStats");
        sb.AppendLine("    {");
        sb.AppendLine("        public uint Polys;");
        sb.AppendLine("        public uint DetailTris;");
        sb.AppendLine("    }");
        sb.AppendLine();
        sb.AppendLine("    /// <summary>Mirror of EngineNavAgent: goal + speed + arrival flag, 64 bytes.</summary>");
        sb.AppendLine("    [StructLayout(LayoutKind.Sequential, Pack = 4, Size = 64)]");
        sb.AppendLine("    public struct NavAgentData");
        sb.AppendLine("    {");
        sb.AppendLine("        public float Gx;");
        sb.AppendLine("        public float Gy;");
        sb.AppendLine("        public float Gz;");
        sb.AppendLine("        public float MaxSpeed;");
        sb.AppendLine("        public float StopRadius;");
        sb.AppendLine("        public int Arrived;");
        sb.AppendLine("        public int Idle;");
        sb.AppendLine("        private fixed float _pad[9];");
        sb.AppendLine("    }");
        sb.AppendLine();
    }

    private static void EmitPhysics2DTypes(StringBuilder sb, List<StructDef> structs)
    {
        // P6-07 2D physics mirrors (ABI 4.9): validated field-by-field so a
        // header drift fails closed here, not as a silent C# layout skew.
        var ev = structs.Single(s => s.Name == "EngineCollisionEvent2D");
        string[] evNames = ["entity_a", "entity_b", "point_x", "point_y", "point_z", "kind", "sensor", "reserved"];
        string[] evTypes = ["uint64_t", "uint64_t", "float", "float", "float", "uint8_t", "uint8_t", "uint8_t"];
        int[] evArr = [0, 0, 0, 0, 0, 0, 0, 2];
        if (ev.Fields.Count != evNames.Length)
        {
            throw new InvalidOperationException($"EngineCollisionEvent2D must have {evNames.Length} fields (got {ev.Fields.Count}).");
        }

        for (var i = 0; i < evNames.Length; i++)
        {
            if (ev.Fields[i].Name != evNames[i] || ev.Fields[i].CType != evTypes[i] || ev.Fields[i].ArrayLen != evArr[i])
            {
                throw new InvalidOperationException($"EngineCollisionEvent2D field {i} must be '{evTypes[i]} {evNames[i]}', got '{ev.Fields[i].CType} {ev.Fields[i].Name}'.");
            }
        }

        sb.AppendLine("    /// <summary>Opaque native 2D physics world handle (caller-owned engine::physics::World2D).</summary>");
        sb.AppendLine("    [StructLayout(LayoutKind.Sequential, Pack = 8)]");
        sb.AppendLine("    public unsafe struct EnginePhysics2DWorld");
        sb.AppendLine("    {");
        sb.AppendLine("        public void* Handle;");
        sb.AppendLine("    }");
        sb.AppendLine();
        sb.AppendLine("    /// <summary>Mirror of EngineCollisionEvent2D: same 32-byte record as the 3D path, sensor + kind packed as bytes.</summary>");
        sb.AppendLine("    [StructLayout(LayoutKind.Sequential, Pack = 8, Size = 32)]");
        sb.AppendLine("    public struct CollisionEvent2D");
        sb.AppendLine("    {");
        sb.AppendLine("        public ulong EntityA;");
        sb.AppendLine("        public ulong EntityB;");
        sb.AppendLine("        public float PointX;");
        sb.AppendLine("        public float PointY;");
        sb.AppendLine("        public float PointZ;");
        sb.AppendLine("        public byte Kind;");
        sb.AppendLine("        public byte Sensor;");
        sb.AppendLine("        private fixed byte _reserved[2];");
        sb.AppendLine("    }");
        sb.AppendLine();
        EmitRigidbody2D(sb, structs.Single(s => s.Name == "EngineRigidbody2D"));
        EmitBox2D(sb, structs.Single(s => s.Name == "EngineBoxCollider2D"));
        EmitCircle2D(sb, structs.Single(s => s.Name == "EngineCircleCollider2D"));
        EmitPolygon2D(sb, structs.Single(s => s.Name == "EnginePolygonCollider2D"));
    }

    private static void EmitRigidbody2D(StringBuilder sb, StructDef def)
    {
        // 24 bytes: 3 floats + 6 bytes flags + type + 2 reserved.
        string[] wantNames = ["gravity_scale", "linear_damping", "angular_damping", "fixed_rotation", "bullet", "allow_sleeping", "sensor", "active", "type", "reserved"];
        string[] wantTypes = ["float", "float", "float", "uint8_t", "uint8_t", "uint8_t", "uint8_t", "uint8_t", "uint8_t", "uint8_t"];
        int[] wantArr = [0, 0, 0, 0, 0, 0, 0, 0, 0, 4];
        AssertStructFields(def, "EngineRigidbody2D", wantNames, wantTypes, wantArr);
        sb.AppendLine("    /// <summary>Mirror of EngineRigidbody2D: 24 bytes.</summary>");
        sb.AppendLine("    [StructLayout(LayoutKind.Sequential, Pack = 4, Size = 24)]");
        sb.AppendLine("    public struct Rigidbody2D");
        sb.AppendLine("    {");
        sb.AppendLine("        public float GravityScale;");
        sb.AppendLine("        public float LinearDamping;");
        sb.AppendLine("        public float AngularDamping;");
        sb.AppendLine("        public byte FixedRotation;");
        sb.AppendLine("        public byte Bullet;");
        sb.AppendLine("        public byte AllowSleeping;");
        sb.AppendLine("        public byte Sensor;");
        sb.AppendLine("        public byte Active;");
        sb.AppendLine("        public byte Type;");
        sb.AppendLine("        private fixed byte _reserved[4];");
        sb.AppendLine("    }");
        sb.AppendLine();
    }

    private static void EmitBox2D(StringBuilder sb, StructDef def)
    {
        // 24 bytes: 5 floats + sensor + 3 reserved.
        string[] wantNames = ["hx", "hy", "density", "friction", "restitution", "sensor", "reserved"];
        string[] wantTypes = ["float", "float", "float", "float", "float", "uint8_t", "uint8_t"];
        int[] wantArr = [0, 0, 0, 0, 0, 0, 3];
        AssertStructFields(def, "EngineBoxCollider2D", wantNames, wantTypes, wantArr);
        sb.AppendLine("    /// <summary>Mirror of EngineBoxCollider2D: 24 bytes.</summary>");
        sb.AppendLine("    [StructLayout(LayoutKind.Sequential, Pack = 4, Size = 24)]");
        sb.AppendLine("    public struct BoxCollider2D");
        sb.AppendLine("    {");
        sb.AppendLine("        public float Hx;");
        sb.AppendLine("        public float Hy;");
        sb.AppendLine("        public float Density;");
        sb.AppendLine("        public float Friction;");
        sb.AppendLine("        public float Restitution;");
        sb.AppendLine("        public byte Sensor;");
        sb.AppendLine("        private fixed byte _reserved[3];");
        sb.AppendLine("    }");
        sb.AppendLine();
    }

    private static void EmitCircle2D(StringBuilder sb, StructDef def)
    {
        // 28 bytes: 6 floats + sensor + 3 reserved.
        string[] wantNames = ["cx", "cy", "radius", "density", "friction", "restitution", "sensor", "reserved"];
        string[] wantTypes = ["float", "float", "float", "float", "float", "float", "uint8_t", "uint8_t"];
        int[] wantArr = [0, 0, 0, 0, 0, 0, 0, 3];
        AssertStructFields(def, "EngineCircleCollider2D", wantNames, wantTypes, wantArr);
        sb.AppendLine("    /// <summary>Mirror of EngineCircleCollider2D: 28 bytes.</summary>");
        sb.AppendLine("    [StructLayout(LayoutKind.Sequential, Pack = 4, Size = 28)]");
        sb.AppendLine("    public struct CircleCollider2D");
        sb.AppendLine("    {");
        sb.AppendLine("        public float Cx;");
        sb.AppendLine("        public float Cy;");
        sb.AppendLine("        public float Radius;");
        sb.AppendLine("        public float Density;");
        sb.AppendLine("        public float Friction;");
        sb.AppendLine("        public float Restitution;");
        sb.AppendLine("        public byte Sensor;");
        sb.AppendLine("        private fixed byte _reserved[3];");
        sb.AppendLine("    }");
        sb.AppendLine();
    }

    private static void EmitPolygon2D(StringBuilder sb, StructDef def)
    {
        // 64 bytes: vx[6] + vy[6] + density + friction + count + sensor + 2 reserved.
        string[] wantNames = ["vx", "vy", "density", "friction", "count", "sensor", "reserved"];
        string[] wantTypes = ["float", "float", "float", "float", "uint8_t", "uint8_t", "uint8_t"];
        int[] wantArr = [6, 6, 0, 0, 0, 0, 6];
        AssertStructFields(def, "EnginePolygonCollider2D", wantNames, wantTypes, wantArr);
        sb.AppendLine("    /// <summary>Mirror of EnginePolygonCollider2D: 64 bytes, up to 6 vertices.</summary>");
        sb.AppendLine("    [StructLayout(LayoutKind.Sequential, Pack = 4, Size = 64)]");
        sb.AppendLine("    public unsafe struct PolygonCollider2D");
        sb.AppendLine("    {");
        sb.AppendLine("        public fixed float Vx[6];");
        sb.AppendLine("        public fixed float Vy[6];");
        sb.AppendLine("        public float Density;");
        sb.AppendLine("        public float Friction;");
        sb.AppendLine("        public byte Count;");
        sb.AppendLine("        public byte Sensor;");
        sb.AppendLine("        private fixed byte _reserved[6];");
        sb.AppendLine("    }");
        sb.AppendLine();
    }

    private static void AssertStructFields(StructDef def, string name, string[] wantNames, string[] wantTypes, int[] wantArr)
    {
        if (def.Fields.Count != wantNames.Length)
        {
            throw new InvalidOperationException($"{name} must have {wantNames.Length} fields (got {def.Fields.Count}).");
        }

        for (var i = 0; i < wantNames.Length; i++)
        {
            if (def.Fields[i].Name != wantNames[i] || def.Fields[i].CType != wantTypes[i] || def.Fields[i].ArrayLen != wantArr[i])
            {
                throw new InvalidOperationException($"{name} field {i} must be '{wantTypes[i]} {wantNames[i]}', got '{def.Fields[i].CType} {def.Fields[i].Name}'.");
            }
        }
    }

    private static void EmitOpaqueHandles(StringBuilder sb)
    {
        sb.AppendLine("    /// <summary>Opaque native world handle (owns the Flecs world).</summary>");
        sb.AppendLine("    [StructLayout(LayoutKind.Sequential, Pack = 8)]");
        sb.AppendLine("    public unsafe struct EngineWorld");
        sb.AppendLine("    {");
        sb.AppendLine("        public void* Handle;");
        sb.AppendLine("    }");
        sb.AppendLine();
        sb.AppendLine("    /// <summary>Opaque game UI world (owns one RmlUi context + CPU frame, P5-04).</summary>");
        sb.AppendLine("    [StructLayout(LayoutKind.Sequential, Pack = 8)]");
        sb.AppendLine("    public unsafe struct EngineUi");
        sb.AppendLine("    {");
        sb.AppendLine("        public void* Handle;");
        sb.AppendLine("    }");
        sb.AppendLine();
        sb.AppendLine("    /// <summary>Opaque audio mixer world (owns one miniaudio engine + buses + effects, P5-06).</summary>");
        sb.AppendLine("    [StructLayout(LayoutKind.Sequential, Pack = 8)]");
        sb.AppendLine("    public unsafe struct EngineAudioWorld");
        sb.AppendLine("    {");
        sb.AppendLine("        public void* Handle;");
        sb.AppendLine("    }");
        sb.AppendLine();
        sb.AppendLine("    /// <summary>Opaque editor session (owns queries bound to one world).</summary>");
        sb.AppendLine("    [StructLayout(LayoutKind.Sequential, Pack = 8)]");
        sb.AppendLine("    public unsafe struct EngineEditorSession");
        sb.AppendLine("    {");
        sb.AppendLine("        public void* Handle;");
        sb.AppendLine("    }");
        sb.AppendLine();
        sb.AppendLine("    /// <summary>Opaque PIE snapshot (owns one scene checkpoint).</summary>");
        sb.AppendLine("    [StructLayout(LayoutKind.Sequential, Pack = 8)]");
        sb.AppendLine("    public unsafe struct EngineEditorSnapshot");
        sb.AppendLine("    {");
        sb.AppendLine("        public void* Handle;");
        sb.AppendLine("    }");
        sb.AppendLine();
        sb.AppendLine("    /// <summary>Opaque native renderer handle (P1-09 owns lifecycle; P2-02 only reads its input member).</summary>");
        sb.AppendLine("    [StructLayout(LayoutKind.Sequential, Pack = 8)]");
        sb.AppendLine("    public unsafe struct EngineRenderer");
        sb.AppendLine("    {");
        sb.AppendLine("        public void* Handle;");
        sb.AppendLine("    }");
        sb.AppendLine();
        sb.AppendLine("    /// <summary>Opaque navmesh world (owns one engine::navigation::NavWorld).</summary>");
        sb.AppendLine("    [StructLayout(LayoutKind.Sequential, Pack = 8)]");
        sb.AppendLine("    public unsafe struct EngineNavWorld");
        sb.AppendLine("    {");
        sb.AppendLine("        public void* Handle;");
        sb.AppendLine("    }");
        sb.AppendLine();
        sb.AppendLine("    /// <summary>Opaque render-pass registry (owns the native Compositor).</summary>");
        sb.AppendLine("    [StructLayout(LayoutKind.Sequential, Pack = 8)]");
        sb.AppendLine("    public unsafe struct EngineRenderPassRegistry");
        sb.AppendLine("    {");
        sb.AppendLine("        public void* Handle;");
        sb.AppendLine("    }");
        sb.AppendLine();
    }

    private static void EmitApiTable(StringBuilder sb, StructDef table, List<FnDef> fns)
    {
        sb.AppendLine("    /// <summary>Mirror of EngineApiTable. One raw call per entry.</summary>");
        sb.AppendLine("    [StructLayout(LayoutKind.Sequential, Pack = 8)]");
        sb.AppendLine("    public unsafe struct ApiTable");
        sb.AppendLine("    {");
        var used = new HashSet<string>(StringComparer.Ordinal);
        foreach (var f in table.Fields)
        {
            // Header (non-function) fields: explicit C-type map, fail closed.
            if (f.Name is "size" or "abi_version_major" or "abi_version_minor" or "abi_version_patch" or "column_stride")
            {
                sb.AppendLine(System.Globalization.CultureInfo.InvariantCulture, $"        {ApiScalarDecl(f)}");
                continue;
            }

            // Function fields: resolve the field's C typedef to its FnDef.
            var def = fns.FirstOrDefault(d => d.Name == f.CType);
            if (def == null)
            {
                throw new InvalidOperationException($"EngineApiTable.{f.Name}: unknown field type '{f.CType}' (no such Fn typedef — extend the header and generator together).");
            }

            used.Add(def.Name);
            var ps = new List<string>();
            foreach (var p in def.Params)
            {
                ps.Add(CsType(p.CType));
            }

            ps.Add(CsType(def.Return));
            // Selective suppression: only registration is a guaranteed
            // non-blocking store. Run/propagate/lifecycle calls may block on
            // worker completion — suppressing the GC transition across a
            // blocking native call can stall the runtime.
            var conv = def.Name == "EngineRegisterSystemFn" ? "Cdecl, SuppressGCTransition" : "Cdecl";
            sb.AppendLine(System.Globalization.CultureInfo.InvariantCulture, $"        public delegate* unmanaged[{conv}]<{string.Join(", ", ps)}> {TrimFn(def.Name)};");
        }

        var unused = fns.Where(d => !used.Contains(d.Name)).Select(d => d.Name).ToList();
        // Callback typedefs ride inside other entries' params (ManagedSystemCallback,
        // EngineRenderPassCallback, EngineLogSinkCallback) — used, just not as
        // table fields.
        unused.Remove("ManagedSystemCallback");
        unused.Remove("EngineRenderPassCallback");
        unused.Remove("EngineSystemCallback");
        unused.Remove("EngineLogSinkCallback");
        if (unused.Count != 0)
        {
            throw new InvalidOperationException($"Fn typedef(s) not used by EngineApiTable: {string.Join(", ", unused)} (remove the typedef or wire it into the table).");
        }

        sb.AppendLine("    }");
        sb.AppendLine();
    }

    private static string ApiScalarDecl(StructField f) => (f.Name, f.CType, f.ArrayLen) switch
    {
        ("size", "uint64_t", 0) => "public ulong Size;",
        ("abi_version_major", "int32_t", 0) => "public int AbiVersionMajor;",
        ("abi_version_minor", "int32_t", 0) => "public int AbiVersionMinor;",
        ("abi_version_patch", "int32_t", 0) => "public int AbiVersionPatch;",
        ("column_stride", "int32_t", 0) => "public int ColumnStride;",
        _ => throw new InvalidOperationException($"EngineApiTable.{f.Name}: unknown header field type '{f.CType}' (extend ApiScalarDecl deliberately)."),
    };

    private static string TrimFn(string name) => name.EndsWith("Fn", StringComparison.Ordinal) ? name[..^2] : name;

    private static void EmitLoader(StringBuilder sb)
    {
        sb.AppendLine("    /// <summary>Resolves engine_get_api_table and validates the ABI.</summary>");
        sb.AppendLine("    public static unsafe class ApiLoader");
        sb.AppendLine("    {");
        sb.AppendLine("        private static ApiTable* _table;");
        sb.AppendLine();
        sb.AppendLine("        public static ApiTable* Table => _table != null ? _table : throw new InvalidOperationException(\"NativeApi not loaded. Call ApiLoader.Load() first.\");");
        sb.AppendLine();
        sb.AppendLine("        public static ApiTable* Load()");
        sb.AppendLine("        {");
        sb.AppendLine("            if (_table != null)");
        sb.AppendLine("            {");
        sb.AppendLine("                return _table;");
        sb.AppendLine("            }");
        sb.AppendLine();
        sb.AppendLine("            if (!NativeLibrary.TryLoad(NativeLibraryName, out var handle))");
        sb.AppendLine("            {");
        sb.AppendLine("                throw new DllNotFoundException($\"Unable to load {NativeLibraryName}.\");");
        sb.AppendLine("            }");
        sb.AppendLine();
        sb.AppendLine("            if (!NativeLibrary.TryGetExport(handle, \"engine_get_api_table\", out var addr))");
        sb.AppendLine("            {");
        sb.AppendLine("                throw new EntryPointNotFoundException(\"engine_get_api_table not exported.\");");
        sb.AppendLine("            }");
        sb.AppendLine();
        sb.AppendLine("            var getTable = (delegate* unmanaged[Cdecl, SuppressGCTransition]<ApiTable*>)addr;");
        sb.AppendLine("            var table = getTable();");
        sb.AppendLine("            if (table == null)");
        sb.AppendLine("            {");
        sb.AppendLine("                throw new InvalidOperationException(\"engine_get_api_table returned null.\");");
        sb.AppendLine("            }");
        sb.AppendLine();
        sb.AppendLine("            if (table->Size < (ulong)sizeof(ApiTable))");
        sb.AppendLine("            {");
        sb.AppendLine("                throw new InvalidOperationException($\"Native ABI table too small: {table->Size} < {(ulong)sizeof(ApiTable)}.\");");
        sb.AppendLine("            }");
        sb.AppendLine();
        sb.AppendLine("            if (table->AbiVersionMajor != AbiVersionMajor || table->AbiVersionMinor != AbiVersionMinor || table->AbiVersionPatch != AbiVersionPatch)");
        sb.AppendLine("            {");
        sb.AppendLine("                throw new InvalidOperationException($\"ABI version mismatch: native {table->AbiVersionMajor}.{table->AbiVersionMinor}.{table->AbiVersionPatch} != managed {AbiVersionMajor}.{AbiVersionMinor}.{AbiVersionPatch}.\");");
        sb.AppendLine("            }");
        sb.AppendLine();
        sb.AppendLine("            if (table->ColumnStride != ColumnStride)");
        sb.AppendLine("            {");
        sb.AppendLine("                throw new InvalidOperationException($\"ABI column stride mismatch: native {table->ColumnStride} != managed {ColumnStride}.\");");
        sb.AppendLine("            }");
        sb.AppendLine();
        sb.AppendLine("            if (sizeof(LocalTransform) != 64 || sizeof(WorldTransform) != 64 || sizeof(Ray) != 24 || sizeof(RaycastHit) != 48 || sizeof(EngineChunkColumns) != 120 || sizeof(InputState) != 1668 || sizeof(NavBox) != 24 || sizeof(NavBakeConfig) != 48 || sizeof(NavStats) != 8 || sizeof(NavAgentData) != 64)");
        sb.AppendLine("            {");
        sb.AppendLine("                throw new InvalidOperationException($\"Component layout drift: LocalTransform={sizeof(LocalTransform)} WorldTransform={sizeof(WorldTransform)} Ray={sizeof(Ray)} Hit={sizeof(RaycastHit)} ChunkColumns={sizeof(EngineChunkColumns)} InputState={sizeof(InputState)} NavBox={sizeof(NavBox)} NavConfig={sizeof(NavBakeConfig)} NavStats={sizeof(NavStats)} NavAgent={sizeof(NavAgentData)}.\");");
        sb.AppendLine("            }");
        sb.AppendLine();
        sb.AppendLine("            _table = table;");
        sb.AppendLine("            return table;");
        sb.AppendLine("        }");
        sb.AppendLine("    }");
    }
}

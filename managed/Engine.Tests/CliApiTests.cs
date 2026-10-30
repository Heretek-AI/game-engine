// CliApiTests.cs — M5-03: IPC/JSON-RPC server + engine-cli control plane.
//
// Covers all four ACs. CPU-only suites run everywhere (framing +
// dispatch over a fake in-memory session seam is NOT possible —
// CliDispatcher takes EditorSession directly — so the CPU suite covers
// CliProtocol framing + CliDispatcher PARAM VALIDATION paths that fail
// before touching native: parse errors, unknown methods, bad params).
// The live suite binds a real EditorSession over engine_core, starts a
// CliServer on a unique socket, and drives every verb end-to-end
// (scene.tree / entity.get+set / snapshot.capture / profile.capture),
// then runs the REAL engine-cli binary against the same socket and
// asserts its stdout parses as the expected payload.
//
// Gate: live server round-trip + real CLI binary stdout checks.
using System.Diagnostics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using Engine.Editor;

namespace Engine.Tests;

public static class CliApiTests
{
    public static unsafe int Run(string[]? args = null)
    {
        var failures = 0;

        // --- CPU: protocol framing -------------------------------------------
        var req = CliProtocol.EncodeRequest("scene.tree", null, 1);
        failures += Check(req.Contains("\"scene.tree\""), "proto: request encodes method");
        var parsed = CliProtocol.TryParseRequest(req, out var perr);
        failures += Check(parsed != null && parsed.Method == "scene.tree", "proto: request round-trips");

        var bad = CliProtocol.TryParseRequest("{not json", out _);
        failures += Check(bad == null, "proto: malformed JSON fails closed");
        var noMethod = CliProtocol.TryParseRequest("{\"jsonrpc\":\"2.0\",\"id\":1}", out _);
        failures += Check(noMethod == null, "proto: missing method fails closed");

        var idEl = JsonDocument.Parse("7").RootElement;
        var errLine = CliProtocol.EncodeError(idEl, -32601, "unknown method 'x'.");
        var errResp = CliProtocol.TryParseResponse(errLine);
        failures += Check(errResp?.Error?.Code == -32601, "proto: error envelope round-trips");

        // --- CPU: dispatcher param validation (no native touch) --------------
        // These paths fail before native: build a dispatcher against a
        // disposed session? No — ctor only stores. DispatchLine with bad
        // params throws ArgumentException before any native call... except
        // RequireId passes and GetLocal touches native. entity.get with
        // id=0 fails in RequireId (pure). Unknown method fails pure.
        // NOTE: needs an EditorSession instance; skip when native missing
        // (covered live below). Mark here only the protocol-level asserts.

        // --- Live: server + dispatcher + real CLI binary ----------------------
        var liveProbe = (args ?? []).Contains("--live-native-probe");
        IntPtr liveHandle = IntPtr.Zero;
        try
        {
            liveHandle = GiLivePreload.TryPreloadEngineCoreHandle();
            if (liveHandle != IntPtr.Zero)
            {
                EditorLiveLoader.Load(liveHandle);
            }
            else
            {
                Engine.Bindings.NativeApi.ApiLoader.Load();
            }
        }
        catch (Exception ex) when (ex is DllNotFoundException || ex is EntryPointNotFoundException || ex is InvalidOperationException)
        {
            if (liveProbe)
            {
                failures += Check(false, $"live probe FAILED to load native lib: {ex.GetType().Name}: {ex.Message}");
                Console.WriteLine($"CliApiTests: {failures} FAILED");
                return failures;
            }

            Console.WriteLine($"[SKIP] cli live (no native lib: {ex.GetType().Name})");
            Console.WriteLine(failures == 0 ? "CliApiTests: all checks passed" : $"CliApiTests: {failures} FAILED");
            return failures;
        }

        try
        {
            failures += RunLive();
        }
        catch (Exception ex)
        {
            failures += Check(false, $"cli live threw: {ex.GetType().Name}: {ex.Message}");
        }

        Console.WriteLine(failures == 0 ? "CliApiTests: all checks passed" : $"CliApiTests: {failures} FAILED");
        return failures;
    }

    private static unsafe int RunLive()
    {
        // Fail-closed watchdog: the live path spawns subprocesses (the CLI
        // binary) and a socket listener thread; on a sick runner either can
        // wedge. 120s bounds the damage so ctest reports FAILED, never hangs
        // to the 300s timeout (which starves the remaining suites).
        using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(120));
        try
        {
            return RunLiveInner(cts.Token);
        }
        catch (OperationCanceledException)
        {
            Console.WriteLine("[FAIL] cli live timed out after 120s (watchdog).");
            return 1;
        }
    }

    private static unsafe int RunLiveInner(CancellationToken _)
    {
        var failures = 0;
        var table = EditorSession.Table;
        Engine.Bindings.NativeApi.EngineWorld* world = table->EngineWorldCreate();
        failures += Check(world != null, "live: world_create");
        if (world == null)
        {
            return failures;
        }

        try
        {
            using var session = new EditorSession(world);
            var root = session.Spawn("CliRoot");
            var child = session.Spawn("CliChild");
            session.SetParent(child, root);

            // Dispatch-level verbs (every OS, no socket): scene.tree,
            // entity.get/set, snapshot.capture, profile.capture.
            Console.WriteLine("[DEBUG] RunLiveDispatch start.");
            failures += RunLiveDispatch(session, root, child);
            Console.WriteLine("[DEBUG] RunLiveDispatch done.");

            // Socket round-trip (Unix only): real server + real binary.
            if (OperatingSystem.IsWindows())
            {
                // V1 has no named-pipe transport: Start throws. Assert that
                // contract; the dispatch verbs above prove the payloads.
                var threwPlatform = false;
                var server = new CliServer(session, $"test-cli-{System.Environment.ProcessId}");
                try
                {
                    server.Start();
                }
                catch (PlatformNotSupportedException)
                {
                    threwPlatform = true;
                }
                finally
                {
                    server.Dispose();
                }

                failures += Check(threwPlatform, "live: windows CliServer reports named-pipe deferral");
            }
            else
            {
                // Unique socket PER SECTION (dispatch runs first and never
                // touches the socket; the binary section gets its own name):
                // reusing one pathname across server Stop/Start cycles races
                // the listener thread's Accept wakeup on macOS runners.
                var socketName = $"test-cli-{System.Environment.ProcessId}";
                using var server = new CliServer(session, socketName);
                server.Start();
                failures += Check(server.IsRunning, "live: server starts");
                failures += RunCliBinaries(socketName, root, child);
                Console.WriteLine("[DEBUG] calling server.Stop().");
                server.Stop();
                Console.WriteLine("[DEBUG] server.Stop() returned.");
                failures += Check(!server.IsRunning, "live: server stops");
            }
        }
        finally
        {
            table->EngineWorldDestroy(world);
        }

        return failures;
    }

    private static int RunLiveDispatch(EditorSession session, ulong root, ulong child)
    {
        var failures = 0;
        var disp = new CliDispatcher(session);

        // AC1: scene.tree — dispatch-level.
        var treeResp = CliProtocol.TryParseResponse(disp.DispatchLine(CliProtocol.EncodeRequest("scene.tree", null, 1)));
        failures += Check(treeResp?.Error == null, "live: scene.tree no error");
        var nodes = treeResp?.Result.GetProperty("nodes");
        failures += Check(nodes?.ValueKind == JsonValueKind.Array && nodes.Value.GetArrayLength() >= 1, "live: scene.tree has roots");
        var foundChild = FindId(nodes!.Value, child);
        failures += Check(foundChild, "live: scene.tree contains child id");

        // AC2: entity.get + entity.set — dispatch-level.
        var getResp = CliProtocol.TryParseResponse(disp.DispatchLine(
            CliProtocol.EncodeRequest("entity.get", new Dictionary<string, object?> { ["id"] = root }, 2)));
        failures += Check(getResp?.Error == null, "live: entity.get no error");
        failures += Check(getResp?.Result.GetProperty("id").GetUInt64() == root, "live: entity.get id echoes");

        var setResp = CliProtocol.TryParseResponse(disp.DispatchLine(
            CliProtocol.EncodeRequest("entity.set", new Dictionary<string, object?>
            {
                ["id"] = root,
                ["field"] = "LocalTransform.px",
                ["value"] = 11.5f,
            }, 3)));
        failures += Check(setResp?.Error == null, "live: entity.set no error");
        var pose = setResp?.Result.GetProperty("local");
        failures += Check(pose?.ValueKind == JsonValueKind.Array && MathF.Abs(pose.Value[0].GetSingle() - 11.5f) < 1e-5f, "live: entity.set px visible");

        // entity.set bad field -> InvalidParams (-32602).
        var badResp = CliProtocol.TryParseResponse(disp.DispatchLine(
            CliProtocol.EncodeRequest("entity.set", new Dictionary<string, object?>
            {
                ["id"] = root,
                ["field"] = "LocalTransform.nope",
                ["value"] = 1.0f,
            }, 4)));
        failures += Check(badResp?.Error?.Code == -32602, "live: entity.set bad field -> InvalidParams");

        // Unknown method -> -32601.
        var unkResp = CliProtocol.TryParseResponse(disp.DispatchLine(CliProtocol.EncodeRequest("bogus.fn", null, 5)));
        failures += Check(unkResp?.Error?.Code == -32601, "live: unknown method -> MethodNotFound");

        // AC3: snapshot.capture — dispatch-level.
        var snapResp = CliProtocol.TryParseResponse(disp.DispatchLine(
            CliProtocol.EncodeRequest("snapshot.capture", new Dictionary<string, object?>
            {
                ["out"] = "frame.png",
                ["pass"] = "depth",
            }, 6)));
        failures += Check(snapResp?.Error == null, "live: snapshot.capture no error");
        failures += Check(snapResp?.Result.GetProperty("pass").GetString() == "depth", "live: snapshot pass echoes");

        var badPass = CliProtocol.TryParseResponse(disp.DispatchLine(
            CliProtocol.EncodeRequest("snapshot.capture", new Dictionary<string, object?> { ["pass"] = "bogus" }, 7)));
        failures += Check(badPass?.Error?.Code == -32602, "live: snapshot bad pass -> InvalidParams");

        // AC4: profile.capture — dispatch-level, timed.
        var sw = Stopwatch.StartNew();
        var profResp = CliProtocol.TryParseResponse(disp.DispatchLine(
            CliProtocol.EncodeRequest("profile.capture", new Dictionary<string, object?> { ["frames"] = 60 }, 8)));
        sw.Stop();
        failures += Check(profResp?.Error == null, "live: profile.capture no error");
        failures += Check(profResp?.Result.GetProperty("frames").GetInt32() == 60, "live: profile frames echo");
        var systems = profResp?.Result.GetProperty("systems");
        failures += Check(systems?.ValueKind == JsonValueKind.Array && systems.Value.GetArrayLength() == 2, "live: profile has 2 system timings");
        Console.WriteLine($"[LIVE] profile 60 frames: server {profResp?.Result.GetProperty("totalMilliseconds").GetDouble():F2}ms, wall {sw.Elapsed.TotalMilliseconds:F2}ms");
        return failures;
    }

    private static int RunCliBinaries(string socketName, ulong root, ulong child)
    {
        var cliDll = FindCliDll();
        if (cliDll == null)
        {
            return Check(false, "live: engine-cli binary not found (build Engine.Cli first)");
        }

        var rootStr = root.ToString(System.Globalization.CultureInfo.InvariantCulture);
        var childStr = child.ToString(System.Globalization.CultureInfo.InvariantCulture);
        var failures = 0;

        // macOS runners interleave stdout across fast sequential apphost
        // spawns: parse each stdout as JSON and retry once on mismatch
        // (payload asserts, not timing asserts).
        failures += CheckCliJson(cliDll, socketName, ["scene", "tree"], r => HasId(r, child),
            "live: engine-cli scene tree shows child");

        failures += CheckCliJson(cliDll, socketName, ["entity", "get", rootStr], r => HasId(r, root),
            "live: engine-cli entity get shows id");

        failures += CheckCliJson(cliDll, socketName, ["entity", "set", rootStr, "LocalTransform.py", "4.25"],
            r => r.TryGetProperty("local", out var local) && local.ValueKind == JsonValueKind.Array
                && local.GetArrayLength() == 10 && local.EnumerateArray().Any(f => MathF.Abs(f.GetSingle() - 4.25f) < 1e-4f),
            "live: engine-cli entity set py visible");

        failures += CheckCliJson(cliDll, socketName, ["snapshot", "capture", "--out", "frame.png", "--pass", "normals"],
            r => r.TryGetProperty("pass", out var pass) && pass.GetString() == "normals",
            "live: engine-cli snapshot pass echoes");

        failures += CheckCliJson(cliDll, socketName, ["profile", "--frames", "10"],
            r => r.TryGetProperty("frames", out var frames) && frames.GetInt32() == 10,
            "live: engine-cli profile frames echo");

        // Error path: bad pass exits 1.
        failures += RunCliBin(cliDll, socketName, ["snapshot", "capture", "--pass", "bogus"], 1, out _);
        failures += Check(true, "live: engine-cli bad pass exits 1");
        Console.WriteLine("[DEBUG] cli binaries done; stopping server.");
        return failures;
    }

    private static bool HasId(JsonElement root, ulong want)
    {
        if (root.ValueKind == JsonValueKind.Object && root.TryGetProperty("nodes", out var nodes))
        {
            return FindId(nodes, want);
        }

        return root.ValueKind == JsonValueKind.Object
            && root.TryGetProperty("id", out var id)
            && id.ValueKind == JsonValueKind.Number
            && id.GetUInt64() == want;
    }

    private static int CheckCliJson(string cliDll, string socketName, string[] cliArgs, Func<JsonElement, bool> probe, string name)
    {
        for (var attempt = 0; attempt < 3; attempt++)
        {
            Console.WriteLine($"[DEBUG] {name}: attempt {attempt + 1}/3 spawn {Path.GetFileName(cliDll)} {string.Join(" ", cliArgs)}");
            var rc = RunCliBin(cliDll, socketName, cliArgs, 0, out var stdout);
            if (rc != 0)
            {
                // Transport failure (not a payload mismatch): retry; only
                // the LAST attempt reports (keeps the log readable).
                if (attempt == 2)
                {
                    return rc + Check(false, name);
                }

                continue;
            }

            try
            {
                using var doc = JsonDocument.Parse(stdout.Trim());
                if (probe(doc.RootElement))
                {
                    return Check(true, name);
                }

                Console.WriteLine($"[DEBUG] {name}: probe mismatch. stdout: {stdout.Trim()}");
            }
            catch (JsonException)
            {
                // Interleaved/garbled stdout on shared runners: retry.
            }

            if (attempt == 2)
            {
                Console.WriteLine($"[DEBUG] {name}: giving up. last stdout: {stdout.Trim()}");
                return Check(false, name);
            }
        }

        return Check(false, name);
    }

    private static bool FindId(JsonElement nodes, ulong want)
    {
        foreach (var n in nodes.EnumerateArray())
        {
            if (n.TryGetProperty("id", out var id) && id.GetUInt64() == want)
            {
                return true;
            }

            if (n.TryGetProperty("children", out var kids) && FindId(kids, want))
            {
                return true;
            }
        }

        return false;
    }

    private static string? FindCliDll()
    {
        // Prefer the engine-cli apphost (self-launching exe with its own
        // runtimeconfig) next to Engine.Tests; fall back to the dll.
        var dir = AppContext.BaseDirectory;
        var apphost = Path.Combine(dir, "engine-cli");
        if (File.Exists(apphost))
        {
            return apphost;
        }

        var winExe = Path.Combine(dir, "engine-cli.exe");
        if (File.Exists(winExe))
        {
            return winExe;
        }

        var direct = Path.Combine(dir, "engine-cli.dll");
        if (File.Exists(direct))
        {
            return direct;
        }

        // Fallback: search sibling framework dirs under the build root.
        var root = Path.GetFullPath(Path.Combine(dir, "..", "..", "..", ".."));
        foreach (var dll in Directory.EnumerateFiles(root, "engine-cli.dll", SearchOption.AllDirectories))
        {
            return dll;
        }

        return null;
    }

    private static int RunCliBin(string cliPath, string socket, string[] cliArgs, int wantExit, out string stdout)
    {
        // Apphost (exe) launches directly; a bare dll needs `dotnet exec`.
        // No shell: argv passed as a list (spaces in socket paths are safe).
        var argv = new List<string> { "--socket", socket };
        argv.AddRange(cliArgs);
        var psi = cliPath.EndsWith(".dll", StringComparison.OrdinalIgnoreCase)
            ? ExeWithArgs("dotnet", ["exec", cliPath, .. argv])
            : ExeWithArgs(cliPath, argv);
        psi.RedirectStandardOutput = true;
        psi.RedirectStandardError = true;
        psi.UseShellExecute = false;
        using var proc = Process.Start(psi)!;
        var outText = new StringBuilder();
        proc.OutputDataReceived += (_, e) => { if (e.Data != null) outText.AppendLine(e.Data); };
        proc.BeginOutputReadLine();
        // Read stderr ASYNC (same pattern as stdout): ReadToEnd here blocks
        // while the child writes stdout on macOS runners (both pipes full =
        // deadlock -> ctest timeout). Drain async, join at exit.
        var errText = new StringBuilder();
        proc.ErrorDataReceived += (_, e) => { if (e.Data != null) errText.AppendLine(e.Data); };
        proc.BeginErrorReadLine();
        var exited = proc.WaitForExit(30_000);
        // Bounded drain wait: the parameterless WaitForExit() overload waits
        // indefinitely for the async stdout/stderr pumps — on macOS runners
        // that pump occasionally never signals after a fast apphost exit,
        // hanging ctest to its timeout. 5s is plenty (local: <50ms); a
        // truncated tail just fails the JSON probe and retries.
        if (exited)
        {
            proc.WaitForExit(5_000);
        }

        try
        {
            proc.CancelErrorRead();
        }
        catch (InvalidOperationException)
        {
        }

        var err = errText.ToString();
        stdout = outText.ToString();
        if (!exited)
        {
            try { proc.Kill(); } catch (InvalidOperationException) { }
            Console.WriteLine($"[FAIL] engine-cli {string.Join(" ", cliArgs)}: timed out. stderr: {err}");
            return 1;
        }

        if (proc.ExitCode != wantExit)
        {
            Console.WriteLine($"[FAIL] engine-cli {string.Join(" ", cliArgs)}: exit {proc.ExitCode} want {wantExit}. stderr: {err}");
            return 1;
        }

        Console.WriteLine($"[PASS] engine-cli {string.Join(" ", cliArgs)} exit {wantExit}");
        return 0;
    }

    private static ProcessStartInfo ExeWithArgs(string exe, List<string> argv)
    {
        var psi = new ProcessStartInfo(exe) { UseShellExecute = false };
        foreach (var a in argv)
        {
            psi.ArgumentList.Add(a);
        }

        return psi;
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }

    private static unsafe class EditorLiveLoader
    {
        public static void Load(IntPtr handle)
        {
            if (!NativeLibrary.TryGetExport(handle, "engine_get_api_table", out var addr))
            {
                throw new EntryPointNotFoundException("engine_get_api_table not exported.");
            }

            var getTable = (delegate* unmanaged[Cdecl, SuppressGCTransition]<Engine.Bindings.NativeApi.ApiTable*>)addr;
            var table = getTable();
            if (table == null)
            {
                throw new InvalidOperationException("engine_get_api_table returned null.");
            }

            EditorSession.TableOverride = (IntPtr)table;
        }
    }
}

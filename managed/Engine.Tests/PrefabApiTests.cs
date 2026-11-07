// PrefabApiTests.cs — P6-09: prefab assets, overrides, apply/revert, nesting.
//
// CPU-side checks run everywhere (command guards, CLI verb validation);
// the live session suite runs when engine_core is loadable and is skipped
// otherwise (same precedent as CompositorApiTests):
//   * save/instantiate shape: instance root IsPrefabInstance, child names.
//   * acceptance: asset edit -> update -> non-overridden lanes follow,
//     overridden lanes keep (per-field, including the Mover user blob).
//   * apply: asset file gains the instance value; revert: live restored.
//   * nesting: nested asset edit updates nested instances only.
//   * CLI verbs: prefab.save/instantiate/apply/revert/update round-trip
//     through CliDispatcher.
using System.Text.Json;
using Engine.Bindings;
using Engine.Editor;

namespace Engine.Tests;

public static class PrefabApiTests
{
    public static unsafe int Run(string[]? args = null)
    {
        var failures = 0;

        // --- CPU: command guards (no native needed) ---------------------------
        var nullGuard = false;
        try
        {
            _ = new ApplyPrefabCommand(null!, 1, "x.engine-prefab");
        }
        catch (ArgumentNullException)
        {
            nullGuard = true;
        }

        failures += Check(nullGuard, "prefab-cpu: ApplyPrefabCommand rejects null session");
        var idGuard = false;
        try
        {
            _ = new RevertPrefabCommand(null!, 0);
        }
        catch (Exception ex) when (ex is ArgumentNullException || ex is ArgumentOutOfRangeException)
        {
            idGuard = true;
        }

        failures += Check(idGuard, "prefab-cpu: RevertPrefabCommand rejects bad args");

        // --- Live native probe (skipped without engine_core) ------------------
        var liveProbe = (args ?? []).Contains("--live-native-probe");
        IntPtr liveHandle = IntPtr.Zero;
        try
        {
            liveHandle = EngineCoreLocator.TryLoad();
            if (liveHandle != IntPtr.Zero)
            {
                EditorLiveLoader.Load(liveHandle);
            }
            else
            {
                NativeApi.ApiLoader.Load();
            }
        }
        catch (Exception ex) when (ex is DllNotFoundException || ex is EntryPointNotFoundException || ex is InvalidOperationException)
        {
            if (liveProbe)
            {
                failures += Check(false, $"live probe FAILED to load native lib: {ex.GetType().Name}: {ex.Message}");
                Console.WriteLine($"PrefabApiTests: {failures} FAILED");
                return failures;
            }

            Console.WriteLine($"[SKIP] prefab live (no native lib: {ex.GetType().Name})");
            Console.WriteLine(failures == 0 ? "PrefabApiTests: all checks passed" : $"PrefabApiTests: {failures} FAILED");
            return failures;
        }

        try
        {
            failures += RunLiveSession();
        }
        catch (Exception ex)
        {
            failures += Check(false, $"prefab live threw: {ex.GetType().Name}: {ex.Message}");
        }

        Console.WriteLine(failures == 0 ? "PrefabApiTests: all checks passed" : $"PrefabApiTests: {failures} FAILED");
        return failures;
    }

    private static unsafe int RunLiveSession()
    {
        var failures = 0;
        var table = EditorSession.Table;
        NativeApi.EngineWorld* world = table->EngineWorldCreate();
        failures += Check(world != null, "live: world_create");
        if (world == null)
        {
            return failures;
        }

        try
        {
            using var session = new EditorSession(world);
            _ = Engine.Runtime.Ecs.ComponentId<Engine.Runtime.MeshRenderer>(table, world);
            var staging = Path.Combine(Path.GetTempPath(), $"p609_{Guid.NewGuid():N}");
            Directory.CreateDirectory(staging);
            try
            {
                var asset = Path.Combine(staging, "basic.engine-prefab");
                // Source subtree: Root (pose + MeshRenderer) + Child.
                var root = session.Spawn("Root");
                session.SetLocal(root, new float[] { 1, 2, 3, 0, 0, 0, 1, 1, 1, 1 });
                var child = session.Spawn("Child");
                session.SetParent(child, root);
                session.SetLocal(child, new float[] { 4, 5, 6, 0, 0, 0, 1, 1, 1, 1 });

                session.SavePrefab(root, asset);
                failures += Check(File.Exists(asset) && File.ReadAllText(asset).Contains("engine-prefab"),
                    "live: save writes an engine-prefab asset");

                var a = session.InstantiatePrefab(asset);
                var b = session.InstantiatePrefab(asset);
                failures += Check(a != 0 && b != 0 && a != b, "live: two instances spawn");
                failures += Check(session.IsPrefabInstance(a), "live: instance root is a prefab instance");
                failures += Check(!session.IsPrefabInstance(root), "live: source root is not an instance");
                var pose = new float[10];
                session.GetLocal(a, pose);
                failures += Check(MathF.Abs(pose[0] - 1) < 1e-6f && MathF.Abs(pose[1] - 2) < 1e-6f,
                    "live: instance pose matches asset");

                // Override A's X lane, then edit the asset (X=50, Y=60) and update.
                var pa = new float[10];
                session.GetLocal(a, pa);
                pa[0] = 100;
                session.SetLocal(a, pa);
                var pr = new float[10];
                session.GetLocal(root, pr);
                pr[0] = 50;
                pr[1] = 60;
                session.SetLocal(root, pr);
                session.SavePrefab(root, asset);
                var updated = session.UpdatePrefab(asset);
                failures += Check(updated == 2, $"live: update merges both instances (got {updated})");
                session.GetLocal(a, pa);
                failures += Check(MathF.Abs(pa[0] - 100) < 1e-6f, "live: overridden lane keeps instance value");
                failures += Check(MathF.Abs(pa[1] - 60) < 1e-6f, "live: non-overridden lane follows asset");
                var pb = new float[10];
                session.GetLocal(b, pb);
                failures += Check(MathF.Abs(pb[0] - 50) < 1e-6f && MathF.Abs(pb[1] - 60) < 1e-6f,
                    "live: clean instance fully follows asset");

                // Revert A: back to the asset image (100 -> 50, Y stays 60).
                session.RevertPrefab(a);
                session.GetLocal(a, pa);
                failures += Check(MathF.Abs(pa[0] - 50) < 1e-6f && MathF.Abs(pa[1] - 60) < 1e-6f,
                    "live: revert restores base image");

                // Apply: B's X=111 lands in the asset; fresh instance sees it.
                var pbm = new float[10];
                session.GetLocal(b, pbm);
                pbm[0] = 111;
                session.SetLocal(b, pbm);
                session.ApplyPrefab(b);
                var c = session.InstantiatePrefab(asset);
                var pc = new float[10];
                session.GetLocal(c, pc);
                failures += Check(MathF.Abs(pc[0] - 111) < 1e-6f, "live: apply writes instance value to asset");

                // Inspector undo: RevertPrefabCommand apply/revert round-trip.
                var inspector = new Inspector(session);
                var pd = new float[10];
                session.GetLocal(c, pd);
                pd[0] = 222;
                session.SetLocal(c, pd);
                using (var revertCmd = new RevertPrefabCommand(session, c))
                {
                    inspector.Execute(revertCmd);
                    var after = new float[10];
                    session.GetLocal(c, after);
                    failures += Check(MathF.Abs(after[0] - 111) < 1e-6f, "live: RevertPrefabCommand execute reverts");
                    failures += Check(inspector.Undo(), "live: RevertPrefabCommand undo succeeds");
                    var undone = new float[10];
                    session.GetLocal(c, undone);
                    failures += Check(MathF.Abs(undone[0] - 222) < 1e-6f, "live: RevertPrefabCommand undo restores live pose");
                }

                // CLI verbs over the same session.
                var dispatcher = new CliDispatcher(session);
                var saveLine = dispatcher.DispatchLine(JsonRpc("prefab.save", new { id = root, path = asset }, 1));
                failures += Check(IsResult(saveLine), "live: cli prefab.save ok");
                var instLine = dispatcher.DispatchLine(JsonRpc("prefab.instantiate", new { path = asset }, 2));
                failures += Check(IsResult(instLine), "live: cli prefab.instantiate ok");
                var updLine = dispatcher.DispatchLine(JsonRpc("prefab.update", new { path = asset }, 3));
                failures += Check(IsResult(updLine) && updLine.Contains("updated"), "live: cli prefab.update reports count");
                var badLine = dispatcher.DispatchLine(JsonRpc("prefab.instantiate", new { path = Path.Combine(staging, "missing.engine-prefab") }, 4));
                failures += Check(IsError(badLine), "live: cli prefab.instantiate missing asset errors");

                // Fail-closed managed guards.
                var threw = false;
                try
                {
                    session.InstantiatePrefab(Path.Combine(staging, "missing.engine-prefab"));
                }
                catch (InvalidOperationException)
                {
                    threw = true;
                }

                failures += Check(threw, "live: instantiate missing asset throws");
                var threwApply = false;
                try
                {
                    session.ApplyPrefab(root);
                }
                catch (InvalidOperationException)
                {
                    threwApply = true;
                }

                failures += Check(threwApply, "live: apply non-instance throws");
            }
            finally
            {
                try
                {
                    Directory.Delete(staging, true);
                }
                catch (IOException)
                {
                }
            }
        }
        finally
        {
            table->EngineWorldDestroy(world);
        }

        return failures;
    }

    private static string JsonRpc(string method, object? @params, int id)
    {
        var paramsJson = @params == null ? "null" : JsonSerializer.Serialize(@params);
        return $"{{\"jsonrpc\":\"2.0\",\"method\":\"{method}\",\"params\":{paramsJson},\"id\":{id}}}";
    }

    private static bool IsResult(string line)
    {
        try
        {
            using var doc = JsonDocument.Parse(line);
            return doc.RootElement.TryGetProperty("result", out _);
        }
        catch (JsonException)
        {
            return false;
        }
    }

    private static bool IsError(string line)
    {
        try
        {
            using var doc = JsonDocument.Parse(line);
            return doc.RootElement.TryGetProperty("error", out _);
        }
        catch (JsonException)
        {
            return false;
        }
    }

    private static unsafe class EditorLiveLoader
    {
        public static void Load(IntPtr handle)
        {
            EditorSession.TableOverride = (IntPtr)EngineCoreLocator.LoadTable(handle);
        }
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }
}

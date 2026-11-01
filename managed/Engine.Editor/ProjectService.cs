// ProjectService.cs — P3-10: project create/open + scene save/load over EditorSession.
//
// Plain functions under the File-menu dialogs (StorageProvider lives in the
// UI layer, absent in-tree). Scene JSON is the P2-06 shape (format
// engine-scene, version 1): file-local ids, parent refs (0 = root), named
// LocalTransform fields. Only LocalTransform + hierarchy + names persist
// (user components stay native until P2-06 component_describe lands).
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Engine.Editor;

/// <summary>Project layout + scene persistence (headless-testable).</summary>
public sealed record EngineProject(string Root, string Name, string StartScene);

/// <summary>One decoded scene entity (file-local id space).</summary>
public sealed record SceneEntity(int Id, string Name, int Parent, float[] Pose);

public static class ProjectService
{
    public const string ProjectFileName = "project.json";
    public const string DefaultScenePath = "scenes/main.scene.json";

    /// <summary>
    /// Create a new project: project.json, assets/, scenes/main.scene.json
    /// (empty scene), Game.csproj referencing Engine.Runtime.
    /// </summary>
    public static EngineProject Create(string root, string name)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(root);
        ArgumentException.ThrowIfNullOrWhiteSpace(name);
        root = Path.GetFullPath(root);
        Directory.CreateDirectory(root);
        Directory.CreateDirectory(Path.Combine(root, "assets"));
        Directory.CreateDirectory(Path.Combine(root, "scenes"));

        var proj = new EngineProject(root, name.Trim(), DefaultScenePath);
        var doc = new JsonObject
        {
            ["name"] = proj.Name,
            ["startScene"] = proj.StartScene,
            ["gameAssembly"] = "bin/Game.dll",
        };
        File.WriteAllText(Path.Combine(root, ProjectFileName), doc.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n");

        SaveScene(new List<SceneEntity>(), Path.Combine(root, DefaultScenePath));

        var csproj = $"""
            <Project Sdk="Microsoft.NET.Sdk">
              <PropertyGroup>
                <TargetFramework>net10.0</TargetFramework>
                <Nullable>enable</Nullable>
                <ImplicitUsings>enable</ImplicitUsings>
              </PropertyGroup>
              <ItemGroup>
                <ProjectReference Include="..\\..\\managed\\Engine.Runtime\\Engine.Runtime.csproj" />
              </ItemGroup>
            </Project>
            """;
        File.WriteAllText(Path.Combine(root, $"{name.Trim()}.csproj"), csproj.Replace("\n", Environment.NewLine));

        return proj;
    }

    /// <summary>Open an existing project (reads project.json).</summary>
    public static EngineProject Open(string root)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(root);
        root = Path.GetFullPath(root);
        var pf = Path.Combine(root, ProjectFileName);
        if (!File.Exists(pf))
        {
            throw new FileNotFoundException($"project.json not found: {pf}.", pf);
        }

        using var doc = JsonDocument.Parse(File.ReadAllText(pf));
        var r = doc.RootElement;
        var name = r.TryGetProperty("name", out var n) ? n.GetString() ?? Path.GetFileName(root) : Path.GetFileName(root);
        var start = r.TryGetProperty("startScene", out var s) ? s.GetString() ?? DefaultScenePath : DefaultScenePath;
        return new EngineProject(root, name, start);
    }

    /// <summary>Snapshot every entity reachable from the session into scene records.</summary>
    public static List<SceneEntity> Capture(EditorSession session)
    {
        ArgumentNullException.ThrowIfNull(session);
        var out_ = new List<SceneEntity>();
        var index = new Dictionary<ulong, int>();
        void Walk(ulong nativeId, int parentFileId)
        {
            var pose = new float[10];
            session.GetLocal(nativeId, pose);
            var name = string.Empty;
            try
            {
                name = session.GetName(nativeId);
            }
            catch (InvalidOperationException)
            {
                name = string.Empty;
            }

            var fileId = out_.Count + 1;
            index[nativeId] = fileId;
            out_.Add(new SceneEntity(fileId, name, parentFileId, (float[])pose.Clone()));
            var nc = session.CountChildren(nativeId);
            for (ulong i = 0; i < nc; i++)
            {
                Walk(session.GetChild(nativeId, i), fileId);
            }
        }

        var nr = session.CountRoots();
        for (ulong i = 0; i < nr; i++)
        {
            Walk(session.GetRoot(i), 0);
        }

        return out_;
    }

    /// <summary>Save the session's scene to path (creates parent dirs).</summary>
    public static void Save(EditorSession session, string scenePath)
    {
        ArgumentNullException.ThrowIfNull(session);
        ArgumentException.ThrowIfNullOrWhiteSpace(scenePath);
        SaveScene(Capture(session), scenePath);
    }

    /// <summary>Load a scene file into the session (spawns; returns native ids in file order).</summary>
    public static List<ulong> Load(EditorSession session, string scenePath)
    {
        ArgumentNullException.ThrowIfNull(session);
        ArgumentException.ThrowIfNullOrWhiteSpace(scenePath);
        var entities = LoadScene(scenePath);
        var ids = new List<ulong>(entities.Count);
        var map = new Dictionary<int, ulong>();
        foreach (var e in entities)
        {
            var id = session.Spawn(string.IsNullOrEmpty(e.Name) ? null : e.Name);
            session.EnsureLocal(id);
            session.SetLocal(id, e.Pose);
            map[e.Id] = id;
            ids.Add(id);
        }

        foreach (var e in entities)
        {
            if (e.Parent != 0 && map.TryGetValue(e.Parent, out var p))
            {
                session.SetParent(map[e.Id], p);
            }
        }

        return ids;
    }

    /// <summary>Count entities in a scene file (file-local count, no native).</summary>
    public static int CountEntities(string scenePath) => LoadScene(scenePath).Count;

    private static void SaveScene(List<SceneEntity> entities, string scenePath)
    {
        var dir = Path.GetDirectoryName(Path.GetFullPath(scenePath));
        if (!string.IsNullOrEmpty(dir))
        {
            Directory.CreateDirectory(dir);
        }

        var arr = new JsonArray();
        foreach (var e in entities)
        {
            arr.Add(new JsonObject
            {
                ["id"] = e.Id,
                ["name"] = e.Name,
                ["parent"] = e.Parent,
                ["components"] = new JsonObject
                {
                    ["LocalTransform"] = new JsonObject
                    {
                        ["position"] = new JsonArray(e.Pose[0], e.Pose[1], e.Pose[2]),
                        ["rotation"] = new JsonArray(e.Pose[3], e.Pose[4], e.Pose[5], e.Pose[6]),
                        ["scale"] = new JsonArray(e.Pose[7], e.Pose[8], e.Pose[9]),
                    },
                },
            });
        }

        var root = new JsonObject
        {
            ["format"] = "engine-scene",
            ["version"] = 1,
            ["entities"] = arr,
        };
        File.WriteAllText(scenePath, root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n");
    }

    private static List<SceneEntity> LoadScene(string scenePath)
    {
        using var doc = JsonDocument.Parse(File.ReadAllText(scenePath));
        var r = doc.RootElement;
        if (!r.TryGetProperty("format", out var f) || f.GetString() != "engine-scene")
        {
            throw new InvalidDataException($"not an engine scene: {scenePath}.");
        }

        var list = new List<SceneEntity>();
        foreach (var el in r.GetProperty("entities").EnumerateArray())
        {
            var id = el.GetProperty("id").GetInt32();
            var name = el.TryGetProperty("name", out var ne) ? ne.GetString() ?? string.Empty : string.Empty;
            var parent = el.TryGetProperty("parent", out var pe) ? pe.GetInt32() : 0;
            var pose = new float[] { 0, 0, 0, 0, 0, 0, 1, 1, 1, 1 };
            if (el.TryGetProperty("components", out var c)
                && c.TryGetProperty("LocalTransform", out var lt))
            {
                ReadVec(lt, "position", pose, 0, 3);
                ReadVec(lt, "rotation", pose, 3, 4);
                ReadVec(lt, "scale", pose, 7, 3);
            }

            list.Add(new SceneEntity(id, name, parent, pose));
        }

        return list;
    }

    private static void ReadVec(JsonElement obj, string key, float[] pose, int off, int n)
    {
        if (!obj.TryGetProperty(key, out var a) || a.ValueKind != JsonValueKind.Array)
        {
            return;
        }

        var i = 0;
        foreach (var v in a.EnumerateArray())
        {
            if (i >= n)
            {
                break;
            }

            if (v.TryGetSingle(out var f))
            {
                pose[off + i] = f;
            }

            i++;
        }
    }
}

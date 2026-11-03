// Scene.cs — P2-06: scene files (JSON v1) save and load.
//
// Thin C# wrapper over the ABI 4.16 scene tail (scene_save/scene_load):
// both take the world + a NUL-terminated UTF-8 path. Save returns 0 on
// success; load returns the skipped-bad-component count (>= 0) or a
// negative code on hard error. Plain Cdecl (file IO may block — never
// SuppressGCTransition).
using System.Text;
using Engine.Bindings;

namespace Engine.Runtime;

/// <summary>Scene file (JSON v1) save and load over a native world.</summary>
public static unsafe class Scene
{
    /// <summary>
    /// Saves every LocalTransform-carrying entity in <paramref name="world"/>
    /// to <paramref name="path"/> (UTF-8). Throws on failure.
    /// </summary>
    public static void Save(NativeApi.ApiTable* table, NativeApi.EngineWorld* world, string path)
    {
        ArgumentNullException.ThrowIfNull(table);
        ArgumentNullException.ThrowIfNull(world);
        ArgumentException.ThrowIfNullOrEmpty(path);
        var bytes = Encoding.UTF8.GetBytes(path + "\0");
        int rc;
        fixed (byte* p = bytes)
        {
            rc = table->EngineSceneSave(world, p);
        }

        if (rc != 0)
        {
            throw new InvalidOperationException($"scene_save failed with code {rc}.");
        }
    }

    /// <summary>
    /// Loads the scene file at <paramref name="path"/> into
    /// <paramref name="world"/>. Returns the count of skipped bad user
    /// components (warnings already printed natively). Throws on hard error.
    /// </summary>
    public static int Load(NativeApi.ApiTable* table, NativeApi.EngineWorld* world, string path)
    {
        ArgumentNullException.ThrowIfNull(table);
        ArgumentNullException.ThrowIfNull(world);
        ArgumentException.ThrowIfNullOrEmpty(path);
        var bytes = Encoding.UTF8.GetBytes(path + "\0");
        int rc;
        fixed (byte* p = bytes)
        {
            rc = table->EngineSceneLoad(world, p);
        }

        if (rc < 0)
        {
            throw new InvalidOperationException($"scene_load failed with code {rc}.");
        }

        return rc;
    }
}

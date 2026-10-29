// ScriptContext.cs — M2-04: collectible AssemblyLoadContext for user gameplay scripts.
//
// A ScriptContext owns ONE load of a user scripts assembly (GameScripts.dll
// bytes) inside a collectible AssemblyLoadContext. Loading from an in-memory
// byte[] (AssemblyLoadContext.LoadFromStream) — never LoadFromAssemblyPath —
// so the OS never locks GameScripts.dll/.pdb on disk and the compiler can
// overwrite them while the engine runs.
//
// Lifetime (frame sync barrier protocol):
//   1. ScriptHostManager.Load loads bytes -> new ScriptContext (collectible
//      ALC, stream-loaded). The UCO chunk-runner pointer is resolved by the
//      caller via reflection (MethodHandle.GetFunctionPointer) and handed to
//      the stable trampoline — ScriptContext never touches function pointers
//      itself so this file stays verifiable-safe except for the probe.
//   2. Old context: Unbind (drop all managed references incl. resolved
//      MethodInfos/Assemblies), Unload() the ALC, GC-collect until the
//      WeakReference dies (proves the unload — a leaked reference keeps the
//      old code alive and the barrier reports it instead of silently
//      stacking generations).
//   3. Fresh Load binds the new UCO pointers; the native world keeps every
//      component row in Flecs memory throughout (stateless reload).
//
// Dependency resolution: the context resolves ONLY its own script assembly
// from the bundled bytes; every other assembly (Engine.Runtime,
// Engine.Bindings, System.*) falls through to the default context so user
// code shares the engine's types (IComponent, NativeApi) instead of getting
// a parallel copy that would break casts across the boundary.
using System.Reflection;
using System.Runtime.Loader;

namespace Engine.Runtime;

/// <summary>
/// One collectible load of a user scripts assembly. Owns the ALC; the
/// caller owns the UCO binding (resolved MethodInfo -> trampoline slot).
/// </summary>
public sealed class ScriptContext : IDisposable
{
    private readonly ScriptLoadContext _alc;
    private bool _disposed;

    /// <summary>Loaded script assembly (valid until <see cref="Unload"/>).</summary>
    public Assembly Assembly { get; }

    /// <summary>Generation counter (0-based load index from the manager).</summary>
    public int Generation { get; }

    /// <summary>Weak handle proving the ALC actually unloaded.</summary>
    public WeakReference UnloadProof { get; }

    internal ScriptLoadContext Alc => _alc;

    private ScriptContext(ScriptLoadContext alc, Assembly assembly, int generation)
    {
        _alc = alc;
        Assembly = assembly;
        Generation = generation;
        // A WeakReference to the ALC itself (not the assembly): fires when
        // the collectible context unloads. TrackResurrection adds no value
        // here and would delay the proof.
        UnloadProof = new WeakReference(alc, trackResurrection: false);
    }

    /// <summary>
    /// Load script bytes into a fresh collectible context. The byte array is
    /// copied into a private MemoryStream so the caller's buffer (and the
    /// file it came from) is never retained or locked.
    /// </summary>
    public static ScriptContext Load(byte[] assemblyBytes, byte[]? pdbBytes, int generation)
    {
        ArgumentNullException.ThrowIfNull(assemblyBytes);
        var alc = new ScriptLoadContext(generation);
        try
        {
            using var asmStream = new MemoryStream(assemblyBytes, writable: false);
            Assembly? loaded;
            if (pdbBytes != null)
            {
                using var pdbStream = new MemoryStream(pdbBytes, writable: false);
                loaded = alc.LoadFromStream(asmStream, pdbStream);
            }
            else
            {
                loaded = alc.LoadFromStream(asmStream);
            }

            if (loaded == null)
            {
                alc.Unload();
                throw new InvalidOperationException($"ScriptContext.Load: LoadFromStream returned null (generation {generation}).");
            }

            return new ScriptContext(alc, loaded, generation);
        }
        catch
        {
            alc.Unload();
            throw;
        }
    }

    /// <summary>
    /// Find a static chunk-runner entry point by type + method name. Returns
    /// null when absent (caller decides: skip binding, keep the old slot).
    /// </summary>
    public MethodInfo? FindRunner(string typeName, string methodName)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        ArgumentException.ThrowIfNullOrWhiteSpace(typeName);
        ArgumentException.ThrowIfNullOrWhiteSpace(methodName);
        var type = Assembly.GetType(typeName, throwOnError: false, ignoreCase: false);
        var method = type?.GetMethod(methodName, BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static);
        return method;
    }

    /// <summary>
    /// Begin unload: drops the ALC. Call <see cref="WaitForUnload"/> after
    /// releasing every reference obtained from <see cref="Assembly"/> /
    /// <see cref="FindRunner"/> (MethodInfos, types, instances). Uses the
    /// public <see cref="AssemblyLoadContext.Unload"/> surface — no
    /// reflection into runtime internals.
    /// </summary>
    public void Unload()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        _alc.Unload();
    }

    /// <summary>
    /// Spin GC + finalizers until the ALC dies or <paramref name="attempts"/>
    /// exhaust. Returns true when the unload is proven (WeakReference dead).
    /// Zero managed state is retained on success.
    /// </summary>
    public bool WaitForUnload(int attempts = 10)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        for (var i = 0; i < attempts && UnloadProof.IsAlive; i++)
        {
            GC.Collect(GC.MaxGeneration, GCCollectionMode.Aggressive, blocking: true, compacting: true);
            GC.WaitForPendingFinalizers();
        }

        return !UnloadProof.IsAlive;
    }

    public void Dispose()
    {
        if (!_disposed)
        {
            _disposed = true;
            _alc.Unload();
        }
    }

    /// <summary>
    /// Collectible context resolving ONLY the script assembly from bundled
    /// bytes; everything else falls through to the default context (shared
    /// Engine.Runtime/Bindings/System types — no parallel copies).
    /// </summary>
    internal sealed class ScriptLoadContext : AssemblyLoadContext
    {
        internal ScriptLoadContext(int generation)
            : base($"GameScripts-gen{generation}", isCollectible: true)
        {
        }

        protected override Assembly? Load(AssemblyName assemblyName) => null;
    }
}

// ScriptHostManager.cs — M2-04: stateless hot-reload orchestrator.
//
// Owns the reload lifecycle: load script bytes into a collectible ALC
// (stream-loaded, never file-locked), resolve UCO runner pointers,
// publish them into the stable dispatcher trampolines at the frame sync
// barrier, migrate component schemas, and unload the previous generation.
//
// The frame sync barrier contract: Reload MUST be called when no
// run_systems dispatch is in flight (the editor's frame boundary / the
// benchmark's inter-frame gap). The manager enforces this cheaply: it
// refuses Reload while a dispatch guard is held (BeginFrame/EndFrame
// pairing), so a reload can never swap code under running lanes.
//
// No state loss: component DATA is never read into managed memory during
// reload — only SCHEMAS (sizes/offsets) are compared. Row migration runs
// through the caller's IRowMigrator (native Flecs owns the columns; the
// default migrator is a no-op for identical schemas, which is the common
// edit-code-not-schema case and costs ~0).
//
// Fault isolation: runner invocation always flows through
// ScriptSystemDispatcher (try/catch fault barrier). The manager surfaces
// LastFault per slot; a faulted slot stays bound-but-disabled until the
// next Publish re-arms it.
using System.Diagnostics;
using System.Reflection;
using System.Runtime.CompilerServices;

namespace Engine.Runtime;

/// <summary>
/// Migrates live rows from an old component schema to a new one. The
/// implementation owns the memory (native Flecs columns or a test buffer);
/// the plan owns the decision. Called at the barrier, never per-frame.
/// </summary>
public interface IRowMigrator
{
    /// <summary>
    /// Migrate every live row of a component. Returns rows migrated.
    /// </summary>
    unsafe int MigrateRows(ComponentSchema oldSchema, ComponentSchema newSchema, MigrationPlan plan);
}

/// <summary>
/// Null migrator: only valid for identical schemas (the common
/// edit-code-not-schema case, ~0 cost). Throws on any non-identity plan:
/// without row pointers there is nothing to migrate, and reporting success
/// while new code reads old layouts would be silent corruption. Hosts with
/// live row memory must supply a real <see cref="IRowMigrator"/>.
/// </summary>
public sealed class NullRowMigrator : IRowMigrator
{
    public static readonly NullRowMigrator Instance = new();

    public unsafe int MigrateRows(ComponentSchema oldSchema, ComponentSchema newSchema, MigrationPlan plan)
    {
        ArgumentNullException.ThrowIfNull(plan);
        if (!plan.IsIdentity)
        {
            throw new InvalidOperationException(
                $"NullRowMigrator: schema {oldSchema.ComponentName} changed ({oldSchema.Size}B -> {newSchema.Size}B); " +
                "no row accessor wired — supply an IRowMigrator or the reload would silently misread rows.");
        }

        return 0;
    }
}

/// <summary>
/// One bound script system: slot + type/method identity + schema.
/// </summary>
public sealed record ScriptSystemBinding(int Slot, string TypeName, string MethodName, ComponentSchema? Schema);

/// <summary>
/// Result of a reload: per-system bindings, elapsed barrier time, rows migrated.
/// </summary>
public sealed record ReloadResult(
    int Generation,
    IReadOnlyList<ScriptSystemBinding> Bindings,
    TimeSpan BarrierTime,
    long RowsMigrated,
    bool UnloadedPrevious);

/// <summary>
/// Stateless hot-reload orchestrator. Single-owner per process (mirrors the
/// dispatcher's process-wide slots).
/// </summary>
public sealed class ScriptHostManager : IDisposable
{
    private readonly IRowMigrator _migrator;
    private readonly Dictionary<int, ScriptSystemBinding> _bindings = new();
    private ScriptContext? _current;
    private int _generation;
    private int _framesInFlight;
    private bool _disposed;

    public ScriptHostManager(IRowMigrator? migrator = null)
    {
        _migrator = migrator ?? NullRowMigrator.Instance;
    }

    /// <summary>Currently loaded generation (-1 = nothing loaded).</summary>
    public int Generation => _current?.Generation ?? -1;

    /// <summary>Systems bound in the current generation.</summary>
    public IReadOnlyCollection<ScriptSystemBinding> Bindings => _bindings.Values;

    /// <summary>
    /// Guard a dispatch region: while held, Reload throws (never swap code
    /// under running lanes). Pair every BeginFrame with EndFrame.
    /// </summary>
    public void BeginFrame() => Interlocked.Increment(ref _framesInFlight);

    public void EndFrame() => Interlocked.Decrement(ref _framesInFlight);

    /// <summary>
    /// Initial load: stream-load bytes, resolve + publish runners, record schemas.
    /// </summary>
    public unsafe ReloadResult Load(
        byte[] assemblyBytes,
        byte[]? pdbBytes,
        IReadOnlyList<(string TypeName, string MethodName)> systems)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        ArgumentNullException.ThrowIfNull(assemblyBytes);
        ArgumentNullException.ThrowIfNull(systems);
        if (_current != null)
        {
            throw new InvalidOperationException("ScriptHostManager.Load: already loaded; call Reload.");
        }

        if (systems.Count > ScriptSystemDispatcher.MaxSlots)
        {
            throw new ArgumentOutOfRangeException(nameof(systems), $"Max {ScriptSystemDispatcher.MaxSlots} systems.");
        }

        var sw = Stopwatch.StartNew();
        var ctx = ScriptContext.Load(assemblyBytes, pdbBytes, _generation);
        var bound = PublishAll(ctx, systems, _generation);
        _current = ctx;
        sw.Stop();
        return new ReloadResult(_generation, bound, sw.Elapsed, 0, false);
    }

    /// <summary>
    /// Hot-reload at the frame sync barrier: unbind old slots, unload the
    /// old ALC (proven via WeakReference), load the new bytes, migrate
    /// schemas, publish new runners. Component DATA stays in native memory
    /// throughout; only schemas cross into managed code.
    /// Returns the barrier time (the milestone gate budgets 100ms).
    /// </summary>
    public unsafe ReloadResult Reload(
        byte[] assemblyBytes,
        byte[]? pdbBytes,
        IReadOnlyList<(string TypeName, string MethodName)> systems)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        ArgumentNullException.ThrowIfNull(assemblyBytes);
        ArgumentNullException.ThrowIfNull(systems);
        if (_current == null)
        {
            throw new InvalidOperationException("ScriptHostManager.Reload: nothing loaded; call Load first.");
        }

        if (Volatile.Read(ref _framesInFlight) != 0)
        {
            throw new InvalidOperationException("ScriptHostManager.Reload: dispatch in flight; reload only at the frame sync barrier.");
        }

        if (systems.Count > ScriptSystemDispatcher.MaxSlots)
        {
            throw new ArgumentOutOfRangeException(nameof(systems), $"Max {ScriptSystemDispatcher.MaxSlots} systems.");
        }

        var sw = Stopwatch.StartNew();

        // Snapshot old schemas (offsets/sizes only — never row data) into a
        // local; the dictionary is dead before the proof loop (its values
        // root ComponentSchema records only — no loader references — but
        // dead-is-dead keeps the proof airtight).
        var oldSchemas = _bindings.Values.ToDictionary(
            b => (b.TypeName, b.MethodName),
            b => b.Schema);

        // Unbind every old slot FIRST: stray in-flight trampoline calls
        // (impossible by the barrier, but defense in depth) become no-ops
        // instead of jumping into unloading code.
        foreach (var slot in _bindings.Keys.ToList())
        {
            ScriptSystemDispatcher.Unbind(slot);
        }

        // The unload sequence is split across no-inline frames (proven
        // pattern — see HotReloadTests AlcFrame notes): DropPrevious touches
        // the outgoing generation last (unbind already done above), then its
        // locals die on return; CollectUnloaded runs the proof GC with no
        // loader locals alive; only then does the reload bind the new
        // generation. A single frame holding `previous` across the proof
        // loop keeps the JIT root alive and the proof always fails.
        _generation++;
        DropPrevious();
        var unloaded = CollectUnloaded();
        var outcome = BindGeneration(assemblyBytes, pdbBytes, systems, oldSchemas, _generation, _migrator);
        oldSchemas.Clear();
        _bindings.Clear();
        foreach (var b in outcome.Bindings)
        {
            _bindings[b.Slot] = b;
        }

        _current = outcome.Context;
        sw.Stop();
        if (!unloaded)
        {
            Console.Error.WriteLine($"[hot-reload] generation {_generation - 1} ALC still alive after unload — a managed reference leaked (see UnloadProof).");
        }

        return new ReloadResult(_generation, outcome.Bindings, sw.Elapsed, outcome.RowsMigrated, unloaded);
    }

    // Last frame to touch the outgoing generation: unbind already done by
    // the caller; here stash the unload proof (a WeakReference — does not
    // root), drop every managed root (bindings, context field) and unload.
    // Runner RuntimeMethodInfos are deliberately NOT retained (see BindGeneration):
    // each one roots its loader, so keeping them defeats the unload proof
    // and keeps executable code alive. NoInline so all locals die on return.
    [MethodImpl(MethodImplOptions.NoInlining)]
    private void DropPrevious()
    {
        var previous = _current;
        _current = null;
        _bindings.Clear();
        s_lastUnloadProof = previous!.UnloadProof;
        previous.Unload();
        previous.Dispose();
    }

    private static WeakReference? s_lastUnloadProof;

    // Proof GC loop with NO loader locals alive (the only ALC-adjacent
    // value is the WeakReference, which does not root). NoInline so the
    // caller frame's dead locals stay dead.
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static bool CollectUnloaded()
    {
        var proof = s_lastUnloadProof;
        s_lastUnloadProof = null;
        if (proof == null)
        {
            return false;
        }

        for (var i = 0; i < 30 && proof.IsAlive; i++)
        {
            GC.Collect(GC.MaxGeneration, GCCollectionMode.Aggressive, blocking: true, compacting: true);
            GC.WaitForPendingFinalizers();
        }

        return !proof.IsAlive;
    }

    // Bind one generation: stream-load bytes, migrate schemas against the
    // snapshot, publish runners. Touches ONLY the new generation — the
    // previous one is already unreachable. The resolved MethodInfos are
    // intentionally NOT returned or retained: each RuntimeMethodInfo roots
    // its loader, so holding one defeats the later unload proof (ALC stays
    // alive). Only the raw nint pointer (published to the dispatcher) and
    // the value-type schema snapshot escape this frame. NoInline keeps its
    // locals out of the proof frames above.
    [MethodImpl(MethodImplOptions.NoInlining)]
    private static (ScriptContext Context, List<ScriptSystemBinding> Bindings, long RowsMigrated) BindGeneration(
        byte[] assemblyBytes,
        byte[]? pdbBytes,
        IReadOnlyList<(string TypeName, string MethodName)> systems,
        Dictionary<(string TypeName, string MethodName), ComponentSchema?> oldSchemas,
        int generation,
        IRowMigrator migrator)
    {
        var ctx = ScriptContext.Load(assemblyBytes, pdbBytes, generation);
        long rowsMigrated = 0;
        var bound = new List<ScriptSystemBinding>(systems.Count);
        try
        {
            for (var i = 0; i < systems.Count; i++)
            {
                var (typeName, methodName) = systems[i];
                var runner = ctx.FindRunner(typeName, methodName)
                    ?? throw new MissingMethodException(ctx.Assembly.FullName, $"{typeName}.{methodName}");

                ComponentSchema? newSchema = TryCaptureComponentSchema(runner.DeclaringType);
                if (oldSchemas.TryGetValue((typeName, methodName), out var oldSchema) &&
                    oldSchema != null && newSchema != null)
                {
                    var plan = ComponentMigrator.Plan(oldSchema, newSchema);
                    if (!plan.IsIdentity)
                    {
                        rowsMigrated += migrator.MigrateRows(oldSchema, newSchema, plan);
                    }
                }

                var fn = runner.MethodHandle.GetFunctionPointer();
                ScriptSystemDispatcher.Publish(i, (nint)fn, generation);
                bound.Add(new ScriptSystemBinding(i, typeName, methodName, newSchema));
            }

            return (ctx, bound, rowsMigrated);
        }
        catch
        {
            for (var i = 0; i < bound.Count; i++)
            {
                ScriptSystemDispatcher.Unbind(bound[i].Slot);
            }
            ctx.Unload();
            ctx.Dispose();
            throw;
        }
    }

    /// <summary>
    /// Unbind all slots + unload the current generation (editor shutdown path).
    /// </summary>
    public void UnloadAll()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        UnloadAllCore();
    }

    private void UnloadAllCore()
    {
        foreach (var slot in _bindings.Keys.ToList())
        {
            ScriptSystemDispatcher.Unbind(slot);
        }

        _bindings.Clear();
        _current?.Unload();
        _current?.Dispose();
        _current = null;
    }

    public void Dispose()
    {
        if (!_disposed)
        {
            _disposed = true;
            UnloadAllCore();
        }
    }

    // Publish every runner of a fresh context into dispatcher slots.
    // The resolved MethodInfos stay LOCAL to this NoInline frame: each
    // RuntimeMethodInfo roots its loader, so retaining one (even in a
    // field) defeats the later unload proof. Only raw pointers escape.
    [MethodImpl(MethodImplOptions.NoInlining)]
    private unsafe List<ScriptSystemBinding> PublishAll(
        ScriptContext ctx,
        IReadOnlyList<(string TypeName, string MethodName)> systems,
        int generation)
    {
        var bound = new List<ScriptSystemBinding>(systems.Count);
        for (var i = 0; i < systems.Count; i++)
        {
            var (typeName, methodName) = systems[i];
            var runner = ctx.FindRunner(typeName, methodName)
                ?? throw new MissingMethodException(ctx.Assembly.FullName, $"{typeName}.{methodName}");
            var fn = runner.MethodHandle.GetFunctionPointer();
            ScriptSystemDispatcher.Publish(i, (nint)fn, generation);
            var schema = TryCaptureComponentSchema(runner.DeclaringType);
            var binding = new ScriptSystemBinding(i, typeName, methodName, schema);
            bound.Add(binding);
            _bindings[i] = binding;
        }

        return bound;
    }

    // The component schema is the runner's declaring struct when it is a
    // blittable value type; otherwise null (no schema to evolve — e.g. a
    // static helper class hosting the runner).
    private static ComponentSchema? TryCaptureComponentSchema(Type? declaringType)
    {
        if (declaringType == null || !declaringType.IsValueType)
        {
            return null;
        }

        try
        {
            return ComponentSchema.Capture(declaringType);
        }
        catch (ArgumentException)
        {
            return null;
        }
    }
}

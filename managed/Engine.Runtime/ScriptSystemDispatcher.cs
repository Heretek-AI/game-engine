// ScriptSystemDispatcher.cs — M2-04: stable managed trampoline + fault barrier.
//
// The frame-sync-barrier problem: native Flecs holds raw UCO function
// pointers. A hot-reload swaps the code behind those pointers — the old
// collectible ALC must die, but native keeps calling whatever address it
// registered. So native registers THESE addresses once (they live in
// Engine.Runtime, default ALC, never unloaded) and each trampoline forwards
// to the CURRENT generation's runner through a swappable slot:
//
//   native run_systems -> DispatchSlotN(view) [stable, default ALC]
//     -> s_runners[N] (current generation UCO pointer, volatile read)
//       -> user RunChunk (collectible ALC)
//
// Reload protocol (frame sync barrier, no frame in flight):
//   1. Publish(estimationId, fn): publish new pointers (volatile writes),
//      bump the generation. In-flight frames cannot exist — the barrier
//      holds the only thread that calls run_systems.
//   2. Unbind(slot): publish null (zero) for a slot. Native still calls the
//      stable trampoline; the trampoline sees an empty slot and returns
//      without touching unloaded code. Safe even if a stray in-flight call
//      lands mid-unload.
//   3. Old ALC unloads only AFTER its slots are unbound/retargeted, so no
//      dangling executable code survives.
//
// Fault barrier (AC: exceptions fault-isolate): a UCO method must not let a
// managed exception escape to native (fatal / undefined). The trampoline
// wraps the runner call in try/catch: on throw it captures the stack trace
// into the disabled-system log, marks the slot disabled (no further calls),
// and returns normally — the engine/editor keeps running. Disabled slots
// re-arm on the next Publish (fresh code deserves a fresh chance).
//
// user_data routing: the M1-05 benchmark wires Velocity through user_data,
// but M2-04 script slots carry their own runner identity — the dispatcher
// does NOT interpret user_data (an opaque channel owned by the caller).
// Slots are process-wide singletons (static state), which is correct: one
// ScriptHostManager owns the reload lifecycle per process.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Engine.Bindings;

namespace Engine.Runtime;

/// <summary>
/// Stable UCO trampolines + per-slot fault barrier for hot-reload.
/// Native binds these addresses once; reloads only swap the targets.
/// </summary>
public static unsafe class ScriptSystemDispatcher
{
    /// <summary>Maximum concurrently bound script systems (fixed table, no per-frame alloc).</summary>
    public const int MaxSlots = 8;

    // Current-generation runner per slot (null = unbound). Volatile: the
    // barrier thread publishes, worker lanes read during dispatch.
    private static volatile nint s_runners0;
    private static volatile nint s_runners1;
    private static volatile nint s_runners2;
    private static volatile nint s_runners3;
    private static volatile nint s_runners4;
    private static volatile nint s_runners5;
    private static volatile nint s_runners6;
    private static volatile nint s_runners7;

    // Generation each slot currently serves (for diagnostics / no-state-loss proof).
    private static volatile int s_gen0;
    private static volatile int s_gen1;
    private static volatile int s_gen2;
    private static volatile int s_gen3;
    private static volatile int s_gen4;
    private static volatile int s_gen5;
    private static volatile int s_gen6;
    private static volatile int s_gen7;

    // Fault-barrier state: nonzero = slot disabled after a throw.
    private static volatile int s_disabled0;
    private static volatile int s_disabled1;
    private static volatile int s_disabled2;
    private static volatile int s_disabled3;
    private static volatile int s_disabled4;
    private static volatile int s_disabled5;
    private static volatile int s_disabled6;
    private static volatile int s_disabled7;

    // Last fault per slot (exception type + stack, captured at the throw
    // site — the only managed allocation on the fault path, never the frame path).
    private static string? s_fault0;
    private static string? s_fault1;
    private static string? s_fault2;
    private static string? s_fault3;
    private static string? s_fault4;
    private static string? s_fault5;
    private static string? s_fault6;
    private static string? s_fault7;

    private static readonly object s_faultLock = new();

    /// <summary>
    /// Publish (or retarget) a slot to a new runner. Re-arms a disabled slot.
    /// Must be called at the frame sync barrier (no dispatch in flight).
    /// A publish with the SAME pointer is a no-op for the runner but still
    /// re-arms the slot and records the generation (idempotent re-publish
    /// is the bench/editor pattern after every Reload).
    /// </summary>
    public static void Publish(int slot, nint runner, int generation)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(slot);
        ArgumentOutOfRangeException.ThrowIfGreaterThanOrEqual(slot, MaxSlots);
        SetRunner(slot, runner);
        SetGeneration(slot, generation);
        SetDisabled(slot, 0);
        Interlocked.MemoryBarrier();
    }

    /// <summary>
    /// Barrier generation fence: bump without touching runners. Native
    /// caches dispatch plans per system-count, not per code generation —
    /// a reload that preserves the registry needs no native invalidation.
    /// This fence exists for future native plan-version hooks; today it is
    /// a compiler fence (prevents reordering of slot publishes across the
    /// barrier) and documents the reload point for profilers.
    /// </summary>
    public static void FenceBarrier()
    {
        Interlocked.MemoryBarrier();
    }

    /// <summary>
    /// Unbind a slot at the barrier: the stable trampoline stays registered
    /// in native but becomes a no-op until the next Publish.
    /// Full memory barrier on publish/unbind: the barrier thread's slot
    /// writes must be visible to worker lanes before the next dispatch.
    /// (Volatile int/nint fields are already acquire/release per access;
    /// the explicit fence additionally orders the reader-cursor reset in
    /// bench/editor flows against slot publication.)
    /// </summary>
    public static void Unbind(int slot)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(slot);
        ArgumentOutOfRangeException.ThrowIfGreaterThanOrEqual(slot, MaxSlots);
        SetRunner(slot, 0);
        Interlocked.MemoryBarrier();
    }

    /// <summary>Current runner for a slot (0 = unbound). For diagnostics/tests.</summary>
    public static nint CurrentRunner(int slot)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(slot);
        ArgumentOutOfRangeException.ThrowIfGreaterThanOrEqual(slot, MaxSlots);
        return GetRunner(slot);
    }

    /// <summary>True when the slot was disabled by the fault barrier.</summary>
    public static bool IsDisabled(int slot)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(slot);
        ArgumentOutOfRangeException.ThrowIfGreaterThanOrEqual(slot, MaxSlots);
        return GetDisabled(slot) != 0;
    }

    /// <summary>Last captured fault text for a slot (null when never faulted).</summary>
    public static string? LastFault(int slot)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(slot);
        ArgumentOutOfRangeException.ThrowIfGreaterThanOrEqual(slot, MaxSlots);
        lock (s_faultLock)
        {
            return GetFault(slot);
        }
    }

    /// <summary>Generation a slot currently serves. For diagnostics/tests.</summary>
    public static int SlotGeneration(int slot)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(slot);
        ArgumentOutOfRangeException.ThrowIfGreaterThanOrEqual(slot, MaxSlots);
        return GetGeneration(slot);
    }

    /// <summary>
    /// Stable trampoline address for a slot — the pointer native registers
    /// ONCE. Lives in the default ALC; never unloaded.
    /// </summary>
    public static nint TrampolineAddress(int slot)
    {
        ArgumentOutOfRangeException.ThrowIfNegative(slot);
        ArgumentOutOfRangeException.ThrowIfGreaterThanOrEqual(slot, MaxSlots);
        return slot switch
        {
            0 => (nint)(delegate* unmanaged[Cdecl]<NativeApi.EcsChunkView*, float, void>)&Dispatch0,
            1 => (nint)(delegate* unmanaged[Cdecl]<NativeApi.EcsChunkView*, float, void>)&Dispatch1,
            2 => (nint)(delegate* unmanaged[Cdecl]<NativeApi.EcsChunkView*, float, void>)&Dispatch2,
            3 => (nint)(delegate* unmanaged[Cdecl]<NativeApi.EcsChunkView*, float, void>)&Dispatch3,
            4 => (nint)(delegate* unmanaged[Cdecl]<NativeApi.EcsChunkView*, float, void>)&Dispatch4,
            5 => (nint)(delegate* unmanaged[Cdecl]<NativeApi.EcsChunkView*, float, void>)&Dispatch5,
            6 => (nint)(delegate* unmanaged[Cdecl]<NativeApi.EcsChunkView*, float, void>)&Dispatch6,
            _ => (nint)(delegate* unmanaged[Cdecl]<NativeApi.EcsChunkView*, float, void>)&Dispatch7,
        };
    }

    // --- Stable trampolines (one per slot; NoInlining keeps the address stable) ---

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    [MethodImpl(MethodImplOptions.NoInlining)]
    public static void Dispatch0(NativeApi.EcsChunkView* view, float dt) => InvokeSlot(0, view, dt);

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    [MethodImpl(MethodImplOptions.NoInlining)]
    public static void Dispatch1(NativeApi.EcsChunkView* view, float dt) => InvokeSlot(1, view, dt);

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    [MethodImpl(MethodImplOptions.NoInlining)]
    public static void Dispatch2(NativeApi.EcsChunkView* view, float dt) => InvokeSlot(2, view, dt);

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    [MethodImpl(MethodImplOptions.NoInlining)]
    public static void Dispatch3(NativeApi.EcsChunkView* view, float dt) => InvokeSlot(3, view, dt);

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    [MethodImpl(MethodImplOptions.NoInlining)]
    public static void Dispatch4(NativeApi.EcsChunkView* view, float dt) => InvokeSlot(4, view, dt);

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    [MethodImpl(MethodImplOptions.NoInlining)]
    public static void Dispatch5(NativeApi.EcsChunkView* view, float dt) => InvokeSlot(5, view, dt);

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    [MethodImpl(MethodImplOptions.NoInlining)]
    public static void Dispatch6(NativeApi.EcsChunkView* view, float dt) => InvokeSlot(6, view, dt);

    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    [MethodImpl(MethodImplOptions.NoInlining)]
    public static void Dispatch7(NativeApi.EcsChunkView* view, float dt) => InvokeSlot(7, view, dt);

    // The fault barrier. No allocation, no throw on the happy path: a
    // volatile pointer read + one completeness check per dispatch. The catch
    // path (throw site only) captures the stack and disables the slot.
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static void InvokeSlot(int slot, NativeApi.EcsChunkView* view, float dt)
    {
        if (GetDisabled(slot) != 0)
        {
            return;
        }

        var runner = GetRunner(slot);
        if (runner == 0)
        {
            return;
        }

        try
        {
            ((delegate* unmanaged[Cdecl]<NativeApi.EcsChunkView*, float, void>)runner)(view, dt);
        }
#pragma warning disable CA1031 // Fault barrier: MUST catch everything — an escape is fatal to native.
        catch (Exception ex)
#pragma warning restore CA1031
        {
            RecordFault(slot, ex);
        }
    }

    private static void RecordFault(int slot, Exception ex)
    {
        SetDisabled(slot, 1);
        var text = ex.GetType().FullName + ": " + ex.Message + Environment.NewLine + ex.StackTrace;
        Console.Error.WriteLine($"[hot-reload] system slot {slot} threw; disabling slot. {text}");
        lock (s_faultLock)
        {
            SetFault(slot, text);
        }
    }

    // --- Slot field accessors (switch over a small fixed table) ---

    private static nint GetRunner(int slot) => slot switch
    {
        0 => s_runners0,
        1 => s_runners1,
        2 => s_runners2,
        3 => s_runners3,
        4 => s_runners4,
        5 => s_runners5,
        6 => s_runners6,
        _ => s_runners7,
    };

    private static void SetRunner(int slot, nint value)
    {
        switch (slot)
        {
            case 0: s_runners0 = value; break;
            case 1: s_runners1 = value; break;
            case 2: s_runners2 = value; break;
            case 3: s_runners3 = value; break;
            case 4: s_runners4 = value; break;
            case 5: s_runners5 = value; break;
            case 6: s_runners6 = value; break;
            default: s_runners7 = value; break;
        }
    }

    private static int GetGeneration(int slot) => slot switch
    {
        0 => s_gen0,
        1 => s_gen1,
        2 => s_gen2,
        3 => s_gen3,
        4 => s_gen4,
        5 => s_gen5,
        6 => s_gen6,
        _ => s_gen7,
    };

    private static void SetGeneration(int slot, int value)
    {
        switch (slot)
        {
            case 0: s_gen0 = value; break;
            case 1: s_gen1 = value; break;
            case 2: s_gen2 = value; break;
            case 3: s_gen3 = value; break;
            case 4: s_gen4 = value; break;
            case 5: s_gen5 = value; break;
            case 6: s_gen6 = value; break;
            default: s_gen7 = value; break;
        }
    }

    private static int GetDisabled(int slot) => slot switch
    {
        0 => s_disabled0,
        1 => s_disabled1,
        2 => s_disabled2,
        3 => s_disabled3,
        4 => s_disabled4,
        5 => s_disabled5,
        6 => s_disabled6,
        _ => s_disabled7,
    };

    private static void SetDisabled(int slot, int value)
    {
        switch (slot)
        {
            case 0: s_disabled0 = value; break;
            case 1: s_disabled1 = value; break;
            case 2: s_disabled2 = value; break;
            case 3: s_disabled3 = value; break;
            case 4: s_disabled4 = value; break;
            case 5: s_disabled5 = value; break;
            case 6: s_disabled6 = value; break;
            default: s_disabled7 = value; break;
        }
    }

    private static string? GetFault(int slot) => slot switch
    {
        0 => s_fault0,
        1 => s_fault1,
        2 => s_fault2,
        3 => s_fault3,
        4 => s_fault4,
        5 => s_fault5,
        6 => s_fault6,
        _ => s_fault7,
    };

    private static void SetFault(int slot, string? value)
    {
        switch (slot)
        {
            case 0: s_fault0 = value; break;
            case 1: s_fault1 = value; break;
            case 2: s_fault2 = value; break;
            case 3: s_fault3 = value; break;
            case 4: s_fault4 = value; break;
            case 5: s_fault5 = value; break;
            case 6: s_fault6 = value; break;
            default: s_fault7 = value; break;
        }
    }
}

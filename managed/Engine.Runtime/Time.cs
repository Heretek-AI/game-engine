// Time.cs — P2-03: C# view of the native world clock.
//
// The native EngineWorld owns the only frame clock (ABI 4.12 frame-clock
// tail): variable dt clamped to 0.1 s and scaled by time_scale, a fixed
// step of exactly 1/50 s drained from an accumulator, at most 5 fixed
// steps per frame. Time.Frame calls world_frame, which advances that
// clock and runs the generic systems phase by phase; systems marked
// [UpdateInPhase(Phase.FixedUpdate)] tick once per fixed step with the
// fixed dt. Every property reads native state, so there is no second
// clock to drift.
using Engine.Bindings;

namespace Engine.Runtime;

/// <summary>
/// Frame clock for gameplay code, backed by the native world clock.
/// Call <see cref="Frame"/> once per frame with the wall-clock delta;
/// the properties then read the bound world's clock.
/// </summary>
public static unsafe class Time
{
    /// <summary>Fixed step size: exactly 1/50 s (native kTimestepFixedDt).</summary>
    public const double FixedStepSeconds = 1.0 / 50.0;

    /// <summary>Variable dt clamp: 0.1 s (native kTimestepMaxDt).</summary>
    public const double MaxDeltaSeconds = 0.1;

    /// <summary>Cap on fixed steps per frame (native kTimestepMaxFixedSteps).</summary>
    public const int MaxFixedStepsPerFrame = 5;

    // Native selectors (ENGINE_TIME_* in engine_abi.h).
    private const uint SelDelta = 0;
    private const uint SelFixedDelta = 1;
    private const uint SelScale = 2;
    private const uint SelRealtime = 3;
    private const uint SelFrameCount = 0;
    private const uint SelFixedSteps = 1;
    private const uint SelClampCount = 2;

    private static NativeApi.ApiTable* s_table;
    private static NativeApi.EngineWorld* s_world;

    /// <summary>
    /// Binds the clock to a world. <see cref="Frame"/> binds automatically;
    /// call this to read a world's clock before its first frame.
    /// </summary>
    public static void Bind(NativeApi.ApiTable* table, NativeApi.EngineWorld* world)
    {
        ArgumentNullException.ThrowIfNull(table);
        ArgumentNullException.ThrowIfNull(world);
        s_table = table;
        s_world = world;
    }

    /// <summary>Drops the binding (call before destroying the bound world).</summary>
    public static void Unbind()
    {
        s_table = null;
        s_world = null;
    }

    /// <summary>
    /// Runs one frame: advances the world clock by
    /// <paramref name="wallDeltaSeconds"/> and runs the world's systems in
    /// phase order (Startup on the first frame only, then PreUpdate,
    /// Update, FixedUpdate once per fixed step, PostUpdate, Render).
    /// Returns the fixed steps taken. Throws on a native error.
    /// </summary>
    public static int Frame(NativeApi.ApiTable* table, NativeApi.EngineWorld* world, double wallDeltaSeconds)
    {
        Bind(table, world);
        var rc = table->EngineWorldFrame(world, wallDeltaSeconds);
        if (rc < 0)
        {
            throw new InvalidOperationException($"world_frame failed with code {rc}.");
        }

        return rc;
    }

    /// <summary>Variable dt for this frame: clamped wall delta times <see cref="TimeScale"/>.</summary>
    public static double DeltaTime => ReadF64(SelDelta);

    /// <summary>Fixed step size (always exactly 1/50 s).</summary>
    public static double FixedDeltaTime => ReadF64(SelFixedDelta);

    /// <summary>
    /// Multiplier applied to the clamped dt. Values &lt;= 0 or non-finite
    /// freeze simulation (dt becomes 0); the value is stored as given.
    /// </summary>
    public static double TimeScale
    {
        get => ReadF64(SelScale);
        set
        {
            var (table, world) = Bound();
            var rc = table->EngineTimeSetScale(world, value);
            if (rc != 0)
            {
                throw new InvalidOperationException($"time_set_scale failed with code {rc}.");
            }
        }
    }

    /// <summary>Total frames advanced (the first <see cref="Frame"/> makes it 1).</summary>
    public static ulong FrameCount => ReadU64(SelFrameCount);

    /// <summary>Accumulated scaled time (sum of <see cref="DeltaTime"/>).</summary>
    public static double RealtimeSinceStartup => ReadF64(SelRealtime);

    /// <summary>Fixed steps taken by the last frame (0..5).</summary>
    public static int FixedStepsThisFrame => (int)ReadU64(SelFixedSteps);

    /// <summary>Frames whose wall delta exceeded the 0.1 s clamp.</summary>
    public static ulong ClampCount => ReadU64(SelClampCount);

    private static (NativeApi.ApiTable* Table, NativeApi.EngineWorld* World) Bound()
    {
        if (s_table == null || s_world == null)
        {
            throw new InvalidOperationException("Time is not bound to a world: call Time.Bind or Time.Frame first.");
        }

        return (s_table, s_world);
    }

    private static double ReadF64(uint which)
    {
        var (table, world) = Bound();
        double value;
        var rc = table->EngineTimeGetF64(world, which, &value);
        if (rc != 0)
        {
            throw new InvalidOperationException($"time_get_f64({which}) failed with code {rc}.");
        }

        return value;
    }

    private static ulong ReadU64(uint which)
    {
        var (table, world) = Bound();
        ulong value;
        var rc = table->EngineTimeGetU64(world, which, &value);
        if (rc != 0)
        {
            throw new InvalidOperationException($"time_get_u64({which}) failed with code {rc}.");
        }

        return value;
    }
}

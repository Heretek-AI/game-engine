// Time.cs — P2-03: C# view of the native frame-loop timestep.
//
// Static frame clock over the native engine::renderer::Timestep advanced
// once per renderer_frame: variable dt clamped to 0.1 s, fixed step
// exactly 1/50 s with an accumulator, at most 5 fixed steps per frame.
// Systems marked [UpdateInPhase(Phase.FixedUpdate)] run in the fixed
// step (fixed-dt ticks); all other phases run once per frame with the
// variable dt. Frame state only — never config; the constants live in
// native (kTimestepFixedDt / kTimestepMaxDt / kTimestepMaxFixedSteps).
//
// Test seam: Advance feeds a fake wall-clock delta (same path as the
// native advance: clamp, scale, accumulate, count steps). Production
// callers advance once per frame with the wall delta; readers then see
// DeltaTime/FixedStepsThisFrame.
namespace Engine.Runtime;

/// <summary>
/// Frame clock for gameplay code. Advance once per frame with the
/// wall-clock delta (seconds); readers then see the variable
/// <see cref="DeltaTime"/>, the fixed-step count, and the fixed dt
/// FixedUpdate systems tick with. Backed by the same math as the
/// native timestep (clamp 0.1 s, fixed step 1/50 s, max 5 steps/frame,
/// time_scale): no allocation, no throw on the frame path.
/// </summary>
public static class Time
{
    /// <summary>Fixed step size: exactly 1/50 s (matches native kTimestepFixedDt).</summary>
    public const double FixedStepSeconds = 1.0 / 50.0;

    /// <summary>Variable dt clamp: 0.1 s (matches native kTimestepMaxDt).</summary>
    public const double MaxDeltaSeconds = 0.1;

    /// <summary>Cap on fixed steps drained per frame (matches native kTimestepMaxFixedSteps).</summary>
    public const int MaxFixedStepsPerFrame = 5;

    private static double _timeScale = 1.0;
    private static double _deltaTime;
    private static double _realtime;
    private static double _accumulator;
    private static ulong _frameCount;
    private static ulong _clampCount;
    private static int _fixedStepsThisFrame;

    /// <summary>
    /// Variable dt for this frame: clamped wall delta times
    /// <see cref="TimeScale"/> (0 when the scale is not positive finite).
    /// </summary>
    public static double DeltaTime => _deltaTime;

    /// <summary>Fixed step size (always exactly 1/50 s).</summary>
    public static double FixedDeltaTime => FixedStepSeconds;

    /// <summary>
    /// Time multiplier applied to the clamped dt before accumulation.
    /// Values &lt;= 0 or non-finite freeze simulation (dt becomes 0);
    /// persisted verbatim so round-trips hold.
    /// </summary>
    public static double TimeScale
    {
        get => _timeScale;
        set => _timeScale = value;
    }

    /// <summary>Total frames advanced (first Advance makes it 1).</summary>
    public static ulong FrameCount => _frameCount;

    /// <summary>Accumulated scaled time (sum of <see cref="DeltaTime"/>).</summary>
    public static double RealtimeSinceStartup => _realtime;

    /// <summary>Fixed steps taken by the last <see cref="Advance"/>.</summary>
    public static int FixedStepsThisFrame => _fixedStepsThisFrame;

    /// <summary>Times the wall delta exceeded the 0.1 s clamp.</summary>
    public static ulong ClampCount => _clampCount;

    /// <summary>
    /// Advance one frame with a wall-clock delta in seconds. Negative
    /// deltas (clock jumps) clamp to 0; spikes above 0.1 s clamp and
    /// count once. Surplus beyond the 5-step cap is dropped (no spiral
    /// of death); the accumulator keeps only the unspent remainder.
    /// Returns the fixed steps to run this frame.
    /// </summary>
    public static int Advance(double wallDeltaSeconds)
    {
        var raw = wallDeltaSeconds;
        if (!(raw >= 0.0))
        {
            raw = 0.0;
        }

        if (raw > MaxDeltaSeconds)
        {
            raw = MaxDeltaSeconds;
            _clampCount++;
        }

        var scale = _timeScale;
        if (!(scale > 0.0))
        {
            scale = 0.0;
        }

        _deltaTime = raw * scale;
        _realtime += _deltaTime;
        _frameCount++;
        _accumulator += _deltaTime;

        var steps = 0;
        while (steps < MaxFixedStepsPerFrame && _accumulator >= FixedStepSeconds)
        {
            _accumulator -= FixedStepSeconds;
            steps++;
        }

        if (steps == MaxFixedStepsPerFrame && _accumulator >= FixedStepSeconds)
        {
            _accumulator = 0.0;
        }

        _fixedStepsThisFrame = steps;
        return steps;
    }

    /// <summary>
    /// Run <paramref name="fixedStep"/> once per fixed step taken by the
    /// last <see cref="Advance"/> (the fixed-step dispatch: systems marked
    /// <c>[UpdateInPhase(Phase.FixedUpdate)]</c> tick here with
    /// <see cref="FixedDeltaTime"/>). Zero steps means zero calls.
    /// </summary>
    public static void RunFixedSteps(Action<double> fixedStep)
    {
        ArgumentNullException.ThrowIfNull(fixedStep);
        for (var i = 0; i < _fixedStepsThisFrame; i++)
        {
            fixedStep(FixedStepSeconds);
        }
    }

    /// <summary>
    /// True when the system runs in the fixed step: it carries at least
    /// one <see cref="UpdateInPhaseAttribute"/> with
    /// <see cref="Phase.FixedUpdate"/>.
    /// </summary>
    public static bool RunsInFixedUpdate(IEnumerable<UpdateInPhaseAttribute> phases)
    {
        ArgumentNullException.ThrowIfNull(phases);
        foreach (var phase in phases)
        {
            if (phase.Phase == Phase.FixedUpdate)
            {
                return true;
            }
        }

        return false;
    }

    /// <summary>Reset all frame state (tests drive fake frames through this).</summary>
    public static void ResetForTests(double timeScale = 1.0)
    {
        _timeScale = timeScale;
        _deltaTime = 0.0;
        _realtime = 0.0;
        _accumulator = 0.0;
        _frameCount = 0;
        _clampCount = 0;
        _fixedStepsThisFrame = 0;
    }
}

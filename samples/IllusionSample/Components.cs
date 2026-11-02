// Components.cs — user-style IComponent scripts. Engine.Analyzers emits
// Register/RunChunk for every struct that declares a Tick.
using Engine.Runtime;

namespace IllusionSample;

/// <summary>Read-only input for <see cref="Mover"/>; no Tick of its own.</summary>
public partial struct Health : IComponent
{
    /// <summary>Hit points; doubles as an x-velocity in this sample.</summary>
    public float Value;
}

/// <summary>Moves along x by Health.Value and along y by Speed.</summary>
public partial struct Mover : IComponent
{
    /// <summary>Units per second along y.</summary>
    public float Speed;

    /// <summary>Position x.</summary>
    public float X;

    /// <summary>Position y.</summary>
    public float Y;

    /// <summary>Per-row update: two terms (self inout, Health in) plus dt.</summary>
    public readonly void Tick(ref Mover self, in Health health, float dt)
    {
        self.X += health.Value * dt;
        self.Y += self.Speed * dt;
    }
}

/// <summary>Single-term system with an <c>in float</c> dt.</summary>
public partial struct Spinner : IComponent
{
    /// <summary>Accumulated angle.</summary>
    public float Angle;

    /// <summary>Per-row update.</summary>
    public readonly void Tick(ref Spinner self, in float dt) => self.Angle += dt;
}

/// <summary>Throws on its second increment of any row: proves fault isolation.</summary>
public partial struct Bomb : IComponent
{
    /// <summary>Times this row ticked.</summary>
    public int Ticks;

    // Pre-allocated: ENG0001 forbids `new` inside Tick (hot path).
    private static readonly InvalidOperationException Boom = new("Bomb fixture fault");

    /// <summary>Per-row update that throws once Ticks reaches 2.</summary>
    public readonly void Tick(ref Bomb self)
    {
        self.Ticks++;
        if (self.Ticks == 2)
        {
            throw Boom;
        }
    }
}

/// <summary>
/// P2-03 fixed-step system: ticks once per 1/50 s fixed step, never once
/// per frame. Proves the generated FixedUpdate path end to end.
/// </summary>
[UpdateInPhase(Phase.FixedUpdate)]
public partial struct FixedStepper : IComponent
{
    /// <summary>Fixed steps this row has run.</summary>
    public int Steps;

    /// <summary>Sum of the dt values this row received.</summary>
    public float Elapsed;

    /// <summary>Per-row fixed-step update.</summary>
    public readonly void Tick(ref FixedStepper self, float dt)
    {
        self.Steps++;
        self.Elapsed += dt;
    }
}

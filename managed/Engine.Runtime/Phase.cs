// Phase.cs — M2-01: pipeline-phase scheduling attribute.
//
// Marks which engine phase a system struct (or individual Tick method) runs
// in. The M2-02 generator reads this attribute to register the emitted chunk
// runner with the matching Flecs pipeline phase; unmarked systems default to
// Phase.Update. AllowMultiple so one system can opt into several phases.
namespace Engine.Runtime;

/// <summary>
/// Engine pipeline phases a gameplay system can run in. Ordered by
/// execution: Startup runs once at world boot, then PreUpdate, Update,
/// PostUpdate each frame, and Render after the simulation sync point.
/// </summary>
public enum Phase
{
    /// <summary>Once at world boot (spawn initial entities, load assets).</summary>
    Startup = 0,

    /// <summary>Before the main update (input sampling, camera setup).</summary>
    PreUpdate = 1,

    /// <summary>Main simulation step. Default when no attribute is present.</summary>
    Update = 2,

    /// <summary>After the main update (cleanup, follow-ups, propagation).</summary>
    PostUpdate = 3,

    /// <summary>After the simulation sync point (extract render state).</summary>
    Render = 4,
}

/// <summary>
/// Schedules the target system (or Tick method) into a pipeline <see cref="Phase"/>.
/// May be applied more than once to run in several phases.
/// </summary>
[AttributeUsage(AttributeTargets.Struct | AttributeTargets.Class | AttributeTargets.Method, AllowMultiple = true, Inherited = false)]
public sealed class UpdateInPhaseAttribute(Phase phase) : Attribute
{
    /// <summary>The phase the target runs in.</summary>
    public Phase Phase { get; } = phase;
}

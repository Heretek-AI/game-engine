// Particles.cs — P5-05: C# mirror of the native particle components.
//
// Layout matches native/ecs/include/engine/ecs/ecs_render_components.hpp
// (ParticleSystemComponent) and native/renderer/include/engine/renderer/
// particle_system.hpp (ParticleSystem settings prefix):
//   emission_rate, burst_count, burst_interval, lifetime, start_speed,
//   start_size, start_color[4], gravity, end_color[4], end_size,
//   blend, seed, pad  (76B settings prefix, written by user code)
//   alive, spawned_total, emit_carry, burst_clock  (16B native-owned live
//   state — read but never written by C#)  92B total, align 4.
namespace Engine.Runtime;

#pragma warning disable CA1051 // Visible instance fields by design: blittable interop struct.
/// <summary>GPU particle emitter (92 bytes, align 4 — matches native).</summary>
public unsafe struct ParticleSystem : IComponent
{
    public float EmissionRate;
    public float BurstCount;
    public float BurstInterval;
    public float Lifetime;
    public float StartSpeed;
    public float StartSize;
    public fixed float StartColor[4];
    public float Gravity;
    public fixed float EndColor[4];
    public float EndSize;
    public uint Blend;
    public uint Seed;
    public uint Pad;
    public uint Alive;
    public uint SpawnedTotal;
    public float EmitCarry;
    public float BurstClock;
}
#pragma warning restore CA1051

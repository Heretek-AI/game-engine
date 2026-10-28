// Velocity.cs — M1-05: the third component in the benchmark archetype.
//
// (LocalTransform, WorldTransform, Velocity) rows are what Benchmark1M
// integrates each frame: px += vx*dt, py += vy*dt, pz += vz*dt.
// Layout: 16 bytes payload (vx,vy,vz + pad) — native registers the same
// 16B size/align, so the Flecs column for Velocity is 16-byte strided.
// Managed mirror MUST stay 16 bytes (AbiLayoutTests pins it).
using System.Runtime.InteropServices;

namespace Engine.Bindings;

/// <summary>
/// Linear velocity in world units per second. Blittable 16-byte struct:
/// (vx, vy, vz, _pad) so arrays stride exactly one 16-byte lane.
/// </summary>
[StructLayout(LayoutKind.Sequential, Pack = 4, Size = 16)]
public struct Velocity
{
    public float Vx;
    public float Vy;
    public float Vz;
#pragma warning disable CA1051 // Visible instance field by design: blittable interop struct.
    public float Pad;
#pragma warning restore CA1051
}

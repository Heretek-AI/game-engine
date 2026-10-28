// IComponent.cs — M2-01: the user-facing component contract.
//
// A gameplay component is a `partial struct` implementing this marker
// interface. Blittable fields only (floats, Entity, FixedString64, math
// types, other IComponent structs) — the ENG0002 analyzer (M2-03) rejects
// managed references (string, class, interface, arrays) at compile time.
//
// Per-entity logic is a plain `Tick` method with `ref` (read-write) and
// `in` (read-only) component parameters, e.g.:
//
//   [UpdateInPhase(Phase.Update)]
//   public partial struct Mover : IComponent
//   {
//       public float3 Velocity;
//       public void Tick(ref Mover self, in float dt) { ... }
//   }
//
// The M2-02 source generator turns each Tick into an [UnmanagedCallersOnly]
// chunk runner over native Flecs columns; this interface is intentionally
// empty so the generator (not the runtime) owns the calling convention.
namespace Engine.Runtime;

/// <summary>
/// Marker interface for gameplay components. Implement as a
/// <c>partial struct</c> with blittable fields only; add a <c>Tick</c>
/// method with <c>ref</c>/<c>in</c> parameters for per-entity logic.
/// Holds no behavior itself — all frame state lives in native Flecs tables.
/// </summary>
public interface IComponent
{
}

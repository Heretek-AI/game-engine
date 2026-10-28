// Entity.cs — M2-01: the user-facing entity handle.
//
// Direct mirror of the native `ecs_entity_t` (uint64) handed out in
// EcsChunkView.Entities. A single ulong: blittable, 8 bytes, safe to embed
// in IComponent structs and to pass through [UnmanagedCallersOnly] frames.
// Id 0 is reserved as invalid (Flecs never issues entity 0).
namespace Engine.Runtime;

/// <summary>
/// Opaque handle to one native Flecs entity. Copy by value; compare by Id.
/// The native world owns the entity — this handle is just its 64-bit id.
/// </summary>
public readonly record struct Entity(ulong Id)
{
    /// <summary>Sentinel for "no entity". Never issued by the native world.</summary>
    public static Entity Invalid => new(0);

    /// <summary>False only for <see cref="Invalid"/> (Id == 0).</summary>
    public bool IsValid => Id != 0;
}

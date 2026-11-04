// Collision.cs — P2-05: zero-allocation 3D collision events + handler dispatch.
//
// Wraps the ABI 4.16 physics_drain_events tail: drains the world-owned
// TransformSync ring (fed by the Jolt ContactListener during the fixed-step
// pipeline) into a pinned, growable caller-owned buffer; views the batch as
// a ReadOnlySpan<CollisionEvent> (zero managed allocation — no new arrays,
// no LINQ). DispatchCollisionHandlers maps Jolt BodyIDs on kExit records to
// Flecs entities through a caller-supplied resolver (kExit carries Jolt
// BodyIDs: OnContactRemoved cannot touch bodies — they may be destroyed),
// then fans out to ICollisionHandler per entity.
//
// Native-owned vs caller-owned buffer (§6 says "native-owned buffer filled
// once per fixed step"): the ABI drains into the CALLER's buffer, and this
// file owns that buffer (allocated once via NativeMemory, pinned for the
// Physics lifetime, grown by doubling when the batch fills it). The span
// aliases this reused buffer — never a fresh managed array per drain.
using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Engine.Bindings;

namespace Engine.Runtime;

/// <summary>Collision event kinds: 0 = enter (OnContactAdded), 1 = exit (OnContactRemoved).</summary>
public enum CollisionKind : byte
{
    Enter = 0,
    Exit = 1,
}

/// <summary>
/// One 3D contact record. 32 bytes, align 8 — mirrors BOTH
/// engine::physics::CollisionEvent AND EngineCollisionEvent (ABI 4.16)
/// field-for-field. AbiLayoutTests pins size + offsets here.
/// </summary>
[StructLayout(LayoutKind.Sequential, Pack = 8, Size = 32)]
public unsafe struct CollisionEvent
{
    public ulong EntityA;
    public ulong EntityB;
    public float PointX;
    public float PointY;
    public float PointZ;
    public byte Kind;
    public byte Sensor;
    private fixed byte _reserved[2];

    /// <summary>True for OnContactAdded (first step this pair touches).</summary>
    public readonly bool IsEnter => Kind == (byte)CollisionKind.Enter;

    /// <summary>True for OnContactRemoved (pair separated or a body destroyed).</summary>
    public readonly bool IsExit => Kind == (byte)CollisionKind.Exit;

    /// <summary>True when either body is a sensor (trigger event).</summary>
    public readonly bool IsSensor => Sensor != 0;

    /// <summary>Contact point as a Vector3 (copies 3 floats; no allocation).</summary>
    public readonly Vector3 Point => new(PointX, PointY, PointZ);
}

/// <summary>
/// Handler for per-entity collision callbacks. Register with
/// Physics.RegisterHandler / UnregisterHandler; the runtime dispatches one
/// call per drained event (per entity side) with zero managed allocation.
/// </summary>
public interface ICollisionHandler
{
    /// <summary>First step <paramref name="self"/> touches <paramref name="other"/>.</summary>
    void OnCollisionEnter(Entity self, Entity other, Vector3 point);

    /// <summary><paramref name="self"/> separated from <paramref name="other"/> (or a body was destroyed).</summary>
    void OnCollisionExit(Entity self, Entity other, Vector3 point);
}

/// <summary>
/// Consumer-owned body→entity map for pairing kEnter/kExit (see notes
/// contract). kEnter records already carry Flecs entities; kExit records
/// carry Jolt BodyIDs (GetIndexAndSequenceNumber), so the consumer pairs
/// enter/exit via its own body→entity map (built from kEnter batches +
/// spawn tokens).
/// </summary>
public interface IBodyEntityResolver
{
    /// <summary>Returns the Flecs entity for a Jolt body id, or Entity.Invalid when unknown.</summary>
    Entity Resolve(ulong bodyId);
}

/// <summary>Drain + dispatch side of Physics (P2-05). See Collision.cs header.</summary>
public sealed unsafe partial class Physics : IDisposable
{
    private NativeApi.CollisionEvent* _eventBuffer;
    private uint _eventCapacity;
    private readonly Dictionary<ulong, List<ICollisionHandler>> _handlers = new();

    /// <summary>
    /// Drains pending contact events into the reused native buffer and returns
    /// them as a span. Drain-once-per-access: each call replaces the previous
    /// span. Zero managed allocation (steady state; the buffer grows by
    /// doubling only when a batch fills it).
    /// </summary>
    public ReadOnlySpan<CollisionEvent> CollisionEvents
    {
        get
        {
            ThrowIfDisposed();
            var table = TablePtr;
            const uint initial = 64;
            if (_eventBuffer == null)
            {
                _eventCapacity = initial;
                _eventBuffer = (NativeApi.CollisionEvent*)NativeMemory.AlignedAlloc((nuint)(_eventCapacity * 32u), 8);
                if (_eventBuffer == null)
                {
                    throw new InvalidOperationException("collision event buffer allocation failed.");
                }
            }

            uint totalDrained = 0;
            while (true)
            {
                var remaining = _eventCapacity - totalDrained;
                if (remaining == 0)
                {
                    var grown = _eventCapacity * 2;
                    var newBuffer = (NativeApi.CollisionEvent*)NativeMemory.AlignedAlloc((nuint)(grown * 32u), 8);
                    if (newBuffer == null)
                    {
                        throw new InvalidOperationException("collision event buffer growth failed.");
                    }
                    if (totalDrained > 0)
                    {
                        Buffer.MemoryCopy(_eventBuffer, newBuffer, (ulong)grown * 32UL, (ulong)totalDrained * 32UL);
                    }
                    NativeMemory.AlignedFree(_eventBuffer);
                    _eventBuffer = newBuffer;
                    _eventCapacity = grown;
                    remaining = _eventCapacity - totalDrained;
                }

                var n = table->EnginePhysicsDrainEvents(_world, _eventBuffer + totalDrained, remaining);
                totalDrained += n;

                if (n < remaining)
                {
                    return new ReadOnlySpan<CollisionEvent>(_eventBuffer, (int)totalDrained);
                }
            }
        }
    }

    /// <summary>Registers a handler for one entity's collision callbacks.</summary>
    public void RegisterHandler(Entity entity, ICollisionHandler handler)
    {
        ThrowIfDisposed();
        ArgumentNullException.ThrowIfNull(handler);
        if (!entity.IsValid)
        {
            throw new ArgumentOutOfRangeException(nameof(entity), "entity must be non-zero.");
        }

        if (!_handlers.TryGetValue(entity.Id, out var list))
        {
            list = new List<ICollisionHandler>();
            _handlers[entity.Id] = list;
        }

        if (!list.Contains(handler))
        {
            list.Add(handler);
        }
    }

    /// <summary>Unregisters a handler; removes the entity entry when empty.</summary>
    public void UnregisterHandler(Entity entity, ICollisionHandler handler)
    {
        ThrowIfDisposed();
        ArgumentNullException.ThrowIfNull(handler);
        if (_handlers.TryGetValue(entity.Id, out var list))
        {
            list.Remove(handler);
            if (list.Count == 0)
            {
                _handlers.Remove(entity.Id);
            }
        }
    }

    /// <summary>
    /// Drains once and dispatches to registered handlers: kEnter records carry
    /// Flecs entities directly; kExit body ids resolve through
    /// <paramref name="resolver"/> (null skips kExit mapping — Enter still
    /// dispatches). Returns events drained. Caller-owned buffer version of
    /// <see cref="CollisionEvents"/> for flush-style dispatch.
    /// </summary>
    public int DispatchCollisionHandlers(Span<NativeApi.CollisionEvent> scratch, IBodyEntityResolver? resolver = null)
    {
        ThrowIfDisposed();
        if (scratch.IsEmpty)
        {
            return 0;
        }

        var table = TablePtr;
        uint n;
        fixed (NativeApi.CollisionEvent* ep = scratch)
        {
            n = table->EnginePhysicsDrainEvents(_world, ep, (uint)scratch.Length);
        }

        var eventView = MemoryMarshal.Cast<NativeApi.CollisionEvent, CollisionEvent>(scratch[..(int)n]);
        DispatchToHandlers(eventView, resolver);
        return (int)n;
    }

    /// <summary>
    /// Dispatches one drained <see cref="CollisionEvents"/> batch to handlers
    /// without draining again (pairs with the span property). kExit ids resolve
    /// through <paramref name="resolver"/>; null skips kExit mapping.
    /// </summary>
    public void DispatchCurrentEvents(ReadOnlySpan<CollisionEvent> events, IBodyEntityResolver? resolver = null)
    {
        ThrowIfDisposed();
        DispatchToHandlers(events, resolver);
    }

    private void DispatchToHandlers(ReadOnlySpan<CollisionEvent> events, IBodyEntityResolver? resolver)
    {
        // Zero-alloc iteration: no LINQ, no closures, no per-event managed
        // temporaries beyond the handler-list indexer. Handler lists are only
        // allocated at RegisterHandler time (setup), never on the frame path.
        for (var i = 0; i < events.Length; i++)
        {
            ref readonly var ev = ref events[i];
            var isEnter = ev.Kind == (byte)CollisionKind.Enter;
            Entity a;
            Entity b;
            if (isEnter)
            {
                a = new Entity(ev.EntityA);
                b = new Entity(ev.EntityB);
            }
            else if (resolver != null)
            {
                a = resolver.Resolve(ev.EntityA);
                b = resolver.Resolve(ev.EntityB);
                if (!a.IsValid && !b.IsValid)
                {
                    continue;
                }
            }
            else
            {
                continue;
            }

            var point = new Vector3(ev.PointX, ev.PointY, ev.PointZ);
            if (a.IsValid && _handlers.TryGetValue(a.Id, out var la))
            {
                for (var h = 0; h < la.Count; h++)
                {
                    if (isEnter)
                    {
                        la[h].OnCollisionEnter(a, b, point);
                    }
                    else
                    {
                        la[h].OnCollisionExit(a, b, point);
                    }
                }
            }

            if (b.IsValid && b.Id != a.Id && _handlers.TryGetValue(b.Id, out var lb))
            {
                for (var h = 0; h < lb.Count; h++)
                {
                    if (isEnter)
                    {
                        lb[h].OnCollisionEnter(b, a, point);
                    }
                    else
                    {
                        lb[h].OnCollisionExit(b, a, point);
                    }
                }
            }
        }
    }

    private void DisposeCollisionBuffer()
    {
        if (_eventBuffer != null)
        {
            NativeMemory.AlignedFree(_eventBuffer);
            _eventBuffer = null;
            _eventCapacity = 0;
        }
    }
}

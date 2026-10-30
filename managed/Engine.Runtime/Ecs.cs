// Ecs.cs — ABI 4.2: typed helpers over the generic component/system tail.
//
// Every IComponent type T is registered with the native world by NAME:
// typeof(T).FullName (UTF-8, NUL-terminated), sizeof(T) and AlignOf<T>().
// The name bytes are built once per T (init-time allocation, cached in a
// static generic class); the frame path never touches this file.
//
// Generated systems (Engine.Analyzers) call ComponentId<T> while
// registering, so the names here and in generated code always agree.
using System.Text;
using Engine.Bindings;

namespace Engine.Runtime;

/// <summary>
/// Typed access to native components and entities through the ABI 4.2
/// generic tail. Init-time API: not for use inside system callbacks.
/// </summary>
public static unsafe class Ecs
{
    /// <summary>
    /// Registers <typeparamref name="T"/> with the world (idempotent) and
    /// returns its native component id. Throws when the native world
    /// already holds a component with this name but a different layout.
    /// </summary>
    public static ulong ComponentId<T>(NativeApi.ApiTable* table, NativeApi.EngineWorld* world)
        where T : unmanaged
    {
        ArgumentNullException.ThrowIfNull(table);
        ArgumentNullException.ThrowIfNull(world);
        var name = ComponentName<T>.Utf8;
        ulong id;
        fixed (byte* p = name)
        {
            id = table->EngineComponentRegister(world, p, (uint)sizeof(T), (uint)Layout.AlignOf<T>());
        }

        if (id == 0)
        {
            throw new InvalidOperationException(
                $"component_register failed for {typeof(T).FullName} (size {sizeof(T)}, align {Layout.AlignOf<T>()}): " +
                "a component with this name and a different layout already exists, or the layout is invalid.");
        }

        return id;
    }

    /// <summary>Creates one empty entity. Throws on failure.</summary>
    public static Entity CreateEntity(NativeApi.ApiTable* table, NativeApi.EngineWorld* world)
    {
        ArgumentNullException.ThrowIfNull(table);
        ArgumentNullException.ThrowIfNull(world);
        var id = table->EngineEntityCreate(world);
        if (id == 0)
        {
            throw new InvalidOperationException("entity_create failed (null world, or called inside systems_run).");
        }

        return new Entity(id);
    }

    /// <summary>Adds or overwrites component <typeparamref name="T"/> on an entity.</summary>
    public static void Set<T>(NativeApi.ApiTable* table, NativeApi.EngineWorld* world, Entity entity, in T value)
        where T : unmanaged
    {
        var component = ComponentId<T>(table, world);
        int rc;
        fixed (T* p = &value)
        {
            rc = table->EngineEntitySet(world, entity.Id, component, p, (uint)sizeof(T));
        }

        if (rc != 0)
        {
            throw new InvalidOperationException($"entity_set<{typeof(T).FullName}> failed with code {rc}.");
        }
    }

    /// <summary>Reads component <typeparamref name="T"/> of an entity. Throws when absent.</summary>
    public static T Get<T>(NativeApi.ApiTable* table, NativeApi.EngineWorld* world, Entity entity)
        where T : unmanaged
    {
        var component = ComponentId<T>(table, world);
        T value = default;
        var rc = table->EngineEntityGet(world, entity.Id, component, &value, (uint)sizeof(T));
        if (rc != 0)
        {
            throw new InvalidOperationException($"entity_get<{typeof(T).FullName}> failed with code {rc}.");
        }

        return value;
    }

    /// <summary>Runs every generic system once with frame delta <paramref name="dt"/>.</summary>
    public static void RunSystems(NativeApi.ApiTable* table, NativeApi.EngineWorld* world, float dt)
    {
        ArgumentNullException.ThrowIfNull(table);
        ArgumentNullException.ThrowIfNull(world);
        var dispatch = new NativeApi.FrameDispatch { Dt = dt };
        var rc = table->EngineSystemsRun(world, &dispatch);
        if (rc != 0)
        {
            throw new InvalidOperationException($"systems_run failed with code {rc}.");
        }
    }

    private static class ComponentName<T>
        where T : unmanaged
    {
        // FullName is non-null for every closed, non-generic-parameter type.
        internal static readonly byte[] Utf8 = Encoding.UTF8.GetBytes(typeof(T).FullName + "\0");
    }
}

/// <summary>Managed layout queries for blittable component types.</summary>
public static class Layout
{
    /// <summary>
    /// Alignment of <typeparamref name="T"/> as the runtime lays it out
    /// inside another struct (the offset of a T placed after one byte).
    /// </summary>
    public static unsafe int AlignOf<T>()
        where T : unmanaged
        => sizeof(AlignProbe<T>) - sizeof(T);

    private struct AlignProbe<T>
        where T : unmanaged
    {
#pragma warning disable CS0169, IDE0051, CA1823 // fields exist only to be laid out
        private byte _pad;
        private T _value;
#pragma warning restore CS0169, IDE0051, CA1823
    }
}

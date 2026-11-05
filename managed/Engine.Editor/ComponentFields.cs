// ComponentFields.cs — P3-04/P3-05: component field-metadata readback.
//
// Thin managed reader over the ABI 4.24 field tail (component_describe /
// field_count / field_at, out-param FieldDesc form): Describe registers
// rows for generator-equivalent registration; ReadAll returns every field
// row for a component (empty when undescribed). Init-time API: not for
// use inside system callbacks.
using System.Runtime.InteropServices;
using System.Text;
using Engine.Bindings;

namespace Engine.Editor;

/// <summary>
/// One component field row: managed name plus the native (offset,
/// ENGINE_FIELD_* type) pair. Returned by <see cref="ComponentFields.ReadAll"/>.
/// </summary>
public sealed record ComponentFieldInfo(string Name, uint Offset, uint Type);

/// <summary>
/// ABI 4.24 field-metadata readback over an open native world.
/// </summary>
public static unsafe class ComponentFields
{
    private static NativeApi.ApiTable* TablePtr =>
        EditorSession.TableOverride != IntPtr.Zero
            ? (NativeApi.ApiTable*)EditorSession.TableOverride
            : NativeApi.ApiLoader.Table;

    /// <summary>
    /// Stores field metadata for the component registered under
    /// <paramref name="componentName"/> (replaces any prior rows).
    /// </summary>
    public static void Describe(
        NativeApi.EngineWorld* world,
        string componentName,
        ReadOnlySpan<(string Name, uint Offset, uint Type)> fields)
    {
        ArgumentNullException.ThrowIfNull(world);
        ArgumentException.ThrowIfNullOrEmpty(componentName);
        var nameBytes = Encoding.UTF8.GetBytes(componentName + "\0");
        var pinned = new byte[fields.Length][];
        for (var i = 0; i < fields.Length; i++)
        {
            pinned[i] = Encoding.UTF8.GetBytes(fields[i].Name + "\0");
        }

        var handles = new GCHandle[pinned.Length];
        try
        {
            var descs = new NativeApi.FieldDesc[fields.Length];
            for (var i = 0; i < pinned.Length; i++)
            {
                handles[i] = GCHandle.Alloc(pinned[i], GCHandleType.Pinned);
                descs[i].Name = (byte*)handles[i].AddrOfPinnedObject().ToPointer();
                descs[i].Offset = fields[i].Offset;
                descs[i].Type = fields[i].Type;
            }

            int rc;
            fixed (byte* np = nameBytes)
            {
                fixed (NativeApi.FieldDesc* dp = descs)
                {
                    rc = TablePtr->EngineComponentDescribe(world, np, dp, (uint)fields.Length);
                }
            }

            if (rc != 0)
            {
                throw new InvalidOperationException($"component_describe({componentName}) failed: {rc}.");
            }
        }
        finally
        {
            foreach (var h in handles)
            {
                if (h.IsAllocated)
                {
                    h.Free();
                }
            }
        }
    }

    /// <summary>Reads back every field row for a component (empty when undescribed or unknown).</summary>
    public static IReadOnlyList<ComponentFieldInfo> ReadAll(NativeApi.EngineWorld* world, string componentName)
    {
        ArgumentNullException.ThrowIfNull(world);
        ArgumentException.ThrowIfNullOrEmpty(componentName);
        var nameBytes = Encoding.UTF8.GetBytes(componentName + "\0");
        int count;
        fixed (byte* np = nameBytes)
        {
            count = TablePtr->EngineComponentFieldCount(world, np);
        }

        // Undescribed-but-registered reads 0; unknown reads negative.
        // Both mean "no rows for the panel" — never throw here.
        if (count <= 0)
        {
            return [];
        }

        var rows = new List<ComponentFieldInfo>(count);
        for (uint i = 0; i < (uint)count; i++)
        {
            var desc = default(NativeApi.FieldDesc);
            int rc;
            fixed (byte* np = nameBytes)
            {
                rc = TablePtr->EngineComponentFieldAt(world, np, i, &desc);
            }

            if (rc != 0 || desc.Name == null)
            {
                break;
            }

            var fieldName = Marshal.PtrToStringUTF8((IntPtr)desc.Name) ?? string.Empty;
            rows.Add(new ComponentFieldInfo(fieldName, desc.Offset, desc.Type));
        }

        return rows;
    }
}

// ComponentSchema.cs — M2-04: schema-evolution support for hot-reload.
//
// When a user adds/removes a field on an IComponent struct and presses save,
// the new generation's struct has a different size/layout than the rows
// already stored in native Flecs tables. Blindly running the new runner over
// old rows would misread memory. The migration contract:
//
//   1. Snapshot: before reload, capture the OLD layout (size + ordered
//      (field, offset, size) rows) via reflection over the outgoing
//      generation's struct.
//   2. Compare: capture the NEW layout from the incoming generation.
//   3. Migrate: for every live row, copy the overlapping (name, size)
//      prefix — min(oldSize, newSize) bytes when the leading fields are
//      unchanged, else per-field copies for surviving fields — then
//      zero-fill genuinely new trailing bytes. Removed fields are dropped.
//      Existing entity values for surviving fields remain bit-identical.
//
// Layout source of truth: Marshal.OffsetOf + Marshal.SizeOf over the
// blittable struct (IComponent is analyzer-enforced blittable: no GC
// references, so offsets are stable and memcpy-safe). Field ORDER is
// declaration order (MetadataToken) — the only order a C# author controls.
//
// Where the copy happens: the manager drives migration through a caller-
// supplied row accessor (native Flecs columns stay native-owned; managed
// code only receives pointers at the barrier). This type owns the DECISION
// (what maps to what); the accessor owns the memory. Pure-managed and
// fully unit-testable without the native library.
using System.Reflection;
using System.Runtime.InteropServices;

namespace Engine.Runtime;

/// <summary>
/// Describes one blittable component layout for migration comparison.
/// </summary>
public sealed record ComponentFieldSchema(string Name, int Offset, int Size);

/// <summary>
/// Captured layout of one component struct generation.
/// </summary>
public sealed record ComponentSchema(string ComponentName, int Size, IReadOnlyList<ComponentFieldSchema> Fields)
{
    /// <summary>
    /// Capture the layout of a blittable struct type via Marshal offsets.
    /// Throws for non-blittable types (migration is memcpy-based).
    /// </summary>
    public static ComponentSchema Capture(Type type)
    {
        ArgumentNullException.ThrowIfNull(type);
        if (!type.IsValueType)
        {
            throw new ArgumentException($"Component schema requires a value type (got {type.FullName}).", nameof(type));
        }

        if (ContainsReferences(type))
        {
            throw new ArgumentException($"Component schema requires a blittable struct (got {type.FullName}).", nameof(type));
        }

        var size = Marshal.SizeOf(type);
        var fields = type
            .GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance)
            .OrderBy(f => f.MetadataToken)
            .Select(f => new ComponentFieldSchema(
                f.Name,
                checked((int)Marshal.OffsetOf(type, f.Name)),
                Marshal.SizeOf(f.FieldType)))
            .ToList();
        return new ComponentSchema(type.FullName ?? type.Name, size, fields);
    }

    // Non-generic blittability probe: value type whose instance fields are
    // all primitives (recursively). RuntimeHelpers' generic check cannot
    // take a runtime Type; this walk is the equivalent for reflection-only
    // use. Private nested reference-type fields count (no hiding behind
    // visibility).
    private static bool ContainsReferences(Type type)
    {
        foreach (var field in type.GetFields(BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Instance))
        {
            var ft = field.FieldType;
            if (!ft.IsValueType)
            {
                return true;
            }

            if (ft.IsPrimitive || ft.IsEnum || ft.IsPointer)
            {
                continue;
            }

            if (ContainsReferences(ft))
            {
                return true;
            }
        }

        return false;
    }
}

/// <summary>
/// One field copy operation in a migration plan.
/// </summary>
public sealed record FieldCopy(int SourceOffset, int DestOffset, int Size);

/// <summary>
/// Migration plan from an old schema to a new schema: overlapping field
/// copies + zero-fill window for genuinely new bytes + whether the fast
/// prefix path applies.
/// </summary>
public sealed record MigrationPlan(
    IReadOnlyList<FieldCopy> Copies,
    int ZeroOffset,
    int ZeroSize,
    bool IsPrefixCompatible,
    int OldSize,
    int NewSize)
{
    /// <summary>True when no migration work is needed (identical layout).</summary>
    public bool IsIdentity => ZeroSize == 0 && Copies.Count == 1 &&
        Copies[0] is { SourceOffset: 0, DestOffset: 0 } &&
        Copies[0].Size == OldSize && OldSize == NewSize;
}

/// <summary>
/// Computes migration plans between component schema generations.
/// </summary>
public static class ComponentMigrator
{
    /// <summary>
    /// Diff old -> new. Rules:
    ///   * Identical (same ordered fields, offsets, sizes): identity plan.
    ///   * Prefix-compatible (new fields appended, or trailing fields
    ///     removed): one prefix copy of min(old,new) + zero-fill of the new
    ///     tail when growing.
    ///   * Otherwise (reorder / type change / insert): per-field copies for
    ///     surviving (name,size) matches + zero-fill of every new byte NOT
    ///     covered by a copy. Dropped fields vanish; new fields start zero.
    /// Never throws for schema pairs: unknown shapes degrade to prefix copy
    /// + zero-fill (values preserved when the author only appended).
    /// </summary>
    public static MigrationPlan Plan(ComponentSchema oldSchema, ComponentSchema newSchema)
    {
        ArgumentNullException.ThrowIfNull(oldSchema);
        ArgumentNullException.ThrowIfNull(newSchema);

        if (IsIdentical(oldSchema, newSchema))
        {
            var n = oldSchema.Size;
            return new MigrationPlan([new FieldCopy(0, 0, n)], n, 0, true, n, n);
        }

        if (IsPrefixCompatible(oldSchema, newSchema))
        {
            var prefix = Math.Min(oldSchema.Size, newSchema.Size);
            var copies = prefix > 0 ? new List<FieldCopy> { new(0, 0, prefix) } : new List<FieldCopy>();
            var zeroSize = Math.Max(0, newSchema.Size - oldSchema.Size);
            return new MigrationPlan(
                copies,
                oldSchema.Size,
                zeroSize,
                true,
                oldSchema.Size,
                newSchema.Size);
        }

        // General path: match surviving fields by (name, size), lay out
        // copies in DEST order (sequential writes), then zero-fill every
        // destination byte no copy covers.
        var oldByName = oldSchema.Fields
            .GroupBy(f => f.Name)
            .ToDictionary(g => g.Key, g => g.First());
        var copiesByDest = new List<FieldCopy>();
        var covered = new bool[newSchema.Size];
        foreach (var dest in newSchema.Fields.OrderBy(f => f.Offset))
        {
            if (oldByName.TryGetValue(dest.Name, out var src) && src.Size == dest.Size &&
                src.Offset + src.Size <= oldSchema.Size &&
                dest.Offset + dest.Size <= newSchema.Size)
            {
                copiesByDest.Add(new FieldCopy(src.Offset, dest.Offset, dest.Size));
                for (var i = dest.Offset; i < dest.Offset + dest.Size; i++)
                {
                    covered[i] = true;
                }
            }
        }

        // Zero-fill runs: maximal uncovered spans of the new image.
        var zeroRuns = new List<(int Off, int Len)>();
        var runStart = -1;
        for (var i = 0; i < covered.Length; i++)
        {
            if (!covered[i] && runStart < 0)
            {
                runStart = i;
            }
            else if (covered[i] && runStart >= 0)
            {
                zeroRuns.Add((runStart, i - runStart));
                runStart = -1;
            }
        }

        if (runStart >= 0)
        {
            zeroRuns.Add((runStart, covered.Length - runStart));
        }

        // MigrationPlan carries ONE zero window; fold runs: zero from the
        // first uncovered byte to the end EXCEPT copy-covered holes are
        // handled by applying copies AFTER the fill. Order contract (the
        // row applicator must honor): zero-fill [ZeroOffset, end) FIRST,
        // then apply each copy. Copies restore covered bytes; uncovered
        // bytes stay zero. A single window keeps the applicator branchless.
        var firstUncovered = Array.IndexOf(covered, false);
        var zeroOff = firstUncovered < 0 ? newSchema.Size : firstUncovered;
        return new MigrationPlan(
            copiesByDest,
            zeroOff,
            newSchema.Size - zeroOff,
            false,
            oldSchema.Size,
            newSchema.Size);
    }

    /// <summary>
    /// Apply a plan to one row: src (oldSize bytes) -> dest (newSize bytes).
    /// Contract: zero-fill [ZeroOffset, newSize) first, then the copies
    /// (copies restore the bytes they cover). Uses raw pointers so the
    /// caller can hand native Flecs column memory directly.
    /// </summary>
    public static unsafe void ApplyRow(MigrationPlan plan, byte* src, byte* dest)
    {
        ArgumentNullException.ThrowIfNull(plan);
        if (src == null || dest == null)
        {
            throw new ArgumentNullException(src == null ? nameof(src) : nameof(dest));
        }

        if (plan.ZeroSize > 0)
        {
            new Span<byte>(dest + plan.ZeroOffset, plan.ZeroSize).Clear();
        }

        foreach (var copy in plan.Copies)
        {
            if (copy.Size <= 0)
            {
                continue;
            }

            new Span<byte>(src + copy.SourceOffset, copy.Size)
                .CopyTo(new Span<byte>(dest + copy.DestOffset, copy.Size));
        }
    }

    private static bool IsIdentical(ComponentSchema oldSchema, ComponentSchema newSchema)
    {
        if (oldSchema.Size != newSchema.Size ||
            oldSchema.Fields.Count != newSchema.Fields.Count)
        {
            return false;
        }

        return oldSchema.Fields.Zip(newSchema.Fields).All(pair =>
            pair.First.Name == pair.Second.Name &&
            pair.First.Offset == pair.Second.Offset &&
            pair.First.Size == pair.Second.Size);
    }

    private static bool IsPrefixCompatible(ComponentSchema oldSchema, ComponentSchema newSchema)
    {
        // Every old field must survive at the same offset/size, in order;
        // the new schema may append (grow) or the old may truncate (shrink).
        var shared = Math.Min(oldSchema.Fields.Count, newSchema.Fields.Count);
        for (var i = 0; i < shared; i++)
        {
            var o = oldSchema.Fields[i];
            var n = newSchema.Fields[i];
            if (o.Name != n.Name || o.Offset != n.Offset || o.Size != n.Size)
            {
                return false;
            }
        }

        // The extra fields (if any) must sit strictly after the shared
        // prefix — no interleaved insert.
        var longer = oldSchema.Fields.Count > newSchema.Fields.Count ? oldSchema : newSchema;
        for (var i = shared; i < longer.Fields.Count; i++)
        {
            if (longer.Fields[i].Offset < Math.Min(oldSchema.Size, newSchema.Size) &&
                longer.Fields.Count != shared)
            {
                return false;
            }
        }

        return true;
    }
}

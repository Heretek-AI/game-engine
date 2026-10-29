// TempList.cs — M2-01: frame-scoped scratch list over native memory.
//
// A (pointer, count, capacity) view over LinearFrameAllocator scratch owned
// by the native frame: the caller rents a native buffer (e.g. via the frame
// allocator's bump pointer), wraps it here, and the list never frees — the
// frame reset reclaims everything in O(1). Unmanaged T only (blittable by
// construction); Add returns false at capacity instead of growing (no hidden
// heap allocation on the hot path).
//
// Lifetime: valid only until the frame reset. Never store a TempList<T> in
// an IComponent or hold it across frames.
using System.Runtime.InteropServices;

namespace Engine.Runtime;

/// <summary>
/// Frame-scoped list view over a native buffer. (Pointer, count, capacity):
/// Add appends while under capacity (false when full — never grows), the
/// indexer gives ref access, AsSpan covers the live items. The buffer is
/// owned by the native frame allocator; this struct never allocates or frees.
/// </summary>
/// <typeparam name="T">Element type. Unmanaged (blittable) by constraint.</typeparam>
[StructLayout(LayoutKind.Sequential, Pack = 8)]
public unsafe struct TempList<T> where T : unmanaged
{
    private T* _items;
    private int _count;
    private int _capacity;

    /// <summary>Wraps a native buffer of <paramref name="capacity"/> items.</summary>
    /// <exception cref="ArgumentException">Capacity is negative.</exception>
    public TempList(T* items, int capacity)
    {
        if (capacity < 0)
        {
            throw new ArgumentException("Capacity must be non-negative.", nameof(capacity));
        }

        _items = items;
        _count = 0;
        _capacity = capacity;
    }

    /// <summary>
    /// Wraps a stack buffer (e.g. <c>stackalloc T[N]</c>). The span must stay
    /// alive as long as the list is used — keep both in the same method scope,
    /// never store across frames or in an <c>IComponent</c>.
    /// </summary>
    public TempList(Span<T> buffer)
    {
        _items = (T*)System.Runtime.CompilerServices.Unsafe.AsPointer(ref System.Runtime.InteropServices.MemoryMarshal.GetReference(buffer));
        _count = 0;
        _capacity = buffer.Length;
    }

    /// <summary>Live item count.</summary>
    public readonly int Count => _count;

    /// <summary>Maximum items before Add returns false.</summary>
    public readonly int Capacity => _capacity;

    /// <summary>True when no more items fit.</summary>
    public readonly bool IsFull => _count >= _capacity;

    /// <summary>Ref access to item <paramref name="index"/> (bounds-checked).</summary>
    /// <exception cref="ArgumentOutOfRangeException">Index out of range.</exception>
    public readonly ref T this[int index]
    {
        get
        {
            if ((uint)index >= (uint)_count)
            {
                throw new ArgumentOutOfRangeException(nameof(index));
            }

            return ref _items[index];
        }
    }

    /// <summary>Appends <paramref name="item"/>; false when full (never grows).</summary>
    public bool Add(in T item)
    {
        if (_count >= _capacity)
        {
            return false;
        }

        _items[_count] = item;
        _count++;
        return true;
    }

    /// <summary>Drops all items (keeps the buffer; O(1)).</summary>
    public void Clear() => _count = 0;

    /// <summary>Live items as a span (length == Count).</summary>
    public readonly Span<T> AsSpan() => new(_items, _count);

    /// <summary>Live items as a read-only span.</summary>
    public readonly ReadOnlySpan<T> AsReadOnlySpan() => new(_items, _count);
}

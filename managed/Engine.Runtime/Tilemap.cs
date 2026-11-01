// Tilemap.cs — P6-08: C# mirrors of the native tilemap components.
//
// Layouts match native/ecs/include/engine/ecs/ecs_render_components.hpp:
//   TilemapRenderer2D { uint w,h; float tile_size, ox, oy; uint atlas_id;
//                       int layer, order; }  32B, align 4.
//   TilemapCollider2D { float tile_size; uint solid_count; uint pad[2]; }
//                     16B, align 4.
// TilemapAsset is the managed-side grid (row-major tile ids, 0 = empty)
// plus the palette; the collider merge consumes ToSolidMask() output.
using System.Runtime.InteropServices;

namespace Engine.Runtime;

#pragma warning disable CA1051 // Visible instance fields by design: blittable interop struct.
/// <summary>Grid tilemap draw (32 bytes, align 4 — matches native).</summary>
public unsafe struct TilemapRenderer2D : IComponent
{
    public uint Width;
    public uint Height;
    public float TileSize;
    public fixed float Origin[2];
    public uint AtlasId;
    public int SortingLayer;
    public int SortOrder;
}

/// <summary>Tilemap collider marker (16 bytes, align 4 — matches native).</summary>
public struct TilemapCollider2D : IComponent
{
    public float TileSize;
    public uint SolidTileCount;
    public uint Pad0;
    public uint Pad1;
}
#pragma warning restore CA1051

/// <summary>
/// Managed-side tilemap asset: row-major tile ids (0 = empty) plus the
/// editor palette. The collider merge consumes <see cref="ToSolidMask"/>.
/// </summary>
public sealed class TilemapAsset
{
    private readonly uint[] _tiles;

    public uint Width { get; }
    public uint Height { get; }
    public float TileSize { get; }
    public float OriginX { get; }
    public float OriginY { get; }

    /// <summary>Palette: tile id -> atlas UV cell + solid flag (editor paint source).</summary>
    public TilePalette Palette { get; } = new();

    public TilemapAsset(uint width, uint height, float tileSize = 1.0f, float originX = 0.0f, float originY = 0.0f)
    {
        if (width == 0 || height == 0)
        {
            throw new ArgumentOutOfRangeException(nameof(width), "Tilemap extent must be nonzero.");
        }

        if (tileSize <= 0.0f)
        {
            throw new ArgumentOutOfRangeException(nameof(tileSize), "Tile size must be positive.");
        }

        if ((ulong)width * height > int.MaxValue)
        {
            throw new ArgumentOutOfRangeException(nameof(width), "Tilemap cell count exceeds the array limit.");
        }

        Width = width;
        Height = height;
        TileSize = tileSize;
        OriginX = originX;
        OriginY = originY;
        _tiles = new uint[width * height];
    }

    public uint GetTile(uint x, uint y)
    {
        if (x >= Width || y >= Height)
        {
            throw new ArgumentOutOfRangeException(nameof(x), "Tile coordinate out of range.");
        }

        return _tiles[y * Width + x];
    }

    public void SetTile(uint x, uint y, uint tileId)
    {
        if (x >= Width || y >= Height)
        {
            throw new ArgumentOutOfRangeException(nameof(x), "Tile coordinate out of range.");
        }

        _tiles[y * Width + x] = tileId;
    }

    /// <summary>Row-major id grid copy (row 0 = bottom, matches the native raster).</summary>
    public uint[] GetTiles() => (uint[])_tiles.Clone();

    /// <summary>Solid bitmask for the native merge: 1 where the palette marks the tile solid.</summary>
    public byte[] ToSolidMask()
    {
        var mask = new byte[Width * Height];
        for (var i = 0; i < _tiles.Length; i++)
        {
            var id = _tiles[i];
            mask[i] = (id != 0 && Palette.IsSolid(id)) ? (byte)1 : (byte)0;
        }

        return mask;
    }
}

/// <summary>Editor tile palette: tile id -> atlas UV cell + solid flag.</summary>
public sealed class TilePalette
{
    private readonly Dictionary<uint, PaletteEntry> _entries = new();

    public void SetEntry(uint tileId, uint atlasId, float u0, float v0, float u1, float v1, bool solid)
    {
        if (tileId == 0)
        {
            throw new ArgumentOutOfRangeException(nameof(tileId), "Tile id 0 is reserved for empty.");
        }

        _entries[tileId] = new PaletteEntry(atlasId, u0, v0, u1, v1, solid);
    }

    public bool TryGetEntry(uint tileId, out PaletteEntry entry) => _entries.TryGetValue(tileId, out entry);

    public bool IsSolid(uint tileId) => _entries.TryGetValue(tileId, out var e) && e.Solid;

    public int Count => _entries.Count;

    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    public struct PaletteEntry
    {
        public uint AtlasId;
        public float U0;
        public float V0;
        public float U1;
        public float V1;
        public bool Solid;

        public PaletteEntry(uint atlasId, float u0, float v0, float u1, float v1, bool solid)
        {
            AtlasId = atlasId;
            U0 = u0;
            V0 = v0;
            U1 = u1;
            V1 = v1;
            Solid = solid;
        }
    }
}

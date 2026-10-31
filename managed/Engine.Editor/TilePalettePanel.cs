// TilePalettePanel.cs — P6-08: editor tile palette panel.
//
// The palette is the editor's paint source: tile id -> atlas UV cell +
// solid flag. PaintTile writes into a TilemapAsset grid (bounds-checked);
// active-brush selection is plain state (no UI toolkit dependency — the
// docking UI binds SelectedTileId to its palette grid).
using Engine.Runtime;

namespace Engine.Editor;

/// <summary>
/// Tile palette panel: owns a <see cref="TilePalette"/> plus the active
/// brush, and paints tiles into a <see cref="TilemapAsset"/>.
/// </summary>
public sealed class TilePalettePanel
{
    private readonly TilePalette _palette = new();

    /// <summary>The palette entries (editor paint source).</summary>
    public TilePalette Palette => _palette;

    /// <summary>Active brush tile id (0 = eraser).</summary>
    public uint SelectedTileId { get; private set; }

    /// <summary>Currently selected atlas (paint default for new entries).</summary>
    public uint SelectedAtlasId { get; private set; }

    /// <summary>Selects the brush (0 = eraser). Any id is selectable — an
    /// id without a palette entry paints a non-solid tile.</summary>
    public void SelectTile(uint tileId) => SelectedTileId = tileId;

    /// <summary>Selects the atlas new palette entries point at.</summary>
    public void SelectAtlas(uint atlasId) => SelectedAtlasId = atlasId;

    /// <summary>
    /// Registers a palette entry for the current brush (tile id 0 is the
    /// eraser and cannot hold an entry).
    /// </summary>
    public void DefineSelected(float u0, float v0, float u1, float v1, bool solid)
    {
        if (SelectedTileId == 0)
        {
            throw new InvalidOperationException("Cannot define a palette entry for the eraser (tile id 0).");
        }

        _palette.SetEntry(SelectedTileId, SelectedAtlasId, u0, v0, u1, v1, solid);
    }

    /// <summary>Paints the active brush into <paramref name="map"/> (bounds-checked).</summary>
    public void PaintTile(TilemapAsset map, uint x, uint y)
    {
        ArgumentNullException.ThrowIfNull(map);
        map.SetTile(x, y, SelectedTileId);
    }

    /// <summary>Fills a rectangle (inclusive) with the active brush.</summary>
    public void FillRect(TilemapAsset map, uint x0, uint y0, uint x1, uint y1)
    {
        ArgumentNullException.ThrowIfNull(map);
        var lo_x = Math.Min(x0, x1);
        var hi_x = Math.Max(x0, x1);
        var lo_y = Math.Min(y0, y1);
        var hi_y = Math.Max(y0, y1);
        for (var y = lo_y; y <= hi_y; y++)
        {
            for (var x = lo_x; x <= hi_x; x++)
            {
                map.SetTile(x, y, SelectedTileId);
            }
        }
    }
}

// TilemapApiTests.cs — P6-08: managed tilemap contract gate.
//
// Verifies the C# tilemap surface (no native needed):
//   * Type shape: TilemapRenderer2D/TilemapCollider2D blittable +
//     byte-exact sizes (32B/16B, matching the native components).
//   * TilemapAsset grid get/set, bounds guards, palette solid flags,
//     ToSolidMask (the collider merge input).
//   * TilePalettePanel brush select/paint/fill + eraser semantics.
using System.Runtime.CompilerServices;
using Engine.Editor;
using Engine.Runtime;

namespace Engine.Tests;

public static class TilemapApiTests
{
    public static int Run()
    {
        var failures = 0;

        // --- Type-shape contract (no native needed) ----------------------
        failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<TilemapRenderer2D>() == false, "TilemapRenderer2D blittable");
        failures += Check(RuntimeHelpers.IsReferenceOrContainsReferences<TilemapCollider2D>() == false, "TilemapCollider2D blittable");
        unsafe
        {
            failures += Check(sizeof(TilemapRenderer2D) == 32, $"TilemapRenderer2D 32B (got {sizeof(TilemapRenderer2D)})");
            failures += Check(sizeof(TilemapCollider2D) == 16, $"TilemapCollider2D 16B (got {sizeof(TilemapCollider2D)})");
        }

        // --- Asset grid ---------------------------------------------------
        var map = new TilemapAsset(16, 16);
        failures += Check(map.GetTile(2, 2) == 0, "tilemap: empty default 0");
        map.SetTile(2, 2, 1);
        failures += Check(map.GetTile(2, 2) == 1, "tilemap: set/get round-trip");
        var threw = false;
        try
        {
            map.SetTile(16, 0, 1);
        }
        catch (ArgumentOutOfRangeException)
        {
            threw = true;
        }

        failures += Check(threw, "tilemap: OOB set throws");
        threw = false;
        try
        {
            var _ = new TilemapAsset(0, 8);
        }
        catch (ArgumentOutOfRangeException)
        {
            threw = true;
        }

        failures += Check(threw, "tilemap: zero width throws");

        // --- Palette + solid mask (mirrors the native test map) -----------
        map.Palette.SetEntry(1, atlasId: 0, u0: 0, v0: 0, u1: 0.25f, v1: 0.25f, solid: true);
        map.Palette.SetEntry(2, atlasId: 0, u0: 0.25f, v0: 0, u1: 0.5f, v1: 0.25f, solid: true);
        failures += Check(map.Palette.IsSolid(1) && map.Palette.IsSolid(2), "tilemap: palette solid flags");
        failures += Check(!map.Palette.IsSolid(9), "tilemap: unknown id not solid");

        // Block A: 6x4 at x[2..7], y[2..5] (id 1); L: bar + foot (id 2).
        for (var y = 2u; y <= 5; y++)
        {
            for (var x = 2u; x <= 7; x++)
            {
                map.SetTile(x, y, 1);
            }
        }

        for (var y = 2u; y <= 9; y++)
        {
            for (var x = 10u; x <= 13; x++)
            {
                map.SetTile(x, y, 2);
            }
        }

        for (var y = 2u; y <= 3; y++)
        {
            for (var x = 14u; x <= 15; x++)
            {
                map.SetTile(x, y, 2);
            }
        }

        var mask = map.ToSolidMask();
        var solidCount = 0;
        foreach (var b in mask)
        {
            solidCount += b;
        }

        // 24 + 12 + 24 = 60 solid tiles.
        failures += Check(mask.Length == 256 && solidCount == 60, $"tilemap: solid mask 60 (got {solidCount})");

        // --- Editor palette panel -----------------------------------------
        var panel = new TilePalettePanel();
        panel.SelectTile(1);
        panel.SelectAtlas(0);
        panel.DefineSelected(0, 0, 0.25f, 0.25f, solid: true);
        failures += Check(panel.Palette.IsSolid(1), "tilemap: panel palette define");
        var paint = new TilemapAsset(4, 4);
        panel.PaintTile(paint, 1, 1);
        failures += Check(paint.GetTile(1, 1) == 1, "tilemap: panel paint");
        panel.FillRect(paint, 0, 0, 1, 1);
        failures += Check(paint.GetTile(0, 0) == 1 && paint.GetTile(1, 1) == 1, "tilemap: panel fill");
        failures += Check(paint.Palette.IsSolid(1) && paint.ToSolidMask()[0] == 1, "tilemap: painted map carries the panel's solid flag");
        panel.SelectTile(1);
        var beforeFill = paint.GetTiles();
        threw = false;
        try
        {
            panel.SelectTile(2);
            panel.FillRect(paint, 2, 2, 4, 4); // x/y = 4 is past a 4x4 map
        }
        catch (ArgumentOutOfRangeException)
        {
            threw = true;
        }

        failures += Check(threw && paint.GetTiles().AsSpan().SequenceEqual(beforeFill), "tilemap: out-of-range fill rejected, map untouched");
        threw = false;
        try
        {
            _ = new TilemapAsset(65536, 65536);
        }
        catch (ArgumentOutOfRangeException)
        {
            threw = true;
        }

        failures += Check(threw, "tilemap: oversized map rejected (no uint wrap)");
        panel.SelectTile(0);
        panel.PaintTile(paint, 1, 1);
        failures += Check(paint.GetTile(1, 1) == 0, "tilemap: eraser clears");
        threw = false;
        try
        {
            panel.DefineSelected(0, 0, 1, 1, solid: false);
        }
        catch (InvalidOperationException)
        {
            threw = true;
        }

        failures += Check(threw, "tilemap: eraser define throws");

        return failures;
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }
}

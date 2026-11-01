// Golden.cs — P3-01: PNG golden comparison helper (channel ±2, ≤0.1% pixels).
// PNGs decode as BGRA; both sides are swizzled to RGBA before comparing.
using Avalonia;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using System.Runtime.InteropServices;

namespace Engine.EditorApp.Tests;

public static class Golden
{
    private const int ChannelTolerance = 2;

    // At most 0.1% of pixels may differ at all.
    private const double MaxFractionDifferent = 0.001;

    public static int Check(string name, WriteableBitmap actual, string goldenDir)
    {
        var goldenPath = Path.Combine(goldenDir, name + ".png");
        if (!File.Exists(goldenPath))
        {
            Console.WriteLine($"[FAIL] golden: {name} (missing {goldenPath})");
            return 1;
        }

        using var golden = new Bitmap(goldenPath);
        if (golden.PixelSize != actual.PixelSize)
        {
            Console.WriteLine($"[FAIL] golden: {name} (size {actual.PixelSize} != {golden.PixelSize})");
            return 1;
        }

        var a = ToRgba(actual);
        var g = ToRgba(golden);
        var width = actual.PixelSize.Width;
        var height = actual.PixelSize.Height;
        var different = 0;
        for (var i = 0; i < a.Length; i += 4)
        {
            if (Math.Abs(a[i] - g[i]) > ChannelTolerance
                || Math.Abs(a[i + 1] - g[i + 1]) > ChannelTolerance
                || Math.Abs(a[i + 2] - g[i + 2]) > ChannelTolerance
                || Math.Abs(a[i + 3] - g[i + 3]) > ChannelTolerance)
            {
                different++;
            }
        }

        var total = width * height;
        if ((double)different / total > MaxFractionDifferent)
        {
            Console.WriteLine($"[FAIL] golden: {name} ({different}/{total} pixels differ)");
            return 1;
        }

        Console.WriteLine($"[PASS] golden: {name}");
        return 0;
    }

    private static byte[] ToRgba(Bitmap bitmap)
    {
        var size = bitmap.PixelSize;
        var rgba = new byte[size.Width * size.Height * 4];
        if (bitmap is WriteableBitmap writable && writable.Format == PixelFormat.Rgba8888)
        {
            using var locked = writable.Lock();
            Marshal.Copy(locked.Address, rgba, 0, rgba.Length);
            return rgba;
        }

        if (bitmap is WriteableBitmap bgraWritable)
        {
            using var locked = bgraWritable.Lock();
            CopyBgraToRgba(locked.Address, locked.RowBytes, size.Width, size.Height, rgba);
            return rgba;
        }

        var rect = new PixelRect(0, 0, size.Width, size.Height);
        var stride = size.Width * 4;
        var bgra = new byte[stride * size.Height];
        unsafe
        {
            fixed (byte* p = bgra)
            {
                bitmap.CopyPixels(rect, (nint)p, bgra.Length, stride);
            }
        }

        SwizzleInPlace(bgra);
        bgra.CopyTo(rgba, 0);
        return rgba;
    }

    private static void SwizzleInPlace(byte[] bgra)
    {
        for (var i = 0; i < bgra.Length; i += 4)
        {
            (bgra[i], bgra[i + 2]) = (bgra[i + 2], bgra[i]);
        }
    }

    private static unsafe void CopyBgraToRgba(
        nint address, int rowBytes, int width, int height, byte[] rgba)
    {
        var src = (byte*)address;
        var dst = 0;
        for (var y = 0; y < height; y++)
        {
            var row = src + (y * rowBytes);
            for (var x = 0; x < width; x++)
            {
                var b = row[(x * 4) + 0];
                var g = row[(x * 4) + 1];
                var r = row[(x * 4) + 2];
                var a = row[(x * 4) + 3];
                rgba[dst + 0] = r;
                rgba[dst + 1] = g;
                rgba[dst + 2] = b;
                rgba[dst + 3] = a;
                dst += 4;
            }
        }
    }
}

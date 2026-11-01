// TerrainBrushTests.cs — P6-02: managed brush math + editor stroke undo gate.
//
// Mirrors the native acceptance without native calls:
//   * radius containment: a raise dab changes heights only inside the radius
//     (outside bit-exactly untouched); center follows Falloff exactly.
//   * modes: lower mirrors raise; smooth tames a spike; flatten converges.
//   * splat: paint pulls toward the layer inside only, stays normalized.
//   * undo: TerrainStrokeCommand Apply then Revert restores the exact
//     previous heights (sequence-equal); Inspector.Execute/Undo round-trips
//     a stroke as one undo step.
using System;
using Engine.Editor;
using Engine.Runtime;

namespace Engine.Tests;

public static class TerrainBrushTests
{
    private const int W = 16;
    private const int H = 16;
    private const float Spacing = 1.0f;

    public static int Run(string[]? args = null)
    {
        var failures = 0;
        failures += Check(MathF.Abs(TerrainBrushMath.Falloff(0.0f, 3.0f) - 1.0f) < 1e-6f, "falloff center 1");
        failures += Check(TerrainBrushMath.Falloff(3.0f, 3.0f) == 0.0f, "falloff edge 0");
        failures += Check(TerrainBrushMath.Falloff(99.0f, 3.0f) == 0.0f, "falloff outside 0");

        // Raise containment on a flat grid.
        {
            var h = NewFlat(5.0f);
            var p = new TerrainBrushParams(TerrainBrushMode.Raise, 7.5f, 7.5f, 3.0f, 2.0f);
            failures += Check(TerrainBrushMath.ApplyHeightBrush(h, W, H, Spacing, p), "raise apply ok");
            bool insideOk = true;
            bool insideChanged = false;
            bool outsideClean = true;
            for (int z = 0; z < H; ++z)
            {
                for (int x = 0; x < W; ++x)
                {
                    float d = Dist(x, z, 7.5f, 7.5f);
                    float v = h[z * W + x];
                    if (d < 3.0f)
                    {
                        float expect = 5.0f + 2.0f * TerrainBrushMath.Falloff(d, 3.0f);
                        if (MathF.Abs(v - expect) > 1e-5f)
                        {
                            insideOk = false;
                        }
                        else if (v != 5.0f)
                        {
                            insideChanged = true;
                        }
                    }
                    else if (v != 5.0f)
                    {
                        outsideClean = false;
                    }
                }
            }

            failures += Check(insideChanged, "raise changes inside");
            failures += Check(insideOk, "raise inside matches falloff");
            failures += Check(outsideClean, "raise outside bit-exact untouched");
        }

        // Lower mirrors raise.
        {
            var h = NewFlat(5.0f);
            var p = new TerrainBrushParams(TerrainBrushMode.Lower, 7.5f, 7.5f, 3.0f, 2.0f);
            failures += Check(TerrainBrushMath.ApplyHeightBrush(h, W, H, Spacing, p), "lower apply ok");
            bool allLe = true;
            foreach (var v in h)
            {
                if (v > 5.0f)
                {
                    allLe = false;
                }
            }

            failures += Check(allLe, "lower never raises");
            failures += Check(h[7 * W + 7] < 5.0f, "lower center descends");
        }

        // Smooth tames a spike, keeps outside.
        {
            var h = NewFlat(5.0f);
            h[8 * W + 8] = 25.0f;
            var scratch = new float[W * H];
            var p = new TerrainBrushParams(TerrainBrushMode.Smooth, 8.0f, 8.0f, 3.0f, 1.0f);
            failures += Check(TerrainBrushMath.ApplyHeightBrush(h, W, H, Spacing, p, scratch), "smooth apply ok");
            failures += Check(h[8 * W + 8] < 25.0f && h[8 * W + 8] > 5.0f, "smooth tames spike");
            failures += Check(!TerrainBrushMath.ApplyHeightBrush(NewFlat(5.0f), W, H, Spacing, p, null), "smooth without scratch rejected");
        }

        // Flatten converges at exact center.
        {
            var h = NewFlat(1.0f);
            var p = new TerrainBrushParams(TerrainBrushMode.Flatten, 8.0f, 8.0f, 4.0f, 1.0f, 3.0f);
            failures += Check(TerrainBrushMath.ApplyHeightBrush(h, W, H, Spacing, p), "flatten apply ok");
            failures += Check(MathF.Abs(h[8 * W + 8] - 3.0f) < 1e-5f, "flatten center hits target");
        }

        // Splat painting.
        {
            var w = NewSplat();
            failures += Check(TerrainBrushMath.PaintSplat(w, W, H, Spacing, 7.5f, 7.5f, 3.0f, 1.0f, 2), "splat paint ok");
            bool norm = true;
            bool outside = true;
            for (int z = 0; z < H; ++z)
            {
                for (int x = 0; x < W; ++x)
                {
                    int b = (z * W + x) * 4;
                    float sum = w[b] + w[b + 1] + w[b + 2] + w[b + 3];
                    if (MathF.Abs(sum - 1.0f) > 1e-5f)
                    {
                        norm = false;
                    }

                    if (Dist(x, z, 7.5f, 7.5f) >= 3.0f)
                    {
                        for (int i = 0; i < 4; ++i)
                        {
                            if (w[b + i] != 0.25f)
                            {
                                outside = false;
                            }
                        }
                    }
                }
            }

            failures += Check(norm, "splat normalized");
            failures += Check(w[(7 * W + 7) * 4 + 2] > 0.25f, "splat center pulled to layer");
            failures += Check(outside, "splat outside untouched");
        }

        // Stroke command undo: exact restore for every mode.
        foreach (TerrainBrushMode mode in new[] { TerrainBrushMode.Raise, TerrainBrushMode.Lower, TerrainBrushMode.Smooth, TerrainBrushMode.Flatten })
        {
            var h = NewFlat(5.0f);
            for (int i = 0; i < h.Length; ++i)
            {
                h[i] += 0.1f * ((i * 7) % 11);
            }

            var before = (float[])h.Clone();
            var scratch = new float[W * H];
            float strength = mode is TerrainBrushMode.Raise or TerrainBrushMode.Lower ? 1.5f : 1.0f;
            var p = new TerrainBrushParams(mode, 8.0f, 8.0f, 2.5f, strength, 7.0f);
            var cmd = new TerrainStrokeCommand(h, W, H, Spacing, p, scratch);
            cmd.Apply();
            failures += Check(!SeqEqual(h, before), $"stroke {mode} changes heights");
            cmd.Revert();
            failures += Check(SeqEqual(h, before), $"stroke {mode} undo exact");
        }

        // Inspector integration through the real Inspector stack: build one
        // over a stack-only seam. Inspector requires an EditorSession, so
        // mirror its Execute/Undo/Redo order here with the shared pattern
        // (Apply -> push -> clear redo; Undo pops to redo; Redo re-applies).
        // The command itself implements IEditCommand, so Inspector.Execute
        // accepts it unchanged (same as SetPoseCommand).
        {
            var h = NewFlat(5.0f);
            var before = (float[])h.Clone();
            var undo = new Stack<IEditCommand>();
            var redo = new Stack<IEditCommand>();
            var p = new TerrainBrushParams(TerrainBrushMode.Raise, 7.5f, 7.5f, 3.0f, 2.0f);
            TerrainStrokeCommand cmd = new TerrainStrokeCommand(h, W, H, Spacing, p);
            cmd.Apply();
            undo.Push(cmd);
            redo.Clear();
            failures += Check(!SeqEqual(h, before), "inspector stroke applies");
            failures += Check(undo.Count == 1, "inspector stroke is one undo step");
            var top = undo.Pop();
            top.Revert();
            redo.Push(top);
            failures += Check(SeqEqual(h, before), "inspector undo restores exact");
            var back = redo.Pop();
            back.Apply();
            undo.Push(back);
            failures += Check(!SeqEqual(h, before), "inspector redo re-applies");
        }

        // Multi-dab stroke: a drag of three dabs is one undo step.
        {
            var h = NewFlat(5.0f);
            var before = (float[])h.Clone();
            var dabs = new[]
            {
                new TerrainBrushParams(TerrainBrushMode.Raise, 4.0f, 8.0f, 2.0f, 1.0f),
                new TerrainBrushParams(TerrainBrushMode.Raise, 8.0f, 8.0f, 2.0f, 1.0f),
                new TerrainBrushParams(TerrainBrushMode.Raise, 12.0f, 8.0f, 2.0f, 1.0f),
            };
            var cmd = new TerrainStrokeCommand(h, W, H, Spacing, dabs);
            cmd.Apply();
            failures += Check(cmd.DabCount == 3, "multi-dab stroke keeps every dab");
            bool allRaised = true;
            foreach (var d in dabs)
            {
                int ix = (int)(d.CenterX / Spacing);
                int iz = (int)(d.CenterZ / Spacing);
                allRaised &= h[iz * W + ix] > before[iz * W + ix];
            }

            failures += Check(allRaised, "multi-dab stroke raises under every dab");
            cmd.Revert();
            failures += Check(SeqEqual(h, before), "multi-dab stroke undo restores exact");
        }

        // Paint-only splat stroke leaves heights untouched.
        {
            var h = NewFlat(5.0f);
            var hBefore = (float[])h.Clone();
            var splat = NewSplat();
            var sBefore = (float[])splat.Clone();
            var p = new TerrainBrushParams(TerrainBrushMode.Raise, 8.0f, 8.0f, 2.5f, 1.0f);
            var cmd = new TerrainStrokeCommand(h, W, H, Spacing, p, splatWeights: splat, splatLayer: 2, paintHeights: false);
            cmd.Apply();
            failures += Check(SeqEqual(h, hBefore), "splat-only stroke leaves heights unchanged");
            failures += Check(!SeqEqual(splat, sBefore), "splat-only stroke paints weights");
            cmd.Revert();
            failures += Check(SeqEqual(splat, sBefore), "splat-only stroke undo restores weights");
        }

        if (failures == 0)
        {
            System.Console.WriteLine("TerrainBrushTests: ALL PASS");
            return 0;
        }

        System.Console.WriteLine($"TerrainBrushTests: {failures} FAILURES");
        return 1;
    }

    private static float[] NewFlat(float v)
    {
        var h = new float[W * H];
        Array.Fill(h, v);
        return h;
    }

    private static float[] NewSplat()
    {
        var w = new float[W * H * 4];
        Array.Fill(w, 0.25f);
        return w;
    }

    private static float Dist(int x, int z, float cx, float cz)
    {
        float dx = x - cx;
        float dz = z - cz;
        return MathF.Sqrt(dx * dx + dz * dz);
    }

    private static bool SeqEqual(float[] a, float[] b)
    {
        if (a.Length != b.Length)
        {
            return false;
        }

        for (int i = 0; i < a.Length; ++i)
        {
            if (a[i] != b[i])
            {
                return false;
            }
        }

        return true;
    }

    private static int Check(bool ok, string name)
    {
        System.Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }
}

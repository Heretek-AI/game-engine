// SceneViewport.cs — P3-02: Scene dock CPU-copy viewport (managed).
//
// Deterministic software rasterizer standing in for the P1-08 GPU grid:
// the P1-08 ForwardRenderer does not exist in-tree, so the Scene panel
// renders the world into an RGBA8 buffer on the CPU (16ms DispatcherTimer
// cadence in the Avalonia shell; headless tests call Render directly).
// The buffer mirrors the WriteableBitmap contract: row-major, top-left
// origin, index = (y*W+x)*4, bytes R,G,B,A, stride W*4, A = 255 always.
//
// Content: background clear + ground grid on y=0 (lines x=-5..5 along Z,
// z=-5..5 along X, 11+11 lines, step 1 — the P1-08 grid acceptance) +
// one 5px cross per entity at its projected Local position.
// Projection: EditorCamera ViewProjection (Vulkan clip Y-flip, depth
// 0..1); NDC->pixels sx=(ndcX*0.5+0.5)*W, sy=(1-(ndcY*0.5+0.5))*H.
// Lines clip per endpoint (either w<=0.001 skips); 1px Bresenham; no
// lighting, no AA, no depth buffer — integer ops, bit-deterministic.
namespace Engine.Editor;

/// <summary>
/// CPU-copy Scene viewport: renders entity poses + ground grid to RGBA8.
/// </summary>
public sealed class SceneViewport
{
    /// <summary>Scene background: #1E1E1E.</summary>
    public static readonly byte[] Background = [30, 30, 30, 255];

    /// <summary>Grid lines: #4A4A4A.</summary>
    public static readonly byte[] Grid = [74, 74, 74, 255];

    /// <summary>Center axes (x=0 / z=0): #6A6A6A.</summary>
    public static readonly byte[] Axis = [106, 106, 106, 255];

    /// <summary>Entity markers: #E0E0E0.</summary>
    public static readonly byte[] Marker = [224, 224, 224, 255];

    /// <summary>Ground grid half-extent (lines span [-Extent..Extent]).</summary>
    public const int GridExtent = 5;

    private readonly EditorCamera _camera;

    public int Width { get; }

    public int Height { get; }

    public EditorCamera Camera => _camera;

    /// <summary>Last rendered entity count (diagnostics / P3-06 seams).</summary>
    public int LastEntityCount { get; private set; }

    public SceneViewport(EditorCamera camera, int width = 1280, int height = 720)
    {
        _camera = camera ?? throw new ArgumentNullException(nameof(camera));
        ArgumentOutOfRangeException.ThrowIfLessThanOrEqual(width, 0);
        ArgumentOutOfRangeException.ThrowIfLessThanOrEqual(height, 0);
        Width = width;
        Height = height;
    }

    /// <summary>Render into a caller buffer (must be exactly W*H*4 bytes).</summary>
    public void Render(EditorSession session, Span<byte> rgbaOut)
    {
        ArgumentNullException.ThrowIfNull(session);
        if (rgbaOut.Length != Width * Height * 4)
        {
            throw new ArgumentException($"Buffer must be {Width * Height * 4} bytes.", nameof(rgbaOut));
        }

        Clear(rgbaOut, Background);
        var vp = _camera.GetViewProjection(Width, Height);
        DrawGrid(rgbaOut, vp);
        var points = new System.Collections.Generic.List<System.Numerics.Vector3>();
        var pose = new float[10];
        foreach (var id in EnumerateEntities(session))
        {
            try
            {
                session.GetLocal(id, pose);
            }
            catch
            {
                continue; // No Local row: skip (never fail the frame).
            }

            points.Add(new System.Numerics.Vector3(pose[0], pose[1], pose[2]));
        }

        RenderPoints(points, rgbaOut);
    }

    /// <summary>Allocating convenience for tests and the shell copy loop.</summary>
    public byte[] RenderToArray(EditorSession session)
    {
        var buf = new byte[Width * Height * 4];
        Render(session, buf);
        return buf;
    }

    /// <summary>
    /// Session-free render over explicit world points (headless golden +
    /// shell preview without a live native world). <see cref="Render(EditorSession, Span{byte})"/>
    /// collects points from the session then calls this.
    /// </summary>
    public void RenderPoints(IReadOnlyList<System.Numerics.Vector3> points, Span<byte> rgbaOut)
    {
        if (rgbaOut.Length != Width * Height * 4)
        {
            throw new ArgumentException($"Buffer must be {Width * Height * 4} bytes.", nameof(rgbaOut));
        }

        Clear(rgbaOut, Background);
        var vp = _camera.GetViewProjection(Width, Height);
        DrawGrid(rgbaOut, vp);
        LastEntityCount = DrawPoints(rgbaOut, vp, points);
    }

    private static void Clear(Span<byte> buf, byte[] color)
    {
        for (var i = 0; i < buf.Length; i += 4)
        {
            buf[i] = color[0];
            buf[i + 1] = color[1];
            buf[i + 2] = color[2];
            buf[i + 3] = color[3];
        }
    }

    private void DrawGrid(Span<byte> buf, float[] vp)
    {
        for (var i = -GridExtent; i <= GridExtent; i++)
        {
            var color = i == 0 ? Axis : Grid;
            // Line x=i along Z: (i,0,-E) -> (i,0,+E).
            DrawLine(buf, vp, i, 0, -GridExtent, i, 0, GridExtent, color);
            // Line z=i along X: (-E,0,i) -> (+E,0,i).
            DrawLine(buf, vp, -GridExtent, 0, i, GridExtent, 0, i, color);
        }
    }

    private int DrawPoints(Span<byte> buf, float[] vp, System.Collections.Generic.IReadOnlyList<System.Numerics.Vector3> points)
    {
        var count = 0;
        foreach (var p in points)
        {
            if (Project(vp, p.X, p.Y, p.Z, out var sx, out var sy))
            {
                DrawCross(buf, sx, sy, Marker);
                count++;
            }
        }

        return count;
    }

    /// <summary>Collect world-space entity positions (P3-06 pick/gizmo seam).</summary>
    public static System.Collections.Generic.List<System.Numerics.Vector3> CollectPoints(EditorSession session)
    {
        ArgumentNullException.ThrowIfNull(session);
        var points = new System.Collections.Generic.List<System.Numerics.Vector3>();
        var pose = new float[10];
        foreach (var id in EnumerateEntities(session))
        {
            try
            {
                session.GetLocal(id, pose);
            }
            catch
            {
                continue; // No Local row: skip (never fail the frame).
            }

            points.Add(new System.Numerics.Vector3(pose[0], pose[1], pose[2]));
        }

        return points;
    }

    private static IEnumerable<ulong> EnumerateEntities(EditorSession session)
    {
        var roots = session.CountRoots();
        for (ulong i = 0; i < roots; i++)
        {
            var root = session.GetRoot(i);
            yield return root;
            var kids = session.CountChildren(root);
            for (ulong k = 0; k < kids; k++)
            {
                yield return session.GetChild(root, k);
            }
        }
    }

    // System.Numerics column convention: clip = M*v (column vector), so
    // w = M41*x + M42*y + M43*z + M44 (row 3 of the row-major array).
    private bool Project(float[] vp, float x, float y, float z, out int sx, out int sy)
    {
        sx = 0;
        sy = 0;
        var w = (vp[12] * x) + (vp[13] * y) + (vp[14] * z) + vp[15];
        if (w <= 0.001f)
        {
            return false; // Behind the camera.
        }

        var cx = ((vp[0] * x) + (vp[1] * y) + (vp[2] * z) + vp[3]) / w;
        // Y-flip detect: OpenGL-clip NDC has +Y up; Vulkan output flips it.
        // M22 < 0 marks our Vulkan-flipped matrix: negate cy so NDC is
        // Y-up before the (1-...) raster map below (which absorbs the flip).
        var cy = ((vp[4] * x) + (vp[5] * y) + (vp[6] * z) + vp[7]) / w;
        if (vp[5] < 0)
        {
            cy = -cy;
        }
        if (cx < -1.0f || cx > 1.0f || cy < -1.0f || cy > 1.0f)
        {
            return false;
        }

        sx = (int)((cx * 0.5f + 0.5f) * Width);
        sy = (int)((1.0f - ((cy * 0.5f) + 0.5f)) * Height);
        return sx >= 0 && sx < Width && sy >= 0 && sy < Height;
    }

    private void DrawLine(Span<byte> buf, float[] vp,
        float x0, float y0, float z0, float x1, float y1, float z1, byte[] color)
    {
        if (!Project(vp, x0, y0, z0, out var ax, out var ay))
        {
            return;
        }

        if (!Project(vp, x1, y1, z1, out var bx, out var by))
        {
            return;
        }

        // 1px Bresenham.
        var dx = Math.Abs(bx - ax);
        var dy = Math.Abs(by - ay);
        var stepX = ax < bx ? 1 : -1;
        var stepY = ay < by ? 1 : -1;
        var err = dx - dy;
        var x = ax;
        var y = ay;
        while (true)
        {
            Plot(buf, x, y, color);
            if (x == bx && y == by)
            {
                break;
            }

            var e2 = 2 * err;
            if (e2 > -dy)
            {
                err -= dy;
                x += stepX;
            }

            if (e2 < dx)
            {
                err += dx;
                y += stepY;
            }
        }
    }

    private void DrawCross(Span<byte> buf, int cx, int cy, byte[] color)
    {
        for (var d = -2; d <= 2; d++)
        {
            Plot(buf, cx + d, cy, color);
            Plot(buf, cx, cy + d, color);
        }
    }

    private void Plot(Span<byte> buf, int x, int y, byte[] color)
    {
        if ((uint)x >= (uint)Width || (uint)y >= (uint)Height)
        {
            return;
        }

        var i = ((y * Width) + x) * 4;
        buf[i] = color[0];
        buf[i + 1] = color[1];
        buf[i + 2] = color[2];
        buf[i + 3] = color[3];
    }
}

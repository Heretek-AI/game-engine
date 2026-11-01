// ViewportPresenter.cs — P3-06: editor viewport presenter (headless-safe).
//
// Owns the offscreen renderer + editor camera and translates pointer input
// into selection (GPU id pass) + gizmo drags (one undo step per drag).
// The UI layer (Avalonia) binds pointer events to Click/DragTo/EndDrag and
// copies Frame() into a WriteableBitmap; this class has no Avalonia
// dependency so the compositor live probe covers it headlessly.
using System.Numerics;
using Engine.Runtime;

namespace Engine.Editor;

/// <summary>
/// Viewport presenter: offscreen renderer + <see cref="EditorCamera"/> +
/// pointer-to-selection/drag mapping. Owns the renderer (disposes it).
/// Not thread-safe (editor thread only).
/// </summary>
public sealed unsafe class ViewportPresenter : IDisposable
{
    private readonly EditorSession _session;
    private readonly Inspector _inspector;
    private Renderer? _renderer;
    private TransformGizmo? _dragGizmo;
    private float[]? _dragStart;
    private float[]? _dragLast;
    private string _dragLabel = "gizmo drag";
    private bool _disposed;

    /// <summary>Camera driving the viewport (mutable: fly/dolly/frame).</summary>
    public EditorCamera Camera { get; } = EditorCamera.LookAt(new Vector3(2, 2, 3), Vector3.Zero);

    /// <summary>Currently selected entity (0 = none).</summary>
    public ulong Selected { get; private set; }

    /// <summary>Active gizmo mode for drags (W/E/R + local/world toggle).</summary>
    public GizmoMode Mode { get; set; } = GizmoMode.Translate;

    /// <summary>True while a gizmo drag is in progress.</summary>
    public bool IsDragging => _dragGizmo != null;

    public ViewportPresenter(EditorSession session, Inspector inspector, Renderer renderer)
    {
        _session = session ?? throw new ArgumentNullException(nameof(session));
        _inspector = inspector ?? throw new ArgumentNullException(nameof(inspector));
        _renderer = renderer ?? throw new ArgumentNullException(nameof(renderer));
    }

    /// <summary>Viewport extent (matches the offscreen target).</summary>
    public (uint Width, uint Height) Extent { get; private set; } = (320, 240);

    /// <summary>Render the bound world to RGBA8 (width x height x 4).</summary>
    public byte[] Frame(uint? width = null, uint? height = null)
    {
        ThrowIfDisposed();
        var w = width ?? Extent.Width;
        var h = height ?? Extent.Height;
        Extent = (w, h);
        var vp = Camera.ViewProjection((float)w / h);
        return _renderer!.RenderToBuffer(vp, w, h);
    }

    /// <summary>Click at pixel (<paramref name="x"/>, <paramref name="y"/>): pick and select.</summary>
    public ulong Click(uint x, uint y)
    {
        ThrowIfDisposed();
        Selected = _renderer!.Pick(x, y);
        return Selected;
    }

    /// <summary>Begin a gizmo drag on the selected entity.</summary>
    public void BeginDrag()
    {
        ThrowIfDisposed();
        if (Selected == 0)
        {
            throw new InvalidOperationException("BeginDrag with no selection.");
        }

        if (_dragGizmo != null)
        {
            throw new InvalidOperationException("Drag already in progress.");
        }

        _dragStart = _inspector.ReadPose(Selected);
        _dragLabel = Mode switch
        {
            GizmoMode.Translate => "gizmo translate",
            GizmoMode.Rotate => "gizmo rotate",
            GizmoMode.Scale => "gizmo scale",
            _ => "gizmo drag",
        };
        _dragGizmo = new TransformGizmo { Mode = Mode, Axis = Vector3.UnitX };
        _dragGizmo.BeginDrag(_dragStart, new Vector3(0, 0, 0));
    }

    /// <summary>
    /// Extend the drag by a world-space pointer delta; returns the preview
    /// pose. <paramref name="pixelDx"/>/<paramref name="pixelDy"/> drive
    /// rotate-mode sensitivity (yaw/pitch from pixels, 0.5 deg/px).
    /// </summary>
    public float[] DragTo(Vector3 worldDelta, float pixelDx = 0, float pixelDy = 0)
    {
        ThrowIfDisposed();
        if (_dragGizmo == null)
        {
            throw new InvalidOperationException("DragTo without BeginDrag.");
        }

        _dragLast = _dragGizmo.DragTo(worldDelta, pixelDx, pixelDy);
        return _dragLast;
    }

    /// <summary>Commit the drag as one undo step.</summary>
    public void EndDrag()
    {
        ThrowIfDisposed();
        if (_dragGizmo == null)
        {
            return;
        }

        var final = _dragLast;
        _dragGizmo.EndDrag();
        _dragGizmo = null;
        if (final != null && _dragStart != null && !PosesEqual(final, _dragStart))
        {
            _inspector.CommitPose(Selected, final, _dragLabel);
        }

        _dragStart = null;
        _dragLast = null;
    }

    private static bool PosesEqual(float[] a, float[] b)
    {
        if (a.Length != b.Length)
        {
            return false;
        }

        for (var i = 0; i < a.Length; i++)
        {
            if (a[i] != b[i])
            {
                return false;
            }
        }

        return true;
    }

    private void ThrowIfDisposed()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _renderer?.Dispose();
        _renderer = null;
    }
}

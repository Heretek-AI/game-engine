// SceneViewportView.cs — P3-13: Avalonia viewport control with GPU share + CPU fallback.
//
// The Scene panel hosts this control. It presents frames through
// GpuViewportBridge (Engine.Editor, Avalonia-free): when the Avalonia
// compositor exposes GPU interop (AvaloniaViewportInterop attached), the
// engine's Vulkan image is shared zero-copy; otherwise the CPU fallback
// renders RenderToBuffer bytes into a WriteableBitmap.
//
// Forced fallback (tests): ENGINE_VIEWPORT_GPU=0, or headless/offscreen
// (SDL_VIDEO_DRIVER=offscreen / no compositor), always takes the CPU path.
// Acceptance: GPU build shows the shared image with no CPU copy;
// headless/offscreen automatically uses CPU; no regression to the CPU path.
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using Engine.Editor;

namespace Engine.EditorApp;

/// <summary>
/// Scene viewport control: GPU-shared engine image when interop is
/// available, CPU WriteableBitmap fallback otherwise. Headless-safe:
/// with no compositor (or forced fallback) it renders CPU bytes only.
/// A Border hosting an Image, like the other editor panels.
/// </summary>
public sealed class SceneViewportView : Border, IDisposable
{
    /// <summary>Env toggle: ENGINE_VIEWPORT_GPU=0 forces the CPU fallback path (tests).</summary>
    public const string ForceFallbackEnvVar = "ENGINE_VIEWPORT_GPU";

    private readonly ViewportPresenter? _presenter;
    private readonly GpuViewportBridge? _bridge;
    private WriteableBitmap? _cpuBitmap;
    private readonly Image _cpuImage = new() { Stretch = Stretch.Fill };
    private bool _attached;

    /// <summary>Headless/test ctor: no presenter; renders nothing, reports CPU path.</summary>
    public SceneViewportView()
    {
        Name = "Scene";
        Child = _cpuImage;
    }

    /// <summary>Live ctor: present through <paramref name="bridge"/> (zero-copy when engaged).</summary>
    public SceneViewportView(ViewportPresenter presenter, GpuViewportBridge bridge)
        : this()
    {
        _presenter = presenter ?? throw new ArgumentNullException(nameof(presenter));
        _bridge = bridge ?? throw new ArgumentNullException(nameof(bridge));
    }

    /// <summary>Which present path produced the most recent frame.</summary>
    public ViewportPresentMode LastMode => _bridge?.LastMode ?? ViewportPresentMode.CpuReadback;

    /// <summary>True once any frame took the zero-copy path.</summary>
    public bool ZeroCopyEngaged => _bridge?.ZeroCopyEngaged ?? false;

    /// <summary>
    /// True when the CPU fallback is forced: ENGINE_VIEWPORT_GPU=0,
    /// SDL_VIDEO_DRIVER=offscreen, or UseHeadlessDrawing=false hosts.
    /// Never throws (env reads are guarded).
    /// </summary>
    public static bool IsFallbackForced
    {
        get
        {
            try
            {
                if (string.Equals(
                    Environment.GetEnvironmentVariable(ForceFallbackEnvVar), "0", StringComparison.Ordinal))
                {
                    return true;
                }

                return string.Equals(
                    Environment.GetEnvironmentVariable("SDL_VIDEO_DRIVER"), "offscreen", StringComparison.OrdinalIgnoreCase);
            }
            catch
            {
                return true;
            }
        }
    }

    /// <summary>
    /// Resolve the interop probe for this host: null when fallback is
    /// forced or no compositor/interop is available (headless). The caller
    /// passes the result to GpuViewportBridge; null means CPU-only.
    /// </summary>
    public IGpuViewportInterop? ResolveInterop()
    {
        if (IsFallbackForced)
        {
            return null;
        }

        try
        {
            var interop = new AvaloniaViewportInterop(this);
            return interop.IsAvailable ? interop : null;
        }
        catch
        {
            return null;
        }
    }

    /// <summary>
    /// Present one frame at the control's current size. Returns true on
    /// the zero-copy path (no CPU bytes touched), false on CPU fallback.
    /// Never throws: every failure degrades to (or skips) the CPU blit.
    /// </summary>
    public bool PresentFrame()
    {
        if (_presenter == null || _bridge == null || IsFallbackForced)
        {
            PresentCpu();
            return false;
        }

        var w = Math.Max((uint)Bounds.Width, 1u);
        var h = Math.Max((uint)Bounds.Height, 1u);
        byte[]? bytes;
        try
        {
            bytes = _bridge.Present(w, h);
        }
        catch
        {
            // A throwing bridge showed NO frame: neither zero-copy (no
            // image was shared) nor CPU (no bytes were blitted). Report
            // failure (false), distinct from zero-copy success (true).
            return false;
        }

        if (bytes == null)
        {
            return true;
        }

        BlitCpu(bytes, (int)w, (int)h);
        return false;
    }

    /// <summary>CPU fallback: render via presenter and blit into the WriteableBitmap.</summary>
    private void PresentCpu()
    {
        if (_presenter == null)
        {
            return;
        }

        var w = Math.Max((uint)Bounds.Width, 1u);
        var h = Math.Max((uint)Bounds.Height, 1u);
        byte[] bytes;
        try
        {
            bytes = _presenter.Frame(w, h);
        }
        catch
        {
            return;
        }

        BlitCpu(bytes, (int)w, (int)h);
    }

    private void BlitCpu(byte[] bytes, int w, int h)
    {
        try
        {
            var size = new PixelSize(w, h);
            if (_cpuBitmap == null || _cpuBitmap.PixelSize != size)
            {
                _cpuBitmap?.Dispose();
                _cpuBitmap = new WriteableBitmap(
                    size, new Vector(96, 96), PixelFormat.Rgba8888, AlphaFormat.Opaque);
                _cpuImage.Source = _cpuBitmap;
                _attached = true;
            }

            using var fb = _cpuBitmap.Lock();
            unsafe
            {
                var span = new Span<byte>(fb.Address.ToPointer(), bytes.Length);
                bytes.AsSpan().CopyTo(span);
            }

            InvalidateVisual();
        }
        catch
        {
            // CPU blit must never throw out of the frame path.
        }
    }

    /// <summary>True once the CPU bitmap is attached (fallback rendered at least once).</summary>
    public bool CpuAttachedForTest => _attached;

    /// <summary>Release the owned CPU bitmap (editor teardown path).</summary>
    public void Dispose()
    {
        _cpuBitmap?.Dispose();
        _cpuBitmap = null;
    }
}

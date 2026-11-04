// AvaloniaViewportInterop.cs — P3-13: Avalonia 12.1.4 GPU-interop probe.
//
// Lives in Engine.EditorApp (the only managed project with an Avalonia
// reference) so Engine.Editor stays headless-safe. Implements
// IGpuViewportInterop over the verified 12.1.4 surface (see
// docs/notes/P3-13.md; every type/member below was confirmed by
// reflecting over the shipped Avalonia.Base.dll — no memory, no docs site):
//
//   ElementComposition.GetElementVisual(host).Compositor
//   Compositor.TryGetCompositionGpuInterop() -> ICompositionGpuInterop
//     SupportedImageHandleTypes / SupportedSemaphoreTypes (queried,
//     never hardcoded)
//     GetSynchronizationCapabilities(handleType)
//     ImportImage(IPlatformHandle, PlatformGraphicsExternalImageProperties)
//     IsLost (checked per frame)
//   Compositor.CreateDrawingSurface() + CompositionDrawingSurface.UpdateAsync
//     + CompositionSurfaceVisual.Surface, attached with
//     ElementComposition.SetElementChildVisual.
//
// Zero native changes: the engine keeps rendering into its own Vulkan
// image; only the PRESENT path changes (share the image with the
// compositor instead of render_to_buffer's CPU readback). The native
// export (vkGetMemoryFdKHR / Win32 handle / IOSurface) lands here as an
// IPlatformHandle built by the caller from the engine's per-frame export;
// this class never guesses an fd — no handle, no share (returns false and
// the bridge falls back to CPU).
//
// Object lifetime: the compositor copies/interns the image on UpdateAsync;
// the engine must keep its VkImage alive until that Task completes. The
// async present awaits the update, so by the time it returns true the
// caller may recycle the native image. The sync IGpuViewportInterop
// member blocks only on Task (UpdateAsync), never on ValueTask: the
// ValueTask-returning TryGetCompositionGpuInterop is awaited once in
// AttachAsync and the interop cached.
using Avalonia;
using Avalonia.Platform;
using Avalonia.Rendering.Composition;
using Engine.Editor;

namespace Engine.EditorApp;

/// <summary>
/// Avalonia-backed <see cref="IGpuViewportInterop"/>: shares the engine's
/// already-rendered Vulkan image with the compositor (zero-copy present).
/// The Scene panel hosts a <see cref="CompositionSurfaceVisual"/> whose
/// surface is fed by <see cref="CompositionDrawingSurface.UpdateAsync"/>.
/// Any failure (no interop, lost device, unsupported type, import throws)
/// returns false so <see cref="GpuViewportBridge"/> takes the CPU path.
/// </summary>
public sealed class AvaloniaViewportInterop : IGpuViewportInterop, IDisposable
{
    private readonly Visual _host;
    private ICompositionGpuInterop? _interop;
    private CompositionDrawingSurface? _surface;
    private CompositionSurfaceVisual? _visual;
    private string _handleType = string.Empty;
    private bool _disposed;

    /// <param name="host">
    /// Control the surface visual is attached to
    /// (ElementComposition.SetElementChildVisual). Must already be
    /// attached to the visual tree (after the window is shown) so
    /// GetElementVisual resolves a compositor.
    /// </param>
    public AvaloniaViewportInterop(Visual host)
    {
        _host = host ?? throw new ArgumentNullException(nameof(host));
    }

    /// <summary>Handle type selected at attach (empty until attached).</summary>
    public string HandleType => _handleType;

    /// <summary>
    /// True once attached to a compositor whose interop is present and not
    /// lost. Re-evaluated per frame (device loss mid-session falls back).
    /// </summary>
    public bool IsAvailable
    {
        get
        {
            if (_disposed || _interop is null || _surface is null || _handleType.Length == 0)
            {
                return false;
            }

            try
            {
                return !_interop.IsLost;
            }
            catch
            {
                return false;
            }
        }
    }

    /// <summary>
    /// Attach to the host's compositor and select the engine's handle
    /// type. Returns true when a zero-copy share may be attempted.
    /// The preferred <paramref name="preferredHandleType"/> is honoured
    /// only when the compositor lists it in SupportedImageHandleTypes;
    /// otherwise the first supported type wins; none supported = false.
    /// Safe to call headless: no compositor or no interop returns false.
    /// </summary>
    public async Task<bool> AttachAsync(string preferredHandleType)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        try
        {
            var compositor = ElementComposition.GetElementVisual(_host)?.Compositor;
            var interop = compositor is null ? null : await compositor.TryGetCompositionGpuInterop();
            if (compositor is null || interop is null || interop.IsLost)
            {
                return false;
            }

            var supported = interop.SupportedImageHandleTypes;
            string? selected = null;
            for (var i = 0; i < supported.Count; i++)
            {
                if (supported[i] == preferredHandleType)
                {
                    selected = supported[i];
                    break;
                }
            }

            selected ??= supported.Count > 0 ? supported[0] : null;
            if (string.IsNullOrEmpty(selected))
            {
                return false;
            }

            _surface ??= compositor.CreateDrawingSurface();
            _visual ??= compositor.CreateSurfaceVisual();
            _visual.Surface = _surface;
            ElementComposition.SetElementChildVisual(_host, _visual);
            _interop = interop;
            _handleType = selected;
            return true;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>
    /// Async share: imports the exported frame image and hands it to the
    /// drawing surface. False = take the CPU path this frame.
    /// </summary>
    public async Task<bool> TryPresentSharedImageAsync(ulong frameId, uint width, uint height)
    {
        if (_disposed || frameId == 0 || width == 0 || height == 0)
        {
            return false;
        }

        try
        {
            var interop = _interop;
            var surface = _surface;
            if (interop is null || interop.IsLost || surface is null)
            {
                return false;
            }

            // The caller exports the engine image to an OS handle via
            // vkGetMemoryFdKHR / Win32 / IOSurface and passes it back as
            // (handle, descriptor) through ExportForFrame; IntPtr.Zero
            // means "no export this frame" -> CPU fallback, no throw.
            var (handle, descriptor) = ExportForFrame(frameId, width, height);
            if (handle == IntPtr.Zero || string.IsNullOrEmpty(descriptor))
            {
                return false;
            }

            var props = new PlatformGraphicsExternalImageProperties
            {
                Width = (int)width,
                Height = (int)height,
                Format = PlatformGraphicsExternalImageFormat.B8G8R8A8UNorm,
            };
            var imported = interop.ImportImage(new PlatformHandle(handle, descriptor), props);
            if (imported is null)
            {
                return false;
            }

            await surface.UpdateAsync(imported);
            return true;
        }
        catch
        {
            return false;
        }
    }

    /// <inheritdoc/>
    bool IGpuViewportInterop.TryPresentSharedImage(ulong frameId, uint width, uint height)
    {
        try
        {
            return TryPresentSharedImageAsync(frameId, width, height).GetAwaiter().GetResult();
        }
        catch
        {
            return false;
        }
    }

    /// <summary>
    /// Per-frame native export hook. Default: no export (headless/tests).
    /// The engine host overrides this to return the real fd/NT-handle/
    /// IOSurface of the retired frame.
    /// </summary>
    public Func<ulong, uint, uint, (nint Handle, string Descriptor)> ExportForFrame { get; set; } =
        (_, _, _) => (IntPtr.Zero, string.Empty);

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _interop = null;
        _visual = null;
        var surface = _surface;
        _surface = null;
        try
        {
            surface?.Dispose();
        }
        catch
        {
            // Disposal must not throw out of the editor teardown path.
        }
    }
}

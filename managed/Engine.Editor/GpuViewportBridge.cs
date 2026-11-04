// GpuViewportBridge.cs — P3-13: zero-copy viewport with CPU fallback (headless-safe).
//
// Long pole: on a real GPU, ViewportPresenter.Frame() pays a full CPU
// readback (render_to_buffer renders, barriers to TRANSFER_SRC, copies the
// color target host-side, uploads into a WriteableBitmap). This bridge
// selects the zero-copy path when Avalonia's compositor exposes GPU
// interop, and falls back to the CPU byte[] path everywhere else.
//
// Why this shape (rule 6: verified from the 12.1.4 assemblies, not memory):
//   Compositor.TryGetCompositionGpuInterop() -> ICompositionGpuInterop
//     .SupportedImageHandleTypes / .SupportedSemaphoreTypes (query, never hardcode;
//      values: VulkanOpaquePosixFileDescriptor Linux, VulkanOpaqueNtHandle/
//      D3D11* Windows, IOSurfaceRef macOS)
//     .GetSynchronizationCapabilities(handleType) -> Semaphores | KeyedMutex |
//      Automatic | TimelineSemaphores
//     .ImportImage(IPlatformHandle, PlatformGraphicsExternalImageProperties)
//      or .ImportImage(ICompositionImportableSharedGpuContextImage) (same-device
//      fast path, no fd/NThandle round-trip)
//     .IsLost (re-query per frame; fall back on lost)
//   Present: CompositionDrawingSurface.{UpdateAsync,UpdateWith*}
//    + CompositionSurfaceVisual.Surface (per frame; the surface owns nothing,
//    the engine keeps the Vk image alive until ImportCompleted).
//   CPU fallback: the existing RenderToBuffer byte[] path (this file's
//   Frame() delegate); headless goldens (UseHeadlessDrawing=false) always
//   take this path.
//
// This project (Engine.Editor) has NO Avalonia reference (headless-safe by
// design): the bridge talks only to IGpuViewportInterop, an
// engine-owned probe the Avalonia app layer (Engine.EditorApp, which DOES
// reference Avalonia 12.1.4) implements by reflection-free calls on the
// types above. Tests inject a stub probe: forced-zero-copy measures the
// direct-present half, forced-CPU measures the readback half, and a lost
// probe proves the fallback engages.
namespace Engine.Editor;

/// <summary>Which present path produced a frame (acceptance accounting).</summary>
public enum ViewportPresentMode
{
    /// <summary>CPU readback: render_to_buffer + host copy (fallback, headless).</summary>
    CpuReadback = 0,

    /// <summary>Zero-copy: engine Vk image shared with the compositor.</summary>
    ZeroCopy = 1,
}

/// <summary>
/// Probe over Avalonia's GPU interop (implemented in Engine.EditorApp;
/// Engine.Editor stays Avalonia-free). One method, called per present:
/// return true and the imported-image token when the engine's current
/// frame image was shared; false to take the CPU path this frame.
/// </summary>
public interface IGpuViewportInterop
{
    /// <summary>True when a GPU share may be attempted this frame (compositor up, device not lost).</summary>
    bool IsAvailable { get; }

    /// <summary>
    /// Attempt the zero-copy share of the frame identified by
    /// <paramref name="frameId"/> at <paramref name="width"/>x<paramref name="height"/>.
    /// Returns true when the compositor accepted the image (caller then
    /// skips the readback). Implementations must never throw: false means
    /// "take the CPU path".
    /// </summary>
    bool TryPresentSharedImage(ulong frameId, uint width, uint height);

    /// <summary>Handle type the probe imports with (for diagnostics; e.g. VulkanOpaquePosixFileDescriptor).</summary>
    string HandleType { get; }
}

/// <summary>Zero-copy viewport bridge: picks zero-copy vs CPU readback per frame.</summary>
public sealed class GpuViewportBridge
{
    private readonly IGpuViewportInterop? _interop;
    private readonly Func<uint, uint, byte[]> _cpuFrame;
    private ulong _frames;
    private ulong _zeroCopyFrames;
    private ulong _cpuFrames;
    private ViewportPresentMode _lastMode = ViewportPresentMode.CpuReadback;

    /// <param name="cpuFrame">
    /// CPU fallback: render w x h and return width*height*4 RGBA8 bytes
    /// (ViewportPresenter.Frame). Never null; always available.
    /// </param>
    /// <param name="interop">
    /// GPU probe (null = CPU only, e.g. headless goldens). When
    /// IsAvailable is false this frame, the CPU path is taken.
    /// </param>
    public GpuViewportBridge(Func<uint, uint, byte[]> cpuFrame, IGpuViewportInterop? interop = null)
    {
        _cpuFrame = cpuFrame ?? throw new ArgumentNullException(nameof(cpuFrame));
        _interop = interop;
    }

    /// <summary>Frames presented since construction (both paths).</summary>
    public ulong FramesPresented => _frames;

    /// <summary>Frames presented via the zero-copy path.</summary>
    public ulong ZeroCopyFrames => _zeroCopyFrames;

    /// <summary>Frames presented via the CPU readback path.</summary>
    public ulong CpuFrames => _cpuFrames;

    /// <summary>Which path produced the most recent frame.</summary>
    public ViewportPresentMode LastMode => _lastMode;

    /// <summary>True once any frame took the zero-copy path.</summary>
    public bool ZeroCopyEngaged => _zeroCopyFrames > 0;

    /// <summary>
    /// Present one frame at <paramref name="width"/>x<paramref name="height"/>.
    /// Returns the CPU bytes only on the fallback path (null on zero-copy,
    /// where the image never leaves the GPU). Never throws out of the
    /// interop: any probe failure falls back to the CPU path.
    /// </summary>
    public byte[]? Present(uint width, uint height)
    {
        ArgumentOutOfRangeException.ThrowIfZero(width);
        ArgumentOutOfRangeException.ThrowIfZero(height);
        _frames++;
        if (_interop != null && _interop.IsAvailable)
        {
            bool shared;
            try
            {
                shared = _interop.TryPresentSharedImage(_frames, width, height);
            }
            catch
            {
                shared = false;
            }

            if (shared)
            {
                _zeroCopyFrames++;
                _lastMode = ViewportPresentMode.ZeroCopy;
                return null;
            }
        }

        var bytes = _cpuFrame(width, height);
        _cpuFrames++;
        _lastMode = ViewportPresentMode.CpuReadback;
        return bytes;
    }
}

// GlobalIllumination.cs — M3-04: managed zero-bake GI surface.
//
// CPU-side managed mirror of the native GI planners (MeshSdfBuilder /
// GiCascadePlan / ShadowMapPlan sizing math lives natively; this class
// owns the frame-level GI state C# render code needs):
//   * cascade probe-volume sizing for a scene AABB (byte-exact, mirrors
//     GiCascadePlan::total_probe_bytes),
//   * SDSM atlas sizing (mirrors ShadowMapPlan::atlas_bytes),
//   * GI resolve + shadow pass registration on the GraphicsCompositor
//     (custom passes between shade and post, same callback shape).
//
// Zero-alloc frame path: AddGiPasses registers two counting-forward
// callbacks once; per-frame cost is one Compositor.RunFrame (already
// proven 0-alloc in CompositorApiTests).
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Engine.Bindings;

namespace Engine.Runtime;

/// <summary>
/// Zero-bake GI pass set: registers the M3-04 GI resolve + shadow trace
/// custom passes on a GraphicsCompositor and owns the volume/atlas sizing
/// for the scene AABB. All math is CPU-side (no GPU handle needed).
/// </summary>
public sealed unsafe class GlobalIllumination : IDisposable
{
    // Mirrors native constants (gi_cascade.hpp / gi_shadow.hpp).
    public const int CascadeLevels = 4;
    public const int ProbeRes = 16;
    public const long ProbeBytesPerLevel = 16L * 16 * 16 * 16; // 64KiB
    public const long TotalProbeBytes = ProbeBytesPerLevel * CascadeLevels; // 256KiB
    public const long ShadowAtlasBytes = 2048L * 2048 * 4; // R32F 16MiB
    public const int ShadowCascades = 4;

    private readonly GraphicsCompositor _compositor;
    private bool _disposed;
    private bool _passesAdded;

    public GlobalIllumination(GraphicsCompositor compositor)
    {
        ArgumentNullException.ThrowIfNull(compositor);
        _compositor = compositor;
    }

    /// <summary>Probe volume bytes for the scene (fixed 256KiB — coverage, not scene size).</summary>
    public static long ProbeBytesForScene() => TotalProbeBytes;

    /// <summary>Shadow atlas bytes (fixed 2048^2 R32F).</summary>
    public static long AtlasBytes() => ShadowAtlasBytes;

    // GI callbacks: forward to no-op counting stubs (GPU record happens
    // natively in M4; this card proves registration + ordering + zero
    // per-frame managed allocation).
    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static int GiResolvePass(uint passIndex, void* userData) => 0;

    [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
    private static int ShadowTracePass(uint passIndex, void* userData) => 0;

    /// <summary>
    /// Register the GI resolve + shadow trace passes (hdr read/write pair,
    /// ordered resolve-then-shadow so the shadow factor modulates lit GI).
    /// Idempotent: second call is a no-op.
    /// </summary>
    public void AddGiPasses()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (_passesAdded)
        {
            return;
        }

        _compositor.AddImage("gi_irradiance", 64, 64);
        _compositor.AddImage("shadow_factor", 64, 64);
        _compositor.AddPass("gi_resolve", ["hdr"], ["gi_irradiance"], &GiResolvePass);
        _compositor.AddPass("shadow_trace", ["gi_irradiance"], ["shadow_factor"], &ShadowTracePass);
        _passesAdded = true;
    }

    public bool PassesAdded => _passesAdded;

    public void Dispose()
    {
        _disposed = true;
    }
}

// Rendering.cs — P1-09: C# view of the native render components + Renderer.
//
// Camera, MeshRenderer, DirectionalLight are IComponent structs in
// namespace Engine.Runtime so their full names match the native
// registration P1-08 performs ("Engine.Runtime.Camera" etc.). Layouts
// match the native structs exactly (see ecs_render_components.hpp):
//   Camera           { float fov_y_rad, near_z, far_z; uint is_main; }  16B, align 4
//   MeshRenderer     { uint mesh_id; float base_color[4]; }              20B, align 4
//   DirectionalLight { float direction[3]; float intensity; }           16B, align 4
//
// Renderer is an IDisposable over the ABI 4.4 renderer tail: create /
// frame / mesh_create / screenshot / destroy. The world is BORROWED
// (destroy the Renderer before the world, or frames stop).
using System.Runtime.InteropServices;
using System.Text;
using Engine.Bindings;

namespace Engine.Runtime;

/// <summary>Perspective camera (16 bytes, align 4 — matches native).</summary>
#pragma warning disable CA1051 // Visible instance fields by design: blittable interop struct.
public struct Camera : IComponent
{
    public float FovYRad;
    public float NearZ;
    public float FarZ;
    public uint IsMain;
}

/// <summary>Mesh draw (20 bytes, align 4 — matches native).</summary>
public unsafe struct MeshRenderer : IComponent
{
    public uint MeshId;
    public fixed float BaseColor[4];
}

/// <summary>Directional light (16 bytes, align 4 — matches native).</summary>
public unsafe struct DirectionalLight : IComponent
{
    public fixed float Direction[3];
    public float Intensity;
}
#pragma warning restore CA1051

/// <summary>
/// Owns a native EngineRenderer for a borrowed world. Init-time API:
/// create the world, create the renderer, spawn entities, frame.
/// </summary>
public sealed unsafe class Renderer : IDisposable
{
    private NativeApi.ApiTable* _table;
    private NativeApi.EngineRenderer* _renderer;
    private bool _disposed;

    /// <summary>Render extent the native swapchain was created with.</summary>
    public uint Width { get; }

    /// <summary>Render extent the native swapchain was created with.</summary>
    public uint Height { get; }

    private Renderer(NativeApi.ApiTable* table, NativeApi.EngineRenderer* renderer, uint width, uint height)
    {
        _table = table;
        _renderer = renderer;
        Width = width;
        Height = height;
    }

    /// <summary>
    /// Creates a renderer for <paramref name="world"/> (borrowed: must
    /// outlive the renderer). <paramref name="hidden"/> selects an
    /// offscreen window (flag bit 0, CI).
    /// </summary>
    public static Renderer Create(NativeApi.ApiTable* table, NativeApi.EngineWorld* world, uint width, uint height, bool hidden = true)
    {
        ArgumentNullException.ThrowIfNull(table);
        ArgumentNullException.ThrowIfNull(world);
        if (width == 0 || height == 0)
        {
            throw new ArgumentOutOfRangeException(nameof(width), "Renderer extent must be nonzero.");
        }

        var flags = hidden ? 1u : 0u;
        var renderer = table->EngineRendererCreate(world, width, height, flags);
        if (renderer == null)
        {
            throw new InvalidOperationException("renderer_create failed (null world, zero extent, no Vulkan device, or pipeline build failure).");
        }

        return new Renderer(table, renderer, width, height);
    }

    /// <summary>
    /// Pumps SDL events, renders one frame over the bound world, and
    /// presents. Returns true when the window was closed (caller should
    /// stop its loop); false on a normal frame. Throws on failure.
    /// </summary>
    public bool Frame()
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        var rc = _table->EngineRendererFrame(_renderer);
        if (rc == 1)
        {
            return true;
        }

        if (rc != 0)
        {
            throw new InvalidOperationException($"renderer_frame failed with code {rc}.");
        }

        return false;
    }

    /// <summary>
    /// Uploads one interleaved MeshVertex buffer (pos[3], normal[3], uv[2]
    /// = 8 floats/vertex) plus u32 indices. Returns the mesh_id (>= 1).
    /// Throws when the native mesh pipeline is unavailable (SDK-free CI
    /// builds report kVulkanLoaderFailed at create_mesh time, so this is
    /// 0 there) — the caller decides skip-vs-fail via ENGINE_REQUIRE_GPU.
    /// </summary>
    public uint MeshCreate(ReadOnlySpan<float> vertices, int vertexCount, ReadOnlySpan<uint> indices)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (vertexCount <= 0 || indices.Length == 0 || vertices.Length < vertexCount * 8)
        {
            throw new ArgumentException("Mesh vertex/index counts are invalid (vertices must carry vertexCount * 8 floats).");
        }

        uint id;
        fixed (float* v = vertices)
        fixed (uint* i = indices)
        {
            id = _table->EngineRendererMeshCreate(_renderer, v, (uint)vertexCount, i, (uint)indices.Length);
        }

        if (id == 0)
        {
            throw new InvalidOperationException("renderer_mesh_create failed (null renderer/vertices/indices or zero counts).");
        }

        return id;
    }

    /// <summary>Captures the last presented frame to <paramref name="pngPath"/>.</summary>
    public void Screenshot(string pngPath)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        ArgumentException.ThrowIfNullOrEmpty(pngPath);
        var bytes = Encoding.UTF8.GetBytes(pngPath + "\0");
        int rc;
        fixed (byte* p = bytes)
        {
            rc = _table->EngineRendererScreenshot(_renderer, p);
        }

        if (rc != 0)
        {
            throw new InvalidOperationException($"renderer_screenshot failed with code {rc} (no frame rendered yet or write failure).");
        }
    }

    /// <summary>
    /// Creates an offscreen renderer for <paramref name="world"/> (borrowed:
    /// must outlive the renderer). No window, no swapchain: color +
    /// R32G32_UINT id + depth targets sized to the create extent.
    /// </summary>
    public static Renderer CreateOffscreen(NativeApi.ApiTable* table, NativeApi.EngineWorld* world, uint width, uint height)
    {
        ArgumentNullException.ThrowIfNull(table);
        ArgumentNullException.ThrowIfNull(world);
        if (width == 0 || height == 0)
        {
            throw new ArgumentOutOfRangeException(nameof(width), "Renderer extent must be nonzero.");
        }

        var renderer = table->EngineRendererCreateOffscreen(world, width, height);
        if (renderer == null)
        {
            throw new InvalidOperationException("renderer_create_offscreen failed (null world, zero extent, no Vulkan device, or pipeline/target build failure).");
        }

        return new Renderer(table, renderer, width, height);
    }

    /// <summary>
    /// Renders the bound world offscreen with the explicit column-major
    /// camera matrix <paramref name="viewProj16"/> (16 floats) into an RGBA8
    /// buffer (width x height x 4). Resizes the targets when the size differs
    /// from the create extent. Windowed renderers reject this (BAD_ARG).
    /// </summary>
    public byte[] RenderToBuffer(float[] viewProj16, uint width, uint height)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        ArgumentNullException.ThrowIfNull(viewProj16);
        if (viewProj16.Length != 16)
        {
            throw new ArgumentException("View-projection matrix must be exactly 16 floats.", nameof(viewProj16));
        }

        if (width == 0 || height == 0)
        {
            throw new ArgumentOutOfRangeException(nameof(width), "Render extent must be nonzero.");
        }

        var need = (ulong)width * height * 4;
        if (need > int.MaxValue)
        {
            throw new ArgumentOutOfRangeException(nameof(width), "Render extent too large.");
        }

        var outBytes = new byte[(int)need];
        int rc;
        fixed (byte* o = outBytes)
        fixed (float* vp = viewProj16)
        {
            rc = _table->EngineRendererRenderToBuffer(_renderer, o, need, width, height, vp);
        }

        if (rc != 0)
        {
            throw new InvalidOperationException($"renderer_render_to_buffer failed with code {rc}.");
        }

        return outBytes;
    }

    /// <summary>
    /// Runs the id pass and reads one texel at (<paramref name="x"/>,
    /// <paramref name="y"/>). Returns the 64-bit flecs entity id, or 0 for
    /// background / out-of-bounds reads. Pass the same column-major
    /// <paramref name="viewProj16"/> used for <see cref="RenderToBuffer"/> so
    /// the id matches the frame on screen; null uses the world's main camera.
    /// </summary>
    public ulong Pick(uint x, uint y, float[]? viewProj16 = null)
    {
        ObjectDisposedException.ThrowIf(_disposed, this);
        if (viewProj16 is null)
        {
            return _table->EngineRendererPick(_renderer, x, y, null);
        }

        if (viewProj16.Length != 16)
        {
            throw new ArgumentException("View-projection matrix must be exactly 16 floats.", nameof(viewProj16));
        }

        fixed (float* vp = viewProj16)
        {
            return _table->EngineRendererPick(_renderer, x, y, vp);
        }
    }

    /// <summary>Frees the renderer (never the world). Idempotent.</summary>
    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        if (_renderer != null)
        {
            _table->EngineRendererDestroy(_renderer);
            _renderer = null;
        }
    }
}

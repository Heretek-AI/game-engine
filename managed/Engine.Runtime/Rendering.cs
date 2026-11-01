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
// Renderer is an IDisposable over the ABI 4.3 renderer tail: create /
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

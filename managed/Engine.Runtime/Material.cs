// Material.cs — P4-01: managed mirror of the native Material component.
//
// Blittable 48B struct (Sequential, Pack=4, Size=48) matching
// engine::renderer::Material byte-for-byte: table_index + per-instance
// factor multipliers + flags + pad. Registered with the native world by
// full name ("Engine.Runtime.Material") through Ecs.ComponentId<T>,
// the same path every IComponent takes — no ABI table change.
using System.Runtime.InteropServices;

namespace Engine.Runtime;

/// <summary>Per-entity PBR material assignment. 48 bytes, blittable.</summary>
[StructLayout(LayoutKind.Sequential, Pack = 4, Size = 48)]
public partial struct Material : IComponent
{
    /// <summary>Index into the frame material table (bindless material SSBO slot).</summary>
    public uint TableIndex;

    /// <summary>Per-instance base-color multiplier (multiplied with the table factor).</summary>
    public float BaseColorMulR;
    public float BaseColorMulG;
    public float BaseColorMulB;
    public float BaseColorMulA;

    /// <summary>Per-instance metallic multiplier.</summary>
    public float MetallicMul;

    /// <summary>Per-instance roughness multiplier.</summary>
    public float RoughnessMul;

    /// <summary>Per-instance emissive multiplier.</summary>
    public float EmissiveMulR;
    public float EmissiveMulG;
    public float EmissiveMulB;

    /// <summary>Flag bits (bit0 = double-sided, mirrors kMaterialFlagDoubleSided).</summary>
    public uint Flags;

    private uint _pad;
}

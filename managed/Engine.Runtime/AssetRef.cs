// AssetRef.cs — M2-01: blittable asset handle.
//
// A single ulong cookie identifying a loaded asset (mesh, texture, clip).
// The asset lives in the native/managed asset store; components only hold
// this 8-byte handle, so AssetRef<T> stays blittable for any T and is safe
// to embed in IComponent structs. The default value (Id == 0) is the null
// reference — no statics on this generic type by design (CA1000).
namespace Engine.Runtime;

/// <summary>
/// Blittable 8-byte reference to a loaded asset of type <typeparamref name="T"/>.
/// The asset itself lives outside component storage; this struct is only the
/// lookup cookie. <c>default</c> (Id == 0) means "no asset".
/// </summary>
/// <typeparam name="T">Asset type. Unconstrained on purpose: the struct holds
/// only a ulong, so it is blittable for class and struct assets alike.</typeparam>
public readonly record struct AssetRef<T>(ulong Id)
{
    /// <summary>False only for <c>default</c> (Id == 0).</summary>
    public bool IsValid => Id != 0;
}

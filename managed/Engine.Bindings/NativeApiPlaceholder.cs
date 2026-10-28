// Placeholder so Engine.Bindings builds from M1-01.
// M1-04 (abi_codegen) generates NativeApi.g.cs and supersedes this stub.
namespace Engine.Bindings;

/// <summary>
/// Build pipeline placeholder. The real zero-copy interop surface
/// (delegate* unmanaged[Cdecl, SuppressGCTransition] tables) is emitted
/// by tools/abi_codegen from native/bridge/engine_abi.h in M1-04.
/// </summary>
public static class NativeApiPlaceholder
{
    public const string AbiContractVersion = "0.1.0-m1-01-stub";
}

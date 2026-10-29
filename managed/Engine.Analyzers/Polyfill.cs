// Polyfill.cs — IsExternalInit for netstandard2.0 record support.
// The generator targets netstandard2.0 (loads in any host compiler); the
// record types in ComponentSystemGenerator need this well-known attribute,
// which the netstandard2.0 ref set does not declare. Internal: invisible to
// consumers of the analyzer assembly.
namespace System.Runtime.CompilerServices
{
    internal static class IsExternalInit
    {
    }
}

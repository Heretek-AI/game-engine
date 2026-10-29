// HotPath.cs — M2-03: shared hot-path gate for ENG0001/ENG0003.
//
// A method is hot-path when it carries [UpdateInPhase] itself, or when its
// containing struct does (struct-level scheduling covers every Tick), or
// when it IS a Tick method on an IComponent struct (Tick is hot by
// construction — the phase attribute may be absent entirely).
//
// Name-matched, namespace-agnostic: the attribute and interface resolve
// through `using Engine.Runtime;` in user code, and matching the short names
// ("UpdateInPhaseAttribute", "IComponent") is the robust check — the same
// convention the M2-02 generator uses in ReadPhases.
using System.Linq;
using Microsoft.CodeAnalysis;

namespace Engine.Analyzers;

/// <summary>Shared hot-path predicate for the ENG0001/ENG0003 analyzers.</summary>
internal static class HotPath
{
    /// <summary>Short names matched independent of namespace context.</summary>
    internal const string UpdateInPhaseAttributeName = "UpdateInPhaseAttribute";

    /// <summary>Short names matched independent of namespace context.</summary>
    internal const string IComponentName = "IComponent";

    /// <summary>True when the method runs on the engine hot path.</summary>
    internal static bool IsHotPathMethod(IMethodSymbol method)
    {
        // Direct: [UpdateInPhase] on the method.
        if (HasAttribute(method, UpdateInPhaseAttributeName))
        {
            return true;
        }

        var containing = method.ContainingType;
        if (containing is null)
        {
            return false;
        }

        // Struct-level: [UpdateInPhase] on the containing type schedules
        // every Tick in it.
        if (HasAttribute(containing, UpdateInPhaseAttributeName))
        {
            return true;
        }

        // Construction: Tick on an IComponent struct is the hot path even
        // with no attribute anywhere.
        return method.Name == "Tick" && Implements(containing, IComponentName);
    }

    private static bool HasAttribute(ISymbol symbol, string shortName)
    {
        foreach (var a in symbol.GetAttributes())
        {
            if (a.AttributeClass?.Name == shortName)
            {
                return true;
            }
        }

        return false;
    }

    private static bool Implements(INamedTypeSymbol type, string interfaceShortName)
    {
        foreach (var i in type.AllInterfaces)
        {
            if (i.Name == interfaceShortName)
            {
                return true;
            }
        }

        return false;
    }
}

/// <summary>Short display strings for allocation diagnostics.</summary>
internal static class SymbolDisplayFormats
{
    /// <summary>Minimally-qualified type name (int, string, List&lt;T&gt;).</summary>
    internal static readonly SymbolDisplayFormat Short = new(
        typeQualificationStyle: SymbolDisplayTypeQualificationStyle.NameOnly,
        genericsOptions: SymbolDisplayGenericsOptions.IncludeTypeParameters);
}

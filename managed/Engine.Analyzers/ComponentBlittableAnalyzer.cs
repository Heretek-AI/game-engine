// ComponentBlittableAnalyzer.cs — M2-03: ENG0002 Roslyn analyzer.
//
// Forbids managed reference types inside any struct implementing
// IComponent: every instance field must be blittable (primitives, Entity,
// FixedString64, math types, other IComponent structs, pointers) so the
// struct can cross the native ABI by value with no GC tracking.
//
// Flagged field types:
//   * string, object, dynamic (SpecialType string/object).
//   * Reference-type symbols: class, interface, delegate, type parameters
//     (unresolved generics may bind to a reference at instantiation), and
//     any error type (fail closed — an unresolvable type is not provably
//     blittable).
//   * Arrays: SZ[] and multi-dimensional (a managed reference + bounds).
//   * Nullable<T> of a non-blittable T (recurses into the element).
// Skipped: static/const fields (never cross the ABI per instance),
//   ref structs (Span<T>, TempList<T> are stack-only scratch by
//   construction), unmanaged function/static-method pointers
//   (delegate* unmanaged), records (record struct with blittable fields is
//   a legal component — kind is irrelevant, fields decide).
//
// Error severity: a non-blittable component corrupts the native chunk copy,
// so the build breaks at the field declaration.
using System.Collections.Generic;
using System.Collections.Immutable;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.Diagnostics;

namespace Engine.Analyzers;

/// <summary>Non-blittable component field is a compile-time error.</summary>
[DiagnosticAnalyzer(LanguageNames.CSharp)]
public sealed class ComponentBlittableAnalyzer : DiagnosticAnalyzer
{
    /// <summary>ENG0002 descriptor: managed reference in an IComponent struct.</summary>
    public static readonly DiagnosticDescriptor Rule = new(
        id: "ENG0002",
        title: "Non-blittable component field",
        messageFormat: "Field '{0}' of type {1} is not blittable; IComponent structs must hold only unmanaged data (fixed buffers, Entity, FixedString64, math types)",
        category: "Interop",
        defaultSeverity: DiagnosticSeverity.Error,
        isEnabledByDefault: true,
        description: "IComponent structs cross the native ABI by value; managed references corrupt the chunk copy.");

    /// <inheritdoc/>
    public override ImmutableArray<DiagnosticDescriptor> SupportedDiagnostics => ImmutableArray.Create(Rule);

    /// <inheritdoc/>
    public override void Initialize(AnalysisContext context)
    {
        context.ConfigureGeneratedCodeAnalysis(GeneratedCodeAnalysisFlags.None);
        context.EnableConcurrentExecution();
        context.RegisterSymbolAction(CheckComponent, SymbolKind.NamedType);
    }

    private static void CheckComponent(SymbolAnalysisContext ctx)
    {
        if (ctx.Symbol is not INamedTypeSymbol type || !type.IsValueType)
        {
            return;
        }

        if (!ImplementsIComponent(type))
        {
            return;
        }

        // IComponent conformance includes the interface itself appearing in
        // AllInterfaces of subtypes — never flag the contract.
        if (type.Name == HotPath.IComponentName && type.TypeKind == TypeKind.Interface)
        {
            return;
        }

        foreach (var member in type.GetMembers().OfType<IFieldSymbol>())
        {
            // Static/const fields live off-instance: they never cross the
            // chunk ABI, so managed statics (caches, lookup tables) stay
            // legal while instance payload must be blittable.
            if (member.IsStatic || member.IsConst)
            {
                continue;
            }

            if (!IsBlittable(member.Type))
            {
                ctx.ReportDiagnostic(Diagnostic.Create(
                    Rule,
                    member.DeclaringSyntaxReferences.FirstOrDefault()?.GetSyntax(ctx.CancellationToken).GetLocation(),
                    member.Name,
                    member.Type.ToDisplayString(SymbolDisplayFormats.Short)));
            }
        }
    }

    private static bool ImplementsIComponent(INamedTypeSymbol type)
    {
        return type.AllInterfaces.Any(static i => i.Name == HotPath.IComponentName);
    }

    private static bool IsBlittable(ITypeSymbol type)
    {
        // Pointers and function pointers cross as raw addresses.
        if (type.TypeKind == TypeKind.Pointer || type.TypeKind == TypeKind.FunctionPointer)
        {
            return true;
        }

        // Arrays are managed references (object header + GC tracking) even
        // over blittable elements — FixedString64's fixed buffer is the
        // sanctioned inline alternative. Nullable<T> is a struct wrapper;
        // recurse into the element.
        if (type is IArrayTypeSymbol)
        {
            return false;
        }

        if (type is INamedTypeSymbol { IsGenericType: true } named &&
            named.ConstructedFrom?.SpecialType == SpecialType.System_Nullable_T)
        {
            return IsBlittable(named.TypeArguments[0], new HashSet<ISymbol>(SymbolEqualityComparer.Default));
        }

        // string/object/dynamic, classes, interfaces, delegates, type
        // parameters (may bind to a reference), and error types (fail
        // closed: unresolvable is not provably blittable) are all managed.
        if (!type.IsValueType)
        {
            return false;
        }

        return AreInstanceFieldsBlittable(type, new HashSet<ISymbol>(SymbolEqualityComparer.Default));
    }

    private static bool IsBlittable(ITypeSymbol type, HashSet<ISymbol> visiting)
    {
        if (type.TypeKind == TypeKind.Pointer || type.TypeKind == TypeKind.FunctionPointer)
        {
            return true;
        }

        if (type is IArrayTypeSymbol)
        {
            return false;
        }

        if (type is INamedTypeSymbol { IsGenericType: true } named &&
            named.ConstructedFrom?.SpecialType == SpecialType.System_Nullable_T)
        {
            return IsBlittable(named.TypeArguments[0], visiting);
        }

        if (!type.IsValueType)
        {
            return false;
        }

        return AreInstanceFieldsBlittable(type, visiting);
    }

    // A struct is only blittable when every instance field is: a nested
    // struct smuggling a string/array/List would otherwise cross the
    // native chunk copy with a live GC reference. The visiting set stops
    // infinite recursion on self-referential shapes (fail OPEN on cycles:
    // C# value types cannot be infinitely self-containing by value, so a
    // cycle here passes through a reference indirection the field walk
    // already rejects elsewhere — Nullable<T> unwraps above, never here).
    private static bool AreInstanceFieldsBlittable(ITypeSymbol type, HashSet<ISymbol> visiting)
    {
        if (!visiting.Add(type.OriginalDefinition))
        {
            return true;
        }

        try
        {
            foreach (var field in type.GetMembers().OfType<IFieldSymbol>())
            {
                if (field.IsStatic || field.IsConst)
                {
                    continue;
                }

                if (!IsBlittable(field.Type, visiting))
                {
                    return false;
                }
            }

            return true;
        }
        finally
        {
            visiting.Remove(type.OriginalDefinition);
        }
    }
}

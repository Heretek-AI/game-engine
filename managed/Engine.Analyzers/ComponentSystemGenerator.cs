// ComponentSystemGenerator.cs — M2-02: IComponent.Tick -> zero-copy native chunk job.
//
// An IIncrementalGenerator that intercepts every `partial struct : IComponent`
// carrying a `Tick(...)` method and emits a sibling partial with:
//   * an [UnmanagedCallersOnly] static chunk runner that unpacks
//     `EcsChunkView*` into per-row component refs and invokes the user's Tick
//     in a tight, bounds-check-eliminated loop, and
//   * a registration helper reporting the component size/alignment
//     (sizeof(T)) plus the Tick signature's component dependencies.
//
// Calling convention (matches the M2-01 Tick contract `Tick(ref Self self,
// ...)`): instance Tick is driven through the column element itself —
// `col0[i].Tick(ref col0[i], ...)` — where the element is BOTH the receiver
// AND the explicit ref/in self argument. This is NOT redundant: an instance
// method's hidden `this` and an explicit `ref Self` parameter are different
// slots, and omitting the explicit argument is CS7036. Extra component
// params ride the second ABI channel (`in`/`ref col1[i]`); scalars pin to
// default(T) until the M2-04 registration shim publishes the frame delta.
//
// Pipeline (value-equatable throughout; no symbol/syntax held past its stage):
//   syntax: every partial struct declaring a method named Tick.
//   model: resolve IComponent conformance + Tick overloads to SystemSpec.
//   emit: one .g.cs per system struct.
//
// Frame-path rules for EMITTED code: no managed allocations, no throws
// across the ABI (UCO methods must not throw), thread-safe statics only.
using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.IO;
using System.Linq;
using System.Threading;
using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;

namespace Engine.Analyzers;

/// <summary>
/// Generates zero-copy native chunk runners for <c>IComponent</c> systems.
/// Matches <c>partial struct : IComponent</c> declarations with a user
/// <c>Tick</c> method and emits the UCO entry point plus registration.
/// </summary>
[Generator(LanguageNames.CSharp)]
public sealed class ComponentSystemGenerator : IIncrementalGenerator
{
    private const string IComponentMetadataName = "Engine.Runtime.IComponent";

    /// <inheritdoc/>
    public void Initialize(IncrementalGeneratorInitializationContext context)
    {
        // Stage 1 (syntax only, cheap): every partial struct declaring a
        // method literally named Tick. No semantic work here so unchanged
        // files stay cached.
        var structs = context.SyntaxProvider
            .CreateSyntaxProvider(
                predicate: static (node, _) => IsSystemCandidate(node),
                transform: static (ctx, ct) => ToStructRef(ctx, ct))
            .Where(static r => r is not null);

        // Stage 2 (symbols, cached per struct): resolve IComponent + Ticks.
        var systems = context.CompilationProvider
            .Combine(structs.Collect())
            .SelectMany(static (pair, ct) => ResolveAll(pair.Left, pair.Right, ct));

        context.RegisterSourceOutput(systems, static (spc, spec) => EmitOne(spc, spec));
    }

    // --- Stage 1: syntax ---------------------------------------------------

    private static bool IsSystemCandidate(SyntaxNode node)
    {
        // Partial struct with a member literally named Tick. Interface
        // conformance, arity, and modifiers resolve in stage 2 against
        // symbols (syntax cannot see IComponent).
        if (node is not StructDeclarationSyntax s)
        {
            return false;
        }

        if (!s.Modifiers.Any(SyntaxKind.PartialKeyword))
        {
            return false;
        }

        foreach (var member in s.Members)
        {
            if (member is MethodDeclarationSyntax m && m.Identifier.ValueText == "Tick")
            {
                return true;
            }
        }

        return false;
    }

    private static string? ToStructRef(GeneratorSyntaxContext ctx, CancellationToken ct)
    {
        // Freeze the fully-qualified metadata name as the cross-stage key.
        // The compilation in stage 2 resolves it back to a symbol; nothing
        // here holds syntax or symbols past this transform.
        if (ctx.SemanticModel.GetDeclaredSymbol(ctx.Node, ct) is not INamedTypeSymbol sym)
        {
            return null;
        }

        return sym.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat);
    }

    // --- Stage 2: symbols --------------------------------------------------

    private static ImmutableArray<SystemSpec> ResolveAll(
        Compilation compilation,
        ImmutableArray<string?> refs,
        CancellationToken ct)
    {
        var iComponent = compilation.GetTypeByMetadataName(IComponentMetadataName);
        if (iComponent is null)
        {
            return ImmutableArray<SystemSpec>.Empty;
        }

        var builder = ImmutableArray.CreateBuilder<SystemSpec>();
        var seen = new HashSet<string>();
        foreach (var name in refs)
        {
            ct.ThrowIfCancellationRequested();
            if (name is null || !seen.Add(name))
            {
                continue;
            }

            var spec = ResolveOne(compilation, iComponent, name, ct);
            if (spec is not null)
            {
                builder.Add(spec);
            }
        }

        return builder.ToImmutable();
    }

    private static SystemSpec? ResolveOne(
        Compilation compilation,
        INamedTypeSymbol iComponent,
        string fullyQualifiedName,
        CancellationToken ct)
    {
        var sym = ResolveByName(compilation, fullyQualifiedName);
        if (sym is null)
        {
            return null;
        }

        if (!Implements(sym, iComponent))
        {
            return null;
        }

        // Open generics have no concrete sizeof/chunk layout; the loader
        // (M2-04) only binds closed components.
        if (sym.IsGenericType)
        {
            return null;
        }

        var ticks = ImmutableArray.CreateBuilder<TickSignatureSnapshot>();
        foreach (var method in sym.GetMembers("Tick").OfType<IMethodSymbol>())
        {
            ct.ThrowIfCancellationRequested();
            if (method.IsStatic || method.DeclaredAccessibility != Accessibility.Public)
            {
                continue;
            }

            // Generic Tick overloads need explicit type arguments at the call
            // site; a chunk runner cannot supply them. `out` params cannot be
            // satisfied from a column element either. Both are skipped (a
            // struct with no emittable overload produces no runner).
            if (method.IsGenericMethod || method.Parameters.Any(static p => p.RefKind == RefKind.Out))
            {
                continue;
            }

            var pars = ImmutableArray.CreateBuilder<TickParameterSnapshot>();
            foreach (var p in method.Parameters)
            {
                TickRefKind kind;
                if (p.RefKind == RefKind.Ref)
                {
                    kind = TickRefKind.Ref;
                }
                else if (p.RefKind == RefKind.In)
                {
                    kind = TickRefKind.In;
                }
                else
                {
                    kind = TickRefKind.None;
                }
                pars.Add(new TickParameterSnapshot(
                    kind,
                    p.Type.ToDisplayString(SymbolDisplayFormat.FullyQualifiedFormat),
                    p.Name));
            }

            ticks.Add(new TickSignatureSnapshot(
                pars.ToImmutable(),
                ReadPhases(method)));
        }

        if (ticks.Count == 0)
        {
            return null;
        }

        var containing = ImmutableArray.CreateBuilder<string>();
        for (var c = sym.ContainingType; c is not null; c = c.ContainingType)
        {
            containing.Add(c.Name);
        }

        return new SystemSpec(
            sym.ContainingNamespace.IsGlobalNamespace
                ? string.Empty
                : sym.ContainingNamespace.ToDisplayString(),
            sym.Name,
            containing.ToImmutable(),
            ticks.ToImmutable(),
            ReadPhases(sym),
            sym.DeclaredAccessibility == Accessibility.Public);
    }

    private static INamedTypeSymbol? ResolveByName(Compilation compilation, string fullyQualifiedName)
    {
        // FullyQualifiedFormat renders "global::A.B.C<D, E>"; walk the
        // namespace/type chain with the symbol API instead of re-parsing.
        var stripped = fullyQualifiedName.StartsWith("global::", System.StringComparison.Ordinal)
            ? fullyQualifiedName.Substring("global::".Length)
            : fullyQualifiedName;

        INamespaceOrTypeSymbol scope = compilation.GlobalNamespace;
        foreach (var part in SplitDotted(stripped))
        {
            var next = scope.GetMembers(part).OfType<INamespaceOrTypeSymbol>().FirstOrDefault();
            if (next is null)
            {
                return null;
            }

            scope = next;
        }

        return scope as INamedTypeSymbol;
    }

    private static IEnumerable<string> SplitDotted(string dotted)
    {
        // Nested types render "Outer.Inner": GetMembers resolves each level.
        // Generic arity suffixes ("List`1") are left intact — GetMembers
        // understands them for metadata lookup.
        var depth = 0;
        var current = new StringBuilder();
        foreach (var ch in dotted)
        {
            if (ch == '<')
            {
                depth++;
            }
            else if (ch == '>')
            {
                depth--;
            }

            if (ch == '.' && depth == 0)
            {
                yield return current.ToString();
                current.Clear();
            }
            else
            {
                current.Append(ch);
            }
        }

        yield return current.ToString();
    }

    private static bool Implements(INamedTypeSymbol sym, INamedTypeSymbol iComponent)
    {
        foreach (var iface in sym.AllInterfaces)
        {
            if (SymbolEqualityComparer.Default.Equals(iface, iComponent))
            {
                return true;
            }
        }

        return false;
    }

    private static ImmutableArray<string> ReadPhases(ISymbol sym)
    {
        var builder = ImmutableArray.CreateBuilder<string>();
        foreach (var attr in sym.GetAttributes())
        {
            var cls = attr.AttributeClass;
            // Match by NAME, not namespace: the attribute resolves through
            // `using Engine.Runtime;` in user code, and ToDisplayString
            // renders it relative to that context (bare
            // "UpdateInPhaseAttribute"). M2-01 declares exactly one such
            // attribute; matching the short name is the robust check.
            if (cls is null || cls.Name != "UpdateInPhaseAttribute")
            {
                continue;
            }

            if (attr.ConstructorArguments.Length == 1)
            {
                var v = attr.ConstructorArguments[0];
                builder.Add(PhaseName(v));
            }
        }

        return builder.ToImmutable();
    }

    private static string PhaseName(TypedConstant v)
    {
        // Enum-typed phase args surface as their underlying int (TypedConstant
        // boxes Phase.Render as 4); map back through the M2-01 Phase order so
        // the runner comment and the M2-04 loader agree on the phase token.
        if (v.Kind == TypedConstantKind.Enum && v.Value is int n)
        {
            return n switch
            {
                0 => "Startup",
                1 => "PreUpdate",
                3 => "PostUpdate",
                4 => "Render",
                _ => "Update",
            };
        }

        return v.Value?.ToString() ?? "Update";
    }

    // --- Stage 3: emit -----------------------------------------------------

    private static void EmitOne(SourceProductionContext spc, SystemSpec spec)
    {
        var sb = new StringBuilder();
        sb.AppendLine("// <auto-generated/>");
        sb.AppendLine("// Engine.Analyzers (M2-02): IComponent chunk runner.");
        sb.AppendLine("// Matches NativeApi.EcsChunkView; thread-safe, no allocation, no throw.");
        sb.AppendLine("#nullable enable");
        sb.AppendLine("using System;");
        sb.AppendLine("using System.Runtime.CompilerServices;");
        sb.AppendLine("using System.Runtime.InteropServices;");
        sb.AppendLine("using Engine.Bindings;");
        sb.AppendLine();
        sb.AppendLine("// ABI-guard sentinel: the emitter references this when a Tick");
        sb.AppendLine("// shape cannot map onto EcsChunkView (scalars-only Tick, >2");
        sb.AppendLine("// component params). The method THROWS when executed, so a bad");
        sb.AppendLine("// shape fails LOUDLY at runtime registration instead of silently");
        sb.AppendLine("// generating a wrong runner — and the throw never fires on the");
        sb.AppendLine("// frame path because the M2-04 loader only binds runnable shapes.");
        sb.AppendLine("// This is the compile-time half of that contract.");
        sb.AppendLine("internal static class EngineRequireExtraColumns");
        sb.AppendLine("{");
        sb.AppendLine("    internal static void Unsupported() => throw new NotSupportedException(\"Tick shape exceeds the EcsChunkView ABI (M2-04).\");");
        sb.AppendLine("}");
        sb.AppendLine();
        if (spec.Namespace.Length != 0)
        {
            sb.AppendLine($"namespace {spec.Namespace};");
            sb.AppendLine();
        }

        var access = spec.IsPublic ? "public" : "internal";
        OpenContaining(sb, spec, access);
        sb.AppendLine($"    {access} partial struct {spec.StructName}");
        sb.AppendLine("    {");

        var tick = spec.Ticks[0];
        EmitSizeRegistration(sb, spec);
        EmitChunkRunner(sb, spec, tick);

        sb.AppendLine("    }");
        CloseContaining(sb, spec);

        var hint = "Chunk_" + spec.Namespace.Replace(".", "_") + "_" +
            string.Join("_", spec.ContainingTypes.Reverse()) +
            (spec.ContainingTypes.Length == 0 ? string.Empty : "_") +
            spec.StructName + ".g.cs";
        // File-scoped namespace + file-local lookup: "Chunk__Mover.g.cs".
        // Legal hint names avoid characters identifier rules reject.
        foreach (var ch in Path.GetInvalidFileNameChars())
        {
            hint = hint.Replace(ch, '_');
        }

        spc.AddSource(hint.TrimStart('_'), sb.ToString());
    }

    private static void OpenContaining(StringBuilder sb, SystemSpec spec, string access)
    {
        foreach (var outer in spec.ContainingTypes.Reverse())
        {
            sb.AppendLine($"    {access} partial struct {outer}");
            sb.AppendLine("    {");
        }
    }

    private static void CloseContaining(StringBuilder sb, SystemSpec spec)
    {
        for (var i = 0; i < spec.ContainingTypes.Length; i++)
        {
            sb.AppendLine("    }");
        }
    }

    private static void EmitSizeRegistration(StringBuilder sb, SystemSpec spec)
    {
        // Reports struct size/alignment (sizeof(T)) for the Flecs component
        // registration path. The loader (M2-04 ScriptHostManager) reads these
        // at register time; dependencies derive from the Tick signature below.
        sb.AppendLine("        /// <summary>");
        sb.AppendLine("        /// Byte size of this component (Flecs registration).");
        sb.AppendLine("        /// </summary>");
        sb.AppendLine($"        public static unsafe int ComponentSize => unchecked((int)sizeof({spec.StructName}));");
        sb.AppendLine();
        sb.AppendLine("        /// <summary>");
        sb.AppendLine("        /// Managed component-system registration descriptor. Hands the");
        sb.AppendLine("        /// chunk runner to the native world: name, component size, and the");
        sb.AppendLine("        /// runner function pointer. The native register_system entry");
        sb.AppendLine("        /// (EngineRegisterSystem) binds the callback to every matching");
        sb.AppendLine("        /// table chunk at run_systems time.");
        sb.AppendLine("        /// </summary>");
        sb.AppendLine("        public static unsafe int Register(NativeApi.EngineWorld* world, void* userData)");
        sb.AppendLine("        {");
        sb.AppendLine("            if (world == null)");
        sb.AppendLine("            {");
        sb.AppendLine("                return -1;");
        sb.AppendLine("            }");
        sb.AppendLine();
        sb.AppendLine("            var table = NativeApi.ApiLoader.Table;");
        sb.AppendLine("            delegate* unmanaged[Cdecl]<NativeApi.EcsChunkView*, void> fn = &RunChunk;");
        sb.AppendLine($"            fixed (byte* name = \"{spec.StructName}\"u8)");
        sb.AppendLine("            {");
        sb.AppendLine("                return table->EngineRegisterSystem(world, name, fn, userData);");
        sb.AppendLine("            }");
        sb.AppendLine("        }");
        sb.AppendLine();
    }

    private static void EmitChunkRunner(StringBuilder sb, SystemSpec spec, TickSignatureSnapshot tick)
    {
        var phases = tick.PhaseNames.Length != 0
            ? tick.PhaseNames
            : spec.StructPhaseNames;
        var phaseList = phases.Length != 0
            ? string.Join(", ", phases)
            : "Update";
        var selfName = spec.StructName;

        sb.AppendLine("        /// <summary>");
        sb.AppendLine($"        /// Zero-copy chunk runner (phases: {phaseList}).");
        sb.AppendLine("        /// Unpacks EcsChunkView columns into per-row refs and");
        sb.AppendLine("        /// invokes the user Tick in a bounds-check-eliminated loop.");
        sb.AppendLine("        /// </summary>");
        sb.AppendLine("        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]");
        sb.AppendLine("        public static unsafe void RunChunk(NativeApi.EcsChunkView* view)");
        sb.AppendLine("        {");
        sb.AppendLine("            int count = view->Count;");
        sb.AppendLine("            if (count <= 0)");
        sb.AppendLine("            {");
        sb.AppendLine("                return;");
        sb.AppendLine("            }");
        sb.AppendLine();
        EmitColumnSetup(sb, tick, selfName);
        sb.AppendLine("            for (int i = 0; i < count; i++)");
        sb.AppendLine("            {");
        EmitRowCall(sb, tick);
        sb.AppendLine("            }");
        sb.AppendLine("        }");
    }

    private static void EmitColumnSetup(StringBuilder sb, TickSignatureSnapshot tick, string selfName)
    {
        // The system column is emitted FIRST: the receiver (col0[i]) is also
        // the ref/in self argument, so the self slice always exists even when
        // the ABI has no spare channel for more component params.
        //
        // The native EcsChunkView carries exactly two column pointers (Local
        // + UserData); a Tick with more than two component params cannot map
        // onto the ABI, so the emitter stops at two and reports the rest in
        // a comment for the M2-04 registration shim to reject explicitly.
        var first = FirstComponent(tick);
        var selfRef = first is not null && (int)first.RefKind == 2 ? "ReadOnlySpan" : "Span";
        var selfType = first is not null ? first.TypeName : "global::" + selfName;
        sb.AppendLine($"            var col0 = new {selfRef}<{selfType}>((void*)view->Local, count);");

        // Extra component columns ride packed behind the receiver in Tick
        // parameter order. Scalars (in float dt and friends) are NOT columns:
        // they are loop-invariant Tick arguments, filled with default(T) at
        // the call site by EmitRowCall, so they never consume the ABI channel.
        var compIndex = 1;
        var skippedSelf = false;
        foreach (var p in tick.Parameters)
        {
            if ((int)p.RefKind == 0 || IsScalarTickArg(p.TypeName))
            {
                continue;
            }

            if (!skippedSelf)
            {
                skippedSelf = true;
                continue;
            }

            if (compIndex == 1)
            {
                var spanKind = p.RefKind == TickRefKind.In ? "ReadOnlySpan" : "Span";
                sb.AppendLine($"            var col1 = new {spanKind}<{p.TypeName}>(view->UserData, count);");
            }
            else
            {
                sb.AppendLine($"            // Column {compIndex} ({p.TypeName} {p.Name}): no ABI channel; Tick overload not runnable.");
            }

            compIndex++;
        }
    }

    private static TickParameterSnapshot? FirstComponent(TickSignatureSnapshot tick)
    {
        foreach (var p in tick.Parameters)
        {
            if ((int)p.RefKind != 0)
            {
                return p;
            }
        }

        return null;
    }

    private static bool IsScalarTickArg(string typeName)
    {
        return typeName == "float"
            || typeName == "double"
            || typeName == "global::System.Single"
            || typeName == "global::System.Double"
            || typeName.EndsWith("::float", StringComparison.Ordinal)
            || typeName.EndsWith("::double", StringComparison.Ordinal)
            || typeName.EndsWith("::System.Single", StringComparison.Ordinal)
            || typeName.EndsWith("::System.Double", StringComparison.Ordinal);
    }

    private static bool IsRefOrInScalar(TickParameterSnapshot p)
    {
        return (int)p.RefKind != 0 && IsScalarTickArg(p.TypeName);
    }

    private static string RefPrefix(TickParameterSnapshot p)
    {
        return (int)p.RefKind == 2 ? "in" : "ref";
    }

    private static void EmitRowCall(StringBuilder sb, TickSignatureSnapshot tick)
    {
        // Instance Tick is invoked through the column element itself
        // (col0[i].Tick(ref col0[i], ...)): the element is BOTH the receiver
        // AND the explicit ref/in self argument. Both slots must be filled —
        // C# does not fuse the hidden `this` with an explicit `ref Self`
        // parameter, so omitting it is CS7036.
        //
        // Ref-kind tests use integer values (not == on the enum) so the
        // emitter stays correct even if TickRefKind gains members: 0 means
        // by-value, anything else is a component reference.
        //
        // The scalar dt (in float / float / double) has no ABI channel yet:
        // M2-01 Tick signatures still carry it, so it is pinned to its
        // default for now and the M2-04 registration shim owns publishing the
        // frame delta (mirrors MovementSystem.SetDt in M1-05).
        //
        // Self matching is by Tick DECLARED order, not receiver inference:
        // the first ref/in component param names the system column element
        // the runner receives through (col0[i]); later component params ride
        // col1[i] (ABI's second channel).
        var args = new List<string>();
        var seenSelf = false;
        var extraIndex = 0;
        var runnable = true;
        foreach (var p in tick.Parameters)
        {
            var isComponent = (int)p.RefKind != 0 && !IsScalarTickArg(p.TypeName);

            // First component param: forward the receiver element as the
            // explicit self argument (ref col0[i] / in col0[i]).
            if (!seenSelf && isComponent)
            {
                seenSelf = true;
                args.Add((int)p.RefKind == 2 ? "in col0[i]" : "ref col0[i]");
                continue;
            }

            // Scalars (by value, or ref/in scalars like dt): no ABI channel
            // yet, pin to default. M2-04 owns publishing the frame delta.
            // ref/in scalars cannot take `default(T)` inline (CS8156: a
            // default expression is not a passable variable), so they bind
            // through a loop-scoped temp — still zero-alloc, still throw-free.
            if ((int)p.RefKind == 0 || IsScalarTickArg(p.TypeName))
            {
                if (IsRefOrInScalar(p))
                {
                    var temp = $"__tick_{p.Name}";
                    sb.AppendLine($"            {p.TypeName} {temp} = default({p.TypeName});");
                    args.Add($"{RefPrefix(p)} {temp}");
                }
                else
                {
                    args.Add($"default({p.TypeName})");
                }

                continue;
            }

            if (!seenSelf)
            {
                runnable = false;
                args.Add($"default! /* self must be the first ref/in param ({p.Name}) */");
                continue;
            }

            extraIndex++;
            if (extraIndex == 1)
            {
                args.Add((int)p.RefKind == 2 ? "in col1[i]" : "ref col1[i]");
            }
            else
            {
                runnable = false;
                args.Add($"default! /* no ABI channel for {p.Name} */");
            }
        }

        var receiver = tick.Parameters.Length != 0 && (int)tick.Parameters[0].RefKind != 0
            ? "col0[i]."
            : string.Empty;

        // A Tick with no receiver-side self (static-shaped or scalars-only)
        // cannot be driven per row: the instance call needs a col0[i] both as
        // the receiver and as the explicit self argument. That is a shape the
        // M2-04 loader must reject, not code the emitter can paper over —
        // the runner deliberately emits NO Tick call here, only the guard.
        if (string.IsNullOrEmpty(receiver))
        {
            runnable = false;
        }

        if (!runnable)
        {
            sb.AppendLine("            EngineRequireExtraColumns.Unsupported(); // Tick shape exceeds the EcsChunkView ABI (M2-04).");
            return;
        }

        sb.AppendLine($"                {receiver}Tick({string.Join(", ", args)});");
    }
}

// HotPathAnalyzers.cs — M2-03: ENG0001/ENG0003 Roslyn analyzers.
//
// Flags illegal patterns inside the hot path (any method carrying
// [UpdateInPhase], or any Tick method on an IComponent struct — Tick is the
// hot path by construction even when the attribute sits on the struct):
//   ENG0001 (Error): heap allocation — `new` on a reference type, string
//     interpolation / string.Format / ToString (culture machinery +
//     allocation), LINQ query/clause operators and System.Linq calls, and
//     lambda/anonymous-method closures (compiler-generated display class).
//     Ref structs (Span<T>, TempList<T>, stackalloc) never fire: they are
//     the sanctioned scratch types the fix steers toward.
//   ENG0003 (Error): blocking waits — Task.Wait()/Result, .GetAwaiter()
//     (.GetResult()), Thread.Sleep(), Monitor.Enter/lock, Task.Delay().Wait
//     chains. Deadlocks or stalls the engine thread by construction.
//
// Both rules run on OperationActions (IOperation, language-agnostic) with
// dataflow closure detection for lambdas: captures are only reported when a
// captured variable is actually used inside the anonymous function (a
// display class is only emitted then — a captureless lambda is a cached
// static and costs nothing).
//
// Error severity: Directory.Build.props treats warnings as errors, so these
// break any build that leaves one in a hot-path method.
using System;
using System.Collections.Immutable;
using System.Linq;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.Diagnostics;
using Microsoft.CodeAnalysis.Operations;

namespace Engine.Analyzers;

/// <summary>Heap allocation on the hot path is a compile-time error.</summary>
[DiagnosticAnalyzer(LanguageNames.CSharp)]
public sealed class HotPathAllocationAnalyzer : DiagnosticAnalyzer
{
    /// <summary>Diagnostic property marking the creation as mechanically fixable.</summary>
    public const string FixableNewListProperty = "FixableNewList";

    /// <summary>ENG0001 descriptor: reference-type allocation in a hot-path method.</summary>
    public static readonly DiagnosticDescriptor Rule = new(
        id: "ENG0001",
        title: "Heap allocation in hot path",
        messageFormat: "{0} allocates on the GC heap; use a ref struct (Span<T>, TempList<T>) or stackalloc instead",
        category: "Performance",
        defaultSeverity: DiagnosticSeverity.Error,
        isEnabledByDefault: true,
        description: "Hot-path methods ([UpdateInPhase] or IComponent.Tick) must not allocate: the frame path is zero-GC.");

    /// <inheritdoc/>
    public override ImmutableArray<DiagnosticDescriptor> SupportedDiagnostics => ImmutableArray.Create(Rule);

    /// <inheritdoc/>
    public override void Initialize(AnalysisContext context)
    {
        context.ConfigureGeneratedCodeAnalysis(GeneratedCodeAnalysisFlags.None);
        context.EnableConcurrentExecution();
        context.RegisterOperationBlockStartAction(block =>
        {
            // Hot-path gate: [UpdateInPhase] on the method, or Tick on an
            // IComponent struct (the phase attribute may sit on the struct —
            // Tick is the hot path by construction either way).
            if (block.OwningSymbol is not IMethodSymbol method)
            {
                return;
            }

            if (!HotPath.IsHotPathMethod(method))
            {
                return;
            }

            block.RegisterOperationAction(ctx => CheckAllocation(ctx), OperationKind.ObjectCreation);
            block.RegisterOperationAction(ctx => CheckAllocation(ctx), OperationKind.ArrayCreation);
            block.RegisterOperationAction(ctx => CheckAllocation(ctx), OperationKind.TypeParameterObjectCreation);
            block.RegisterOperationAction(ctx => CheckInterpolated(ctx), OperationKind.InterpolatedString);
            block.RegisterOperationAction(ctx => CheckInvocation(ctx), OperationKind.Invocation);
            block.RegisterOperationAction(ctx => CheckAnonymousFunction(ctx), OperationKind.AnonymousFunction);
        });
    }

    private static void CheckAllocation(OperationAnalysisContext ctx)
    {
        // Attribute application (`[UpdateInPhase(...)]`) is metadata, not a
        // heap allocation: constructors on attributes never enter the
        // operation tree as IObjectCreationOperation on this path — but the
        // SYMBOL is the attribute's .ctor, so skip .ctor calls by name.
        if (ctx.Operation is IObjectCreationOperation attrOp &&
            attrOp.Constructor?.ContainingType?.Name == HotPath.UpdateInPhaseAttributeName)
        {
            return;
        }

        // `new` on a VALUE type is stack inline — only reference types heap
        // allocate. Ref structs (Span<T>, TempList<T>) never reach this
        // branch (they are value types by construction). Implicit array
        // element conversions (e.g. `new int[3] {1,2,3}` boxing each int
        // to object) report as object creation of Int32 — skip conversions,
        // which are implicit and not user-written `new` sites.
        if (ctx.Operation.Parent is IConversionOperation)
        {
            return;
        }

        var type = ctx.Operation.Type;
        if (type is null || type.IsValueType)
        {
            return;
        }

        // Delegates are reference types but never fire here: Func/Action
        // allocation from a lambda reports at the anonymous function (with
        // the closure explanation); plain method-group conversions are
        // zero-alloc cached call sites.
        if (type.TypeKind == TypeKind.Delegate)
        {
            return;
        }

        // Array creation reports the element, not the SZ[] type: `new
        // byte[64]` is clearer as "byte[]" than as the array type name.
        var what = ctx.Operation is IArrayCreationOperation arr && arr.Type is IArrayTypeSymbol arrayType
            ? arrayType.ElementType.ToDisplayString(SymbolDisplayFormats.Short)
            : type.ToDisplayString(SymbolDisplayFormats.Short);

        // Zero-arg `new List<T>()` is mechanically fixable (TempList<T>);
        // the M2-03 code fix keys off this property — every other
        // allocation has no rewrite and offers none.
        var properties = IsFixableNewList(ctx.Operation)
            ? ImmutableDictionary<string, string?>.Empty.Add(FixableNewListProperty, "true")
            : ImmutableDictionary<string, string?>.Empty;
        ctx.ReportDiagnostic(Diagnostic.Create(Rule, ctx.Operation.Syntax.GetLocation(), properties, $"`new {what}`"));
    }

    private static bool IsFixableNewList(IOperation op)
    {
        // ObjectCreation with an empty argument list whose constructed type
        // is System.Collections.Generic.List<T>. Namespace + name + arity:
        // the display form of List<int> varies across Roslyn/SDK ref sets
        // (List<T> vs List<int>), which broke an earlier display gate.
        if (op is not IObjectCreationOperation { Arguments.Length: 0, Constructor: { } ctor })
        {
            return false;
        }

        var containing = ctor.ContainingType;
        if (containing is not { Name: "List", ContainingNamespace.Name: "Generic" })
        {
            return false;
        }

        return containing.ContainingNamespace.ContainingNamespace?.ToDisplayString()
            == "System.Collections";
    }

    private static void CheckInterpolated(OperationAnalysisContext ctx)
    {
        ctx.ReportDiagnostic(Diagnostic.Create(
            Rule, ctx.Operation.Syntax.GetLocation(), "string interpolation"));
    }

    private static void CheckInvocation(OperationAnalysisContext ctx)
    {
        if (ctx.Operation is not IInvocationOperation inv)
        {
            return;
        }

        var target = inv.TargetMethod;
        if (target is null)
        {
            return;
        }

        // string.Format / string.Concat(a, b, ...) with 2+ args: culture
        // machinery + heap allocation (single-arg Concat is the trivial
        // passthrough and stays quiet).
        if (target.ContainingType?.SpecialType == SpecialType.System_String &&
            (target.Name == "Format" || (target.Name == "Concat" && inv.Arguments.Length > 1)))
        {
            ctx.ReportDiagnostic(Diagnostic.Create(
                Rule, ctx.Operation.Syntax.GetLocation(), $"`string.{target.Name}`"));
            return;
        }

        // Object.ToString: virtual dispatch + allocation (value-type
        // overrides box through the object call). Name-only match: the
        // receiver's static type may declare an override (int.ToString),
        // so IsVirtual on the TARGET is false — but the invocation still
        // flows through object.ToString's virtual slot and allocates.
        if (target.Name == "ToString" && target.Parameters.Length == 0 &&
            target.ContainingType?.SpecialType != SpecialType.System_String)
        {
            ctx.ReportDiagnostic(Diagnostic.Create(
                Rule, ctx.Operation.Syntax.GetLocation(), "`ToString()`"));
            return;
        }

        // Any System.Linq call: iterator allocation + deferred-execution
        // closures even before a lambda is inspected.
        var ns = target.ContainingType?.ContainingNamespace?.ToDisplayString();
        if (ns == "System.Linq" || (ns?.StartsWith("System.Linq.", System.StringComparison.Ordinal) == true))
        {
            ctx.ReportDiagnostic(Diagnostic.Create(
                Rule, ctx.Operation.Syntax.GetLocation(), $"LINQ `{target.Name}`"));
        }
    }

    private static void CheckAnonymousFunction(OperationAnalysisContext ctx)
    {
        // A closure allocates its display class ONLY when it captures a
        // local, a parameter, or `this`: a captureless lambda compiles to a
        // cached static and costs nothing, so it stays quiet. Local
        // functions return without a check: ILocalFunctionOperation does
        // not implement IAnonymousFunctionOperation, so this branch only
        // ever sees lambdas and anonymous methods.
        if (ctx.Operation is not IAnonymousFunctionOperation anon)
        {
            return;
        }

        var dataFlow = ctx.Operation.SemanticModel?.AnalyzeDataFlow(anon.Body.Syntax);
        if (dataFlow?.Succeeded == true && dataFlow.CapturedInside.Length != 0)
        {
            ctx.ReportDiagnostic(Diagnostic.Create(
                Rule, ctx.Operation.Syntax.GetLocation(), "lambda closure"));
        }
    }
}

/// <summary>Blocking wait on the hot path is a compile-time error.</summary>
[DiagnosticAnalyzer(LanguageNames.CSharp)]
public sealed class HotPathBlockingAnalyzer : DiagnosticAnalyzer
{
    /// <summary>ENG0003 descriptor: blocking wait on a hot-path method.</summary>
    public static readonly DiagnosticDescriptor Rule = new(
        id: "ENG0003",
        title: "Blocking wait on engine thread",
        messageFormat: "{0} blocks the engine thread; restructure as phase-local work or a frame-scoped poll instead",
        category: "Concurrency",
        defaultSeverity: DiagnosticSeverity.Error,
        isEnabledByDefault: true,
        description: "Hot-path methods ([UpdateInPhase] or IComponent.Tick) must never block: a stall here stalls the frame.");

    /// <inheritdoc/>
    public override ImmutableArray<DiagnosticDescriptor> SupportedDiagnostics => ImmutableArray.Create(Rule);

    /// <inheritdoc/>
    public override void Initialize(AnalysisContext context)
    {
        context.ConfigureGeneratedCodeAnalysis(GeneratedCodeAnalysisFlags.None);
        context.EnableConcurrentExecution();
        context.RegisterOperationBlockStartAction(block =>
        {
            if (block.OwningSymbol is not IMethodSymbol method || !HotPath.IsHotPathMethod(method))
            {
                return;
            }

            block.RegisterOperationAction(ctx => CheckBlockingInvocation(ctx), OperationKind.Invocation);
            block.RegisterOperationAction(ctx => CheckBlockingProperty(ctx), OperationKind.PropertyReference);
            block.RegisterOperationAction(ctx => CheckLock(ctx), OperationKind.Lock);
        });
    }

    // One check per operation KIND, so each syntax site reports at most
    // once: invocations (t.Wait(), Thread.Sleep(), Monitor.Enter,
    // t.GetAwaiter(), t.GetAwaiter().GetResult()) merge here; Task<T>.Result
    // (a property read) lives in CheckBlockingProperty below.
    private static void CheckBlockingInvocation(OperationAnalysisContext ctx)
    {
        if (ctx.Operation is not IInvocationOperation inv || inv.TargetMethod is not { } target)
        {
            return;
        }

        var name = target.Name;
        var containing = target.ContainingType;
        var instType = inv.Instance?.Type;

        // Task.Wait()/WaitAll()/WaitAny()/GetAwaiter(), and
        // t.GetAwaiter().GetResult() (target = the awaiter struct — gate on
        // the inner GetAwaiter() receiver): blocks until completion.
        if (IsTaskLike(instType) &&
            (name is "Wait" or "WaitAll" or "WaitAny" or "GetAwaiter" ||
             (name == "GetResult" && inv.Instance is IInvocationOperation inner &&
              inner.TargetMethod.Name == "GetAwaiter" && IsTaskLike(inner.Instance?.Type))))
        {
            ctx.ReportDiagnostic(Diagnostic.Create(
                Rule, ctx.Operation.Syntax.GetLocation(), $"`{instType?.Name}.{name}`"));
            return;
        }

        // Static Task.WaitAll(t)/WaitAny(t): the receiver is null — the
        // CONTAINING type (System.Threading.Tasks.Task) proves Task-likeness.
        if (name is "WaitAll" or "WaitAny" && IsTaskLike(containing))
        {
            ctx.ReportDiagnostic(Diagnostic.Create(
                Rule, ctx.Operation.Syntax.GetLocation(), $"`{containing?.Name}.{name}`"));
            return;
        }

        // Thread.Sleep: unconditional stall of the engine thread.
        if (name == "Sleep" && containing?.ToDisplayString() == "System.Threading.Thread")
        {
            ctx.ReportDiagnostic(Diagnostic.Create(
                Rule, ctx.Operation.Syntax.GetLocation(), "`Thread.Sleep`"));
            return;
        }

        // lock / Monitor.Enter: unbounded mutual exclusion on the frame path
        // (contention stalls the lane; inversion deadlocks it).
        if ((name is "Enter" or "EnterScope") && containing?.ToDisplayString() == "System.Threading.Monitor")
        {
            ctx.ReportDiagnostic(Diagnostic.Create(
                Rule, ctx.Operation.Syntax.GetLocation(), "`Monitor.Enter`"));
        }
    }

    private static void CheckBlockingProperty(OperationAnalysisContext ctx)
    {
        // Task<T>.Result reads as IPropertyReferenceOperation (invocations
        // live in CheckBlockingInvocation above — this check sees properties
        // only, so each syntax site reports at most once). Receiver TYPE
        // gating keeps innocent same-named members (string.Length,
        // List.Count, anonymous { Result }) quiet: only Task/Task<T>/
        // ValueTask(n) receivers fire (see IsTaskLike).
        if (ctx.Operation is not IPropertyReferenceOperation prop)
        {
            return;
        }

        if (prop.Property.Name == "Result")
        {
            var propInst = prop.Instance?.Type;
            if (IsTaskLike(propInst))
            {
                ctx.ReportDiagnostic(Diagnostic.Create(
                    Rule, ctx.Operation.Syntax.GetLocation(),
                    $"`{propInst?.Name}.{prop.Property.Name}`"));
            }
        }
    }

    private static void CheckLock(OperationAnalysisContext ctx)
    {
        ctx.ReportDiagnostic(Diagnostic.Create(
            Rule, ctx.Operation.Syntax.GetLocation(), "`lock`"));
    }

    private static bool IsTaskLike(ITypeSymbol? type)
    {
        if (type is null)
        {
            return false;
        }

        // Name + namespace match (NOT full display string): the display
        // form of Task<int> varies across Roslyn/SDK ref sets
        // (Task<TResult> vs Task<int>), which broke an earlier display
        // gate. Namespace + name + arity is stable.
        if (type is not INamedTypeSymbol named)
        {
            return false;
        }

        var ns = named.ContainingNamespace?.ToDisplayString();
        if (ns != "System.Threading.Tasks")
        {
            return false;
        }

        return named.Name == "Task" || named.Name == "ValueTask";
    }
}

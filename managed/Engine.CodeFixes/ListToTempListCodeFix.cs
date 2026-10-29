// ListToTempListCodeFix.cs — M2-03: ENG0001 code fix.
//
// Replaces `new List<T>()` with `new TempList<T>()` plus the `using
// Engine.Runtime;` the new name needs. Scoped narrowly by construction:
// only zero-argument `new List<T>` object creation reports a fixable
// property (FixableNewList) in its ENG0001 diagnostic, and only those
// diagnostics reach this provider — every other ENG0001 allocation
// (interpolation, LINQ, closures, parameterized constructors) has no
// mechanical rewrite and offers no fix.
//
// The fix preserves the user's type argument and trivia, adds the using
// only when no `using Engine.Runtime;` (any spelling) is already present,
// and annotates with Formatter so the IDE normalizes whitespace.
using System.Collections.Immutable;
using System.Composition;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CodeActions;
using Microsoft.CodeAnalysis.CodeFixes;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Formatting;

namespace Engine.CodeFixes;

/// <summary>Replaces zero-arg <c>new List&lt;T&gt;()</c> with <c>new TempList&lt;T&gt;()</c>.</summary>
[ExportCodeFixProvider(LanguageNames.CSharp, Name = nameof(ListToTempListCodeFixProvider))]
[Shared]
public sealed class ListToTempListCodeFixProvider : CodeFixProvider
{
    /// <summary>Mirrors HotPathAllocationAnalyzer.FixableNewListProperty (string-coupled by design).</summary>
    public const string FixableProperty = "FixableNewList";

    /// <inheritdoc/>
    public override ImmutableArray<string> FixableDiagnosticIds => ImmutableArray.Create("ENG0001");

    /// <inheritdoc/>
    public override FixAllProvider GetFixAllProvider() => WellKnownFixAllProviders.BatchFixer;

    /// <inheritdoc/>
    public override async Task RegisterCodeFixesAsync(CodeFixContext context)
    {
        var diagnostic = context.Diagnostics.FirstOrDefault(static d =>
            d.Id == "ENG0001" && d.Properties.ContainsKey(FixableProperty));
        if (diagnostic is null)
        {
            return;
        }

        var root = await context.Document.GetSyntaxRootAsync(context.CancellationToken).ConfigureAwait(false);
        if (root?.FindNode(diagnostic.Location.SourceSpan) is not ObjectCreationExpressionSyntax creation
            || creation.ArgumentList?.Arguments.Count != 0
            || creation.Type is not GenericNameSyntax)
        {
            return;
        }

        context.RegisterCodeFix(
            CodeAction.Create(
                title: "Replace with TempList<T>",
                createChangedDocument: ct => ReplaceWithTempListAsync(context.Document, creation, ct),
                equivalenceKey: "ENG0001_ReplaceWithTempList"),
            diagnostic);
    }

    private static async Task<Document> ReplaceWithTempListAsync(
        Document document, ObjectCreationExpressionSyntax creation, CancellationToken ct)
    {
        var tempList = SyntaxFactory.GenericName("TempList")
            .WithTypeArgumentList(((GenericNameSyntax)creation.Type).TypeArgumentList)
            .WithAdditionalAnnotations(Formatter.Annotation);
        var replacement = creation.WithType(tempList).WithAdditionalAnnotations(Formatter.Annotation);

        var root = await document.GetSyntaxRootAsync(ct).ConfigureAwait(false);
        if (root is null)
        {
            return document;
        }

        var replaced = root.ReplaceNode(creation, replacement);

        // Add `using Engine.Runtime;` only when no spelling of it exists
        // (dotted prefix forms like `using Engine.Runtime.Sub;` still need
        // the exact using — a prefix does not import the parent).
        SyntaxNode newRoot = replaced;
        if (replaced is CompilationUnitSyntax unit &&
            !unit.Usings.Any(static u => u.Name?.ToString() == "Engine.Runtime"))
        {
            var directive = SyntaxFactory.UsingDirective(SyntaxFactory.ParseName("Engine.Runtime"))
                .WithAdditionalAnnotations(Formatter.Annotation);
            newRoot = unit.AddUsings(directive);
        }

        return document.WithSyntaxRoot(newRoot);
    }
}

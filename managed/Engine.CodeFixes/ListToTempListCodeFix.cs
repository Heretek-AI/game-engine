// ListToTempListCodeFix.cs — M2-03: ENG0001 code fix.
//
// Replaces `var xs = new List<T>();` with a stack-backed TempList:
//
//     var xs = new List<int>();
//       → Span<int> xsBuffer = stackalloc int[4];
//         var xs = new TempList<int>(xsBuffer);
//
// Scoped narrowly by construction: only zero-argument `new List<T>` object
// creation with an unmanaged T, assigned to a `var` local, reports a fixable
// property (FixableNewList) in its ENG0001 diagnostic — and only those
// diagnostics reach this provider. Every other ENG0001 allocation
// (interpolation, LINQ, closures, parameterized constructors, managed T,
// explicit `List<T>` declared type) has no mechanical rewrite and offers
// no fix.
//
// Why the buffer matters: TempList<T> is a (pointer, count, capacity) view
// with no parameterless ctor — a bare `new TempList<T>()` compiles to a
// zeroed struct with capacity 0 where every Add returns false and silently
// drops elements. The emitted stackalloc gives the list a real buffer
// (capacity 4 is a TODO placeholder the user sizes to their loop).
//
// The fix preserves the user's type argument, inserts the two statements in
// place of the original declaration, adds `using Engine.Runtime;` only when
// no spelling of it exists, and annotates with Formatter so the IDE
// normalizes whitespace.
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

/// <summary>
/// Replaces zero-arg <c>new List&lt;T&gt;()</c> in a <c>var</c> local with a
/// stack-backed <c>TempList&lt;T&gt;</c>.
/// </summary>
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
            || creation.Type is not GenericNameSyntax generic)
        {
            return;
        }

        // Refuse unless the creation sits in a `var x = new List<T>();` local
        // declaration — the rewrite replaces the whole declaration (it needs
        // to insert the buffer statement), so anything else (field, return,
        // argument, explicit type) has no valid rewrite site.
        if (creation.Parent is not EqualsValueClauseSyntax init
            || init.Parent is not VariableDeclaratorSyntax declarator
            || declarator.Parent is not VariableDeclarationSyntax declaration
            || !declaration.Type.IsVar
            || declaration.Variables.Count != 1)
        {
            return;
        }

        context.RegisterCodeFix(
            CodeAction.Create(
                title: "Replace with TempList<T>",
                createChangedDocument: ct => ReplaceWithTempListAsync(
                    context.Document, creation, declarator, generic, ct),
                equivalenceKey: "ENG0001_ReplaceWithTempList"),
            diagnostic);
    }

    private static async Task<Document> ReplaceWithTempListAsync(
        Document document,
        ObjectCreationExpressionSyntax creation,
        VariableDeclaratorSyntax declarator,
        GenericNameSyntax generic,
        CancellationToken ct)
    {
        var root = await document.GetSyntaxRootAsync(ct).ConfigureAwait(false);
        if (root is null)
        {
            return document;
        }

        var elementType = generic.TypeArgumentList.Arguments[0];
        var localName = declarator.Identifier.WithoutTrivia().Text;
        var bufferName = localName + "Buffer";

        // Span<T> xsBuffer = stackalloc T[4];
        var bufferDecl =
            $"global::System.Span<{elementType}> {bufferName} = stackalloc {elementType}[4];";
        var bufferStatement = SyntaxFactory.ParseStatement(bufferDecl)
            .WithAdditionalAnnotations(Formatter.Annotation);

        // var xs = new TempList<T>(xsBuffer);
        var tempListType = SyntaxFactory.GenericName("TempList")
            .WithTypeArgumentList(generic.TypeArgumentList)
            .WithAdditionalAnnotations(Formatter.Annotation);
        var tempListCreation = creation
            .WithType(tempListType)
            .WithArgumentList(SyntaxFactory.ArgumentList(SyntaxFactory.SeparatedList(
            [
                SyntaxFactory.Argument(SyntaxFactory.IdentifierName(bufferName)),
            ])))
            .WithAdditionalAnnotations(Formatter.Annotation);
        var listStatement = SyntaxFactory.LocalDeclarationStatement(
            SyntaxFactory.VariableDeclaration(
                SyntaxFactory.IdentifierName("var"),
                SyntaxFactory.SeparatedList(
                [
                    SyntaxFactory.VariableDeclarator(declarator.Identifier.WithoutTrivia())
                        .WithInitializer(SyntaxFactory.EqualsValueClause(tempListCreation)),
                ])))
            .WithAdditionalAnnotations(Formatter.Annotation);

        var newRoot = root.ReplaceNode(
            declarationOf(declarator),
            new SyntaxNode[] { bufferStatement, listStatement });

        // Add `using Engine.Runtime;` only when no spelling of it exists
        // (dotted prefix forms like `using Engine.Runtime.Sub;` still need
        // the exact using — a prefix does not import the parent).
        if (newRoot is CompilationUnitSyntax unit &&
            !unit.Usings.Any(static u => u.Name?.ToString() == "Engine.Runtime"))
        {
            var directive = SyntaxFactory.UsingDirective(SyntaxFactory.ParseName("Engine.Runtime"))
                .WithAdditionalAnnotations(Formatter.Annotation);
            newRoot = unit.AddUsings(directive);
        }

        return document.WithSyntaxRoot(newRoot);

        static StatementSyntax declarationOf(VariableDeclaratorSyntax d) =>
            (StatementSyntax)d.Parent!.Parent!;
    }
}

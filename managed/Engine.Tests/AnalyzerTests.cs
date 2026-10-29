// AnalyzerTests.cs — M2-03 acceptance gate: ENG0001/ENG0002/ENG0003 fire
// exactly where the issue demands, stay quiet where they must, and the
// ENG0001 code fix rewrites `new List<T>()` to `new TempList<T>()`.
//
// Strategy: compile small in-memory fixtures with CSharpCompilation,
// run the REAL analyzer assembly (loaded from its build output — no
// path pinning beyond the sibling bin layout), and assert diagnostic
// counts. Fixture sources are strings, so the dogfood analyzers wired
// over this project never fire on the test code itself.
using System.Collections.Immutable;
using System.Reflection;
using Engine.Runtime;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CodeActions;
using Microsoft.CodeAnalysis.CodeFixes;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.Diagnostics;
using Microsoft.CodeAnalysis.Text;

namespace Engine.Tests;

public static class AnalyzerTests
{
    public static async Task<int> Run()
    {
        var failures = 0;

        // --- ENG0001: positives (5 = new List, interpolation, Range, ToList, closure) ---
        const string hotAlloc = """
            using System;
            using System.Collections.Generic;
            using System.Linq;
            using Engine.Runtime;
            [UpdateInPhase(Phase.Update)]
            public partial struct HotAlloc : IComponent
            {
                public int Value;
                public void Tick(ref HotAlloc self)
                {
                    var list = new List<int>();
                    var text = $"value {self.Value}";
                    var q = Enumerable.Range(0, 3).ToList();
                    int k = self.Value;
                    Func<int> f = () => k;
                    _ = (list, text, q, f);
                }
            }
            """;
        failures += Check(Count(hotAlloc, "ENG0001") == 5, $"ENG0001 hot path: 5 diagnostics (got {Count(hotAlloc, "ENG0001")})");

        const string hotStrings = """
            using Engine.Runtime;
            [UpdateInPhase(Phase.Update)]
            public partial struct HotStrings : IComponent
            {
                public int Value;
                public void Tick(ref HotStrings self)
                {
                    var a = string.Format("{0}", self.Value);
                    var b = self.Value.ToString();
                    _ = (a, b);
                }
            }
            """;
        failures += Check(Count(hotStrings, "ENG0001") == 2, $"ENG0001 Format+ToString: 2 (got {Count(hotStrings, "ENG0001")})");

        // --- ENG0001: negatives (value types, stackalloc, captureless lambda, cold code) ---
        const string coldOk = """
            using System;
            using System.Collections.Generic;
            using Engine.Runtime;
            public partial struct ColdOk : IComponent
            {
                public int Value;
                public void Tick(ref ColdOk self)
                {
                    int x = self.Value + 1;
                    Span<int> buf = stackalloc int[8];
                    buf[0] = x;
                    Func<int> g = () => 42;
                    _ = (buf[0], g);
                }
            }
            public class Cold
            {
                public void Work()
                {
                    var l = new List<int>();
                    _ = l;
                }
            }
            """;
        failures += Check(Count(coldOk, "ENG0001") == 0, $"ENG0001 quiet on structs/stackalloc/captureless/cold (got {Count(coldOk, "ENG0001")})");

        // --- ENG0001: conversion-wrapped allocation still fires (review catch) ---
        const string convAlloc = """
            using System;
            using System.Collections.Generic;
            using Engine.Runtime;
            [UpdateInPhase(Phase.Update)]
            public partial struct ConvAlloc : IComponent
            {
                public int Value;
                public void Tick(ref ConvAlloc self, Action<object> sink)
                {
                    sink(new List<int>());
                }
            }
            """;
        failures += Check(Count(convAlloc, "ENG0001") == 1, $"ENG0001 conversion-wrapped new still fires: 1 (got {Count(convAlloc, "ENG0001")})");

        // --- ENG0002: positives (string, object, array, List) ---
        const string badComp = """
            using System.Collections.Generic;
            using Engine.Runtime;
            public partial struct BadComp : IComponent
            {
                public string Name;
                public object Tag;
                public int[] Scores;
                public List<int> Items;
            }
            """;
        failures += Check(Count(badComp, "ENG0002") == 4, $"ENG0002 non-blittable fields: 4 (got {Count(badComp, "ENG0002")})");

        // --- ENG0002: nested-struct smuggling (Sourcery review catch) ---
        const string nestedBad = """
            using Engine.Runtime;
            public struct Inner
            {
                public string Smuggled;
            }
            public partial struct Outer : IComponent
            {
                public Inner Payload;
            }
            """;
        failures += Check(Count(nestedBad, "ENG0002") == 1, $"ENG0002 nested struct with string: 1 (got {Count(nestedBad, "ENG0002")})");

        // --- ENG0002: negatives (blittable, Nullable<int>, statics, plain structs) ---
        const string goodComp = """
            using Engine.Runtime;
            public partial struct GoodComp : IComponent
            {
                public int Value;
                public Entity Target;
                public FixedString64 Label;
                public int? Maybe;
                public static string Cache;
                public void Tick(ref GoodComp self) { self.Value++; }
            }
            public struct Plain
            {
                public string S;
            }
            """;
        failures += Check(Count(goodComp, "ENG0002") == 0, $"ENG0002 quiet on blittable/nullable/static/plain (got {Count(goodComp, "ENG0002")})");

        // --- ENG0003: positives (Wait, Result, Sleep, lock — exactly one each) ---
        const string hotBlock = """
            using System.Threading;
            using System.Threading.Tasks;
            using Engine.Runtime;
            [UpdateInPhase(Phase.Update)]
            public partial struct HotBlock : IComponent
            {
                public int Value;
                public void Tick(ref HotBlock self)
                {
                    Task.Delay(1).Wait();
                    var n = Task.FromResult(self.Value).Result;
                    Thread.Sleep(1);
                    _ = n;
                }
                [UpdateInPhase(Phase.PostUpdate)]
                public void TickLock(ref HotBlock self, object gate)
                {
                    lock (gate) { self.Value++; }
                }
            }
            """;
        failures += Check(Count(hotBlock, "ENG0003") == 4, $"ENG0003 blocking waits: 4, one per site (got {Count(hotBlock, "ENG0003")})");

        // --- ENG0003: negatives (await, cold waits, non-task .Result) ---
        const string coldAsync = """
            using System.Threading.Tasks;
            public class ColdAsync
            {
                public async Task<int> FetchAsync()
                {
                    await Task.Delay(1);
                    return 1;
                }
                public void Cold()
                {
                    Task.Delay(1).Wait();
                }
                public int Read()
                {
                    var anon = new { Result = 5 };
                    return anon.Result;
                }
            }
            """;
        failures += Check(Count(coldAsync, "ENG0003") == 0, $"ENG0003 quiet on await/cold/non-task Result (got {Count(coldAsync, "ENG0003")})");

        // --- ENG0003: struct-level attribute covers Tick only (review catch) ---
        // Tick stays hot (interpolation fires); the ctor and the ToString
        // override are cold even on a scheduled type.
        const string structCold = """
            using System.Collections.Generic;
            using Engine.Runtime;
            [UpdateInPhase(Phase.Update)]
            public partial struct StructCold : IComponent
            {
                public int Value;
                public StructCold(int v)
                {
                    Value = new List<int>().Count + v;
                }
                public void Tick(ref StructCold self)
                {
                    var text = $"v={self.Value}";
                    _ = text;
                }
                public override string ToString() => $"v={Value}";
            }
            """;
        failures += Check(
            Count(structCold, "ENG0001") == 1 && Count(structCold, "ENG0003") == 0,
            $"struct-level attr: Tick hot x1, ctor+ToString cold (ENG0001={Count(structCold, "ENG0001")}, ENG0003={Count(structCold, "ENG0003")})");

        // --- CodeFix honesty: explicit List<T> type / managed T => fires but no property ---
        // `List<int> xs = new List<int>()` still heap-allocates (ENG0001
        // fires) but is NOT mechanically fixable (declared type stays List).
        // `new List<string>()` fires too, but string is not unmanaged so no
        // TempList rewrite is offered. Two `new` keywords = two sites.
        const string explicitList = """
            using System.Collections.Generic;
            using Engine.Runtime;
            [UpdateInPhase(Phase.Update)]
            public partial struct ExplicitList : IComponent
            {
                public int Value;
                public void Tick(ref ExplicitList self)
                {
                    List<int> xs = new List<int>();
                    List<string> ys = new List<string>();
                    _ = (xs, ys);
                }
            }
            """;
        var explicitDiags = GetDiagnostics(explicitList)
            .Where(static d => d.Id == "ENG0001")
            .ToImmutableArray();
        failures += Check(explicitDiags.Length == 2, $"explicit/managed List still ENG0001 x2 (got {explicitDiags.Length})");
        failures += Check(
            explicitDiags.All(static d => !d.Properties.ContainsKey("FixableNewList")),
            "explicit/managed List carries no FixableNewList (no dishonest fix)");

        // --- Code fix: metadata + mechanical rewrite ---
        failures += await CheckCodeFixAsync();

        return failures;
    }

    private static async Task<int> CheckCodeFixAsync()
    {
        var failures = 0;
        var fixAsm = Assembly.LoadFrom(FindBin("Engine.CodeFixes", "net8.0"));
        var providerType = fixAsm.GetType("Engine.CodeFixes.ListToTempListCodeFixProvider");
        failures += Check(providerType is not null, "codefix provider type resolves");
        if (providerType is null)
        {
            return failures;
        }

        var provider = (CodeFixProvider)Activator.CreateInstance(providerType)!;
        failures += Check(provider.FixableDiagnosticIds.Contains("ENG0001"), "codefix targets ENG0001");

        const string fixSource = """
            using System.Collections.Generic;
            using Engine.Runtime;
            [UpdateInPhase(Phase.Update)]
            public partial struct FixMe : IComponent
            {
                public int Value;
                public void Tick(ref FixMe self)
                {
                    var xs = new List<int>();
                    _ = xs;
                }
            }
            """;
        var diag = GetDiagnostics(fixSource)
            .FirstOrDefault(static d => d.Id == "ENG0001" && d.Properties.ContainsKey("FixableNewList"));
        failures += Check(diag is not null, "new List<T> diagnostic carries FixableNewList");
        if (diag is null)
        {
            return failures;
        }

        using var ws = new AdhocWorkspace();
        var project = ws.AddProject("Fix", LanguageNames.CSharp)
            .WithCompilationOptions(new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary))
            .AddMetadataReferences(References());
        var document = project.AddDocument("Fix.cs", SourceText.From(fixSource));
        CodeAction? found = null;
        var ctx = new CodeFixContext(document, diag, (a, _) => { found ??= a; }, CancellationToken.None);
        await provider.RegisterCodeFixesAsync(ctx);
        failures += Check(found is not null, "codefix offers TempList rewrite");
        if (found is null)
        {
            return failures;
        }

        var apply = (await found.GetOperationsAsync(
                ws.CurrentSolution, progress: null!, CancellationToken.None))
            .OfType<ApplyChangesOperation>()
            .SingleOrDefault();
        failures += Check(apply is not null, "codefix produces an ApplyChanges operation");
        if (apply is null)
        {
            return failures;
        }

        apply.Apply(ws, CancellationToken.None);
        var text = (await ws.CurrentSolution.GetDocument(document.Id)!.GetTextAsync()).ToString();
        failures += Check(text.Contains("TempList<int>", StringComparison.Ordinal), "codefix rewrites to TempList<int>");
        failures += Check(text.Contains("stackalloc int[4]", StringComparison.Ordinal), "codefix emits stackalloc int[4] buffer");
        failures += Check(text.Contains("xsBuffer", StringComparison.Ordinal), "codefix threads the buffer into the TempList ctor");
        failures += Check(text.Contains("using Engine.Runtime;", StringComparison.Ordinal), "codefix keeps Engine.Runtime using");

        // Behavioral gate: the fixed output must compile AND hold items.
        // A bare `new TempList<T>()` compiles to a zeroed struct (capacity 0)
        // where Add silently drops — this gate proves the emission does not.
        failures += CheckFixedOutputRunsAsync(text);
        return failures;
    }

    private static int CheckFixedOutputRunsAsync(string fixedText)
    {
        // Compile the actual fixed document's emitted statements with a
        // driver that exercises them in place: the fixed statements already
        // declare xsBuffer/xs, so append usage + return.
        const string driver = """
            using System;
            using Engine.Runtime;
            public static class CodeFixProbe
            {
                public static string Run()
                {
                    __FIXED__
                    xs.Add(10);
                    xs.Add(20);
                    return $"{xs.Count}/{xs.Capacity}/{xs[0]}/{xs[1]}";
                }
            }
            """;
        // Extract the two emitted statements from the fixed document: the
        // buffer declaration and the TempList construction.
        var lines = fixedText.Split('\n')
            .Select(static l => l.Trim())
            .Where(static l => l.Contains("stackalloc", StringComparison.Ordinal)
                || (l.Contains("TempList<int>", StringComparison.Ordinal) && l.StartsWith("var ", StringComparison.Ordinal)))
            .ToArray();
        if (lines.Length != 2)
        {
            Console.WriteLine($"[FAIL] codefix output holds data (expected 2 emitted statements, got {lines.Length})");
            return 1;
        }

        var program = driver.Replace("__FIXED__", string.Join("\n        ", lines), StringComparison.Ordinal);
        var tree = CSharpSyntaxTree.ParseText(program, new CSharpParseOptions(LanguageVersion.Preview));
        var compilation = CSharpCompilation.Create(
            "CodeFixProbe",
            [tree],
            References(),
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary).WithAllowUnsafe(true));
        using var ms = new MemoryStream();
        var emit = compilation.Emit(ms);
        if (!emit.Success)
        {
            Console.WriteLine("[FAIL] codefix output holds data (fixed output does not compile):");
            foreach (var d in emit.Diagnostics)
            {
                Console.WriteLine($"  {d}");
            }

            return 1;
        }

        ms.Seek(0, SeekOrigin.Begin);
        var asm = Assembly.Load(ms.ToArray());
        var result = (string)asm.GetType("CodeFixProbe")!.GetMethod("Run")!.Invoke(null, null)!;
        if (result != "2/4/10/20")
        {
            Console.WriteLine($"[FAIL] codefix output holds data (got \"{result}\", want \"2/4/10/20\")");
            return 1;
        }

        Console.WriteLine("[PASS] codefix output holds data (compiles, 2/4/10/20)");
        return 0;
    }

    private static int Count(string source, string id)
    {
        var n = 0;
        foreach (var d in GetDiagnostics(source))
        {
            if (d.Id == id)
            {
                n++;
            }
        }

        return n;
    }

    private static ImmutableArray<Diagnostic> GetDiagnostics(string source)
    {
        var tree = CSharpSyntaxTree.ParseText(source, new CSharpParseOptions(LanguageVersion.Preview));
        var compilation = CSharpCompilation.Create(
            "Probe",
            [tree],
            References(),
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary).WithAllowUnsafe(true));
        var analyzers = AnalyzerAssembly.Value.GetTypes()
            .Where(static t => typeof(DiagnosticAnalyzer).IsAssignableFrom(t) && !t.IsAbstract)
            .Select(static t => (DiagnosticAnalyzer)Activator.CreateInstance(t)!)
            .ToImmutableArray();
        return compilation.WithAnalyzers(analyzers)
            .GetAnalyzerDiagnosticsAsync()
            .GetAwaiter()
            .GetResult()
            .Where(static d => d.Id.StartsWith("ENG", StringComparison.Ordinal))
            .ToImmutableArray();
    }

    private static readonly Lazy<Assembly> AnalyzerAssembly = new(
        () => Assembly.LoadFrom(FindBin("Engine.Analyzers", "netstandard2.0")));

    private static IEnumerable<MetadataReference> References()
    {
        var tpa = (string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")!;
        foreach (var path in tpa.Split(Path.PathSeparator))
        {
            yield return MetadataReference.CreateFromFile(path);
        }

        yield return MetadataReference.CreateFromFile(typeof(IComponent).Assembly.Location);
    }

    private static string FindBin(string project, string tfm)
    {
        var config = typeof(AnalyzerTests).Assembly
            .GetCustomAttribute<AssemblyConfigurationAttribute>()?.Configuration ?? "Debug";
        // Tests run from managed/<project>/bin/<config>/<tfm>/ — walk up to managed/.
        // Live-probe carve-out (M3-03 engine_compositor_live): the staged dir
        // mirrors managed/<project>/bin/<config>/<tfm>/ one level down
        // (<staged>/managed/...), so probe that layout first when the
        // standard walk misses.
        var managed = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", ".."));
        var direct = Path.Combine(managed, project, "bin", config, tfm, project + ".dll");
        if (File.Exists(direct))
        {
            return direct;
        }

        var staged = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "managed", project, "bin", config, tfm, project + ".dll"));
        if (File.Exists(staged))
        {
            return staged;
        }

        return direct;
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }
}

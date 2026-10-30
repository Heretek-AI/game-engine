// GeneratorDriverTests.cs — ABI 4.2 gate: runs the REAL ComponentSystemGenerator
// (loaded by path, like AnalyzerTests) over in-memory IComponent fixtures,
// requires the output to compile with zero errors, then loads the emitted
// assembly and calls the generated [UnmanagedCallersOnly] RunChunk through a
// function pointer over hand-built EngineChunkColumns. No native library:
// runs on every CI OS in the managed job.
using System.Reflection;
using Engine.Bindings;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

namespace Engine.Tests;

public static unsafe class GeneratorDriverTests
{
    private const string Fixture = """
        using System;
        using Engine.Runtime;

        namespace Fx;

        public partial struct Health : IComponent
        {
            public float Value;
        }

        public partial struct Mover : IComponent
        {
            public float Speed;
            public float X;
            public float Y;

            public void Tick(ref Mover self, in Health health, float dt)
            {
                self.X += health.Value * dt;
                self.Y += self.Speed * dt;
            }
        }

        public partial struct Spinner : IComponent
        {
            public float Angle;

            public void Tick(ref Spinner self, in float dt) => self.Angle += dt;
        }

        public partial struct Bomb : IComponent
        {
            private static readonly InvalidOperationException Boom = new("boom");
            public int Ticks;

            public void Tick(ref Bomb self)
            {
                self.Ticks++;
                if (self.Ticks == 2)
                {
                    throw Boom;
                }
            }
        }

        // Unrunnable shapes: Register must return -2.
        public partial struct ByValue : IComponent
        {
            public int V;

            public void Tick(ref ByValue self, Health health) => self.V++;
        }

        public partial struct DoubleDt : IComponent
        {
            public int V;

            public void Tick(ref DoubleDt self, double dt) => self.V++;
        }

        public partial struct DupTerm : IComponent
        {
            public int V;

            public void Tick(ref DupTerm self, in Health a, in Health b) => self.V++;
        }
        """;

    public static int Run()
    {
        var failures = 0;
        var asm = CompileWithGenerator(out var errors, out var generated);
        failures += Check(errors.Count == 0, $"generated output compiles with 0 errors (got {errors.Count}: {string.Join(" | ", errors)})");
        failures += Check(generated.Contains("RunChunk(NativeApi.EngineChunkColumns* view, float dt)", StringComparison.Ordinal), "runner takes (EngineChunkColumns*, float dt)");
        failures += Check(generated.Contains("table->EngineSystemRegister(world, name, ids, access, 2u, fn, userData)", StringComparison.Ordinal), "Mover registers 2 terms");
        failures += Check(generated.Contains("col0[i].Tick(ref col0[i], in col1[i], dt)", StringComparison.Ordinal), "Mover row call forwards in Health + dt");
        failures += Check(generated.Contains("col0[i].Tick(ref col0[i], in dt)", StringComparison.Ordinal), "Spinner row call forwards in dt");
        if (asm is null)
        {
            Console.WriteLine($"GeneratorDriverTests: {failures} FAILED (no assembly)");
            return failures + 1;
        }

        failures += CheckMover(asm);
        failures += CheckBomb(asm);
        foreach (var name in new[] { "Fx.ByValue", "Fx.DoubleDt", "Fx.DupTerm" })
        {
            failures += Check(CallRegister(asm, name) == -2, $"{name}.Register -> -2 (unrunnable shape)");
        }

        failures += Check(CallRegister(asm, "Fx.Mover") == -1, "Fx.Mover.Register(null table) -> -1");
        Console.WriteLine(failures == 0 ? "GeneratorDriverTests: all checks passed" : $"GeneratorDriverTests: {failures} FAILED");
        return failures;
    }

    private static int CheckMover(Assembly asm)
    {
        // Mover = 3 floats (Speed, X, Y); Health = 1 float.
        const int rows = 4;
        var movers = stackalloc float[rows * 3];
        var health = stackalloc float[rows];
        for (var i = 0; i < rows; i++)
        {
            movers[i * 3] = i;          // Speed
            movers[(i * 3) + 1] = 0;    // X
            movers[(i * 3) + 2] = 0;    // Y
            health[i] = 10 * (i + 1);
        }

        var view = default(NativeApi.EngineChunkColumns);
        view.Columns[0] = (ulong)movers;
        view.Columns[1] = (ulong)health;
        view.Sizes[0] = 12;
        view.Sizes[1] = 4;
        view.Count = rows;
        view.TermCount = 2;
        RunChunk(asm, "Fx.Mover")(&view, 0.5f);

        var ok = true;
        for (var i = 0; i < rows; i++)
        {
            ok &= movers[(i * 3) + 1] == 10 * (i + 1) * 0.5f && movers[(i * 3) + 2] == i * 0.5f;
        }

        var failures = Check(ok, "Mover RunChunk: X += Health*dt, Y += Speed*dt for 4 rows");

        // Wrong term count: the runner must not touch memory.
        view.TermCount = 1;
        RunChunk(asm, "Fx.Mover")(&view, 0.5f);
        failures += Check(movers[1] == 5.0f, "Mover RunChunk ignores a view with the wrong term count");
        return failures;
    }

    private static int CheckBomb(Assembly asm)
    {
        var ticks = stackalloc int[3];
        var view = default(NativeApi.EngineChunkColumns);
        view.Columns[0] = (ulong)ticks;
        view.Sizes[0] = 4;
        view.Count = 3;
        view.TermCount = 1;
        var fn = RunChunk(asm, "Fx.Bomb");
        fn(&view, 0f);
        fn(&view, 0f);  // row 0 reaches 2 and throws; the runner catches it
        fn(&view, 0f);  // faulted: skipped entirely
        var faulted = (bool)asm.GetType("Fx.Bomb", throwOnError: true)!.GetProperty("Faulted")!.GetValue(null)!;
        var failures = Check(faulted, "Bomb.Faulted after Tick threw");
        failures += Check(ticks[0] == 2 && ticks[1] == 1 && ticks[2] == 1, $"Bomb rows after fault == 2,1,1 (got {ticks[0]},{ticks[1]},{ticks[2]})");
        return failures;
    }

    private static delegate* unmanaged[Cdecl]<NativeApi.EngineChunkColumns*, float, void> RunChunk(Assembly asm, string type)
    {
        var mi = asm.GetType(type, throwOnError: true)!.GetMethod("RunChunk", BindingFlags.Public | BindingFlags.Static)!;
        return (delegate* unmanaged[Cdecl]<NativeApi.EngineChunkColumns*, float, void>)mi.MethodHandle.GetFunctionPointer();
    }

    private static int CallRegister(Assembly asm, string type)
    {
        var mi = asm.GetType(type, throwOnError: true)!.GetMethod("Register", BindingFlags.Public | BindingFlags.Static)!;
        var fn = (delegate*<NativeApi.ApiTable*, NativeApi.EngineWorld*, void*, int>)mi.MethodHandle.GetFunctionPointer();
        return fn(null, null, null);
    }

    private static Assembly? CompileWithGenerator(out List<string> errors, out string generated)
    {
        // The driver MUST get the same parse options as the input tree, or
        // AddSyntaxTrees throws "Inconsistent language versions".
        var parseOptions = new CSharpParseOptions(LanguageVersion.Preview);
        var tree = CSharpSyntaxTree.ParseText(Fixture, parseOptions);
        var compilation = CSharpCompilation.Create(
            "GeneratorDriverFixture",
            [tree],
            References(),
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary).WithAllowUnsafe(true));
        var genType = GeneratorAssembly.Value.GetType("Engine.Analyzers.ComponentSystemGenerator", throwOnError: true)!;
        var generator = ((IIncrementalGenerator)Activator.CreateInstance(genType)!).AsSourceGenerator();
        var driver = CSharpGeneratorDriver.Create([generator], parseOptions: parseOptions);
        driver.RunGeneratorsAndUpdateCompilation(compilation, out var output, out var genDiagnostics);
        generated = string.Join("\n", output.SyntaxTrees.Skip(1).Select(static t => t.ToString()));
        errors = genDiagnostics.Concat(output.GetDiagnostics())
            .Where(static d => d.Severity == DiagnosticSeverity.Error)
            .Select(static d => d.ToString())
            .ToList();
        if (errors.Count != 0)
        {
            return null;
        }

        using var ms = new MemoryStream();
        var emit = output.Emit(ms);
        if (!emit.Success)
        {
            errors.AddRange(emit.Diagnostics.Where(static d => d.Severity == DiagnosticSeverity.Error).Select(static d => d.ToString()));
            return null;
        }

        return Assembly.Load(ms.ToArray());
    }

    private static readonly Lazy<Assembly> GeneratorAssembly = new(
        () => Assembly.LoadFrom(FindAnalyzerDll()));

    private static IEnumerable<MetadataReference> References()
    {
        var tpa = (string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")!;
        foreach (var path in tpa.Split(Path.PathSeparator))
        {
            yield return MetadataReference.CreateFromFile(path);
        }
    }

    private static string FindAnalyzerDll()
    {
        // Same layout rules as AnalyzerTests.FindBin (direct, then staged).
        var config = typeof(GeneratorDriverTests).Assembly
            .GetCustomAttribute<AssemblyConfigurationAttribute>()?.Configuration ?? "Debug";
        var managed = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", ".."));
        var direct = Path.Combine(managed, "Engine.Analyzers", "bin", config, "netstandard2.0", "Engine.Analyzers.dll");
        if (File.Exists(direct))
        {
            return direct;
        }

        var staged = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "managed", "Engine.Analyzers", "bin", config, "netstandard2.0", "Engine.Analyzers.dll"));
        return File.Exists(staged) ? staged : direct;
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }
}

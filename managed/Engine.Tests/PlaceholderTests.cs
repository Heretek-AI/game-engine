// M1-01 pipeline placeholder: asserts the managed stubs load and report
// the expected stub versions. Real gates (layout assertions, Benchmark1M)
// land in M1-04/M1-05.
using Engine.Bindings;
using Engine.Runtime;

namespace Engine.Tests;

public static class PlaceholderTests
{
    public static int Run()
    {
        var failures = 0;
        failures += Check(NativeApiPlaceholder.AbiContractVersion == "0.1.0-m1-01-stub", "bindings stub version");
        failures += Check(RuntimePlaceholder.Version == "0.1.0-m1-01-stub", "runtime stub version");
        Console.WriteLine(failures == 0 ? "Engine.Tests: all checks passed" : $"Engine.Tests: {failures} FAILED");
        return failures;
    }

    private static int Check(bool ok, string name)
    {
        Console.WriteLine($"[{(ok ? "PASS" : "FAIL")}] {name}");
        return ok ? 0 : 1;
    }
}

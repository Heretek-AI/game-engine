// M1-04 gate: layout assertions over the generated ABI bindings +
// M2-01 gate: ergonomic scripting surface (IComponent/Entity/phases,
// FixedString64/TempList/AssetRef, SIMD math). Native live-contract checks
// (engine_get_api_table round-trip) run in CTest. Benchmark1M is M1-05.
var failures = 0;
failures += Engine.Tests.AbiLayoutTests.Run();
failures += Engine.Tests.RuntimeApiTests.Run();
return failures;

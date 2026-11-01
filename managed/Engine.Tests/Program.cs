// M1-04 gate: layout assertions over the generated ABI bindings +
// M2-01 gate: ergonomic scripting surface (IComponent/Entity/phases,
// FixedString64/TempList/AssetRef, SIMD math). M2-02 gate: generator emission
// contract (chunk runner shapes). Native live-contract checks
// (engine_get_api_table round-trip) run in CTest. Benchmark1M is M1-05.
var failures = 0;
failures += Engine.Tests.AbiLayoutTests.Run();
failures += Engine.Tests.InputApiTests.Run();
failures += Engine.Tests.RuntimeApiTests.Run();
failures += Engine.Tests.CompositorApiTests.Run(args);
failures += Engine.Tests.GiApiTests.Run(args);
failures += Engine.Tests.PhysicsApiTests.Run(args);
failures += Engine.Tests.NavigationApiTests.Run(args);
failures += Engine.Tests.Physics2DApiTests.Run(args);
failures += Engine.Tests.TilemapApiTests.Run();

failures += Engine.Tests.TerrainApiTests.Run(args);
failures += Engine.Tests.TerrainBrushTests.Run(args);
failures += Engine.Tests.GeneratorTests.Run();
failures += Engine.Tests.GeneratorDriverTests.Run();
failures += Engine.Tests.EditorApiTests.Run(args);
failures += Engine.Tests.CliApiTests.Run(args);
failures += Engine.Tests.ExportApiTests.Run(args);
failures += await Engine.Tests.AnalyzerTests.Run();
failures += Engine.Tests.HotReloadTests.Run();
failures += Engine.Tests.ScriptWorkflowTests.Run();
return failures;

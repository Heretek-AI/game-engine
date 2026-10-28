// M1-04 gate: layout assertions over the generated ABI bindings.
// Native live-contract checks (engine_get_api_table round-trip) run in
// CTest (native/tests/test_abi_layout.cpp). Benchmark1M lands in M1-05.
return Engine.Tests.AbiLayoutTests.Run();

# Illusion Layer implementation notes

## Baseline

- Date: 2026-10-30 (ILLU-M0).
- `origin/main` commit: `3112291aa06f476b068b37502cfcee7d198423c0` (Merge PR #51).
- Main-drift note (IMPLEMENTATION_PROMPT §3): spike was done on `70846a5`; current `origin/main` is `3112291`, recorded here as required. No `Find`-text edits were needed in M0 (no code changes).
- Branch: `feat/illusion-layer-abi-4.2`, created from `origin/main` in this task.
- Tool versions: `dotnet 10.0.403`, `cmake 3.31.6`, `ninja 1.12.1`, `g++ (Debian 14.2.0-19) 14.2.0`.
- CTest: `100% tests passed, 0 tests failed out of 40` (see `build/logs/m0-ctest.log`). Failing tests: none.
- Managed tests: `grep -c 'all checks passed' build/logs/m0-tests.log` = 9 (>= 9), `grep -c FAIL` = 0 (see `build/logs/m0-tests.log`).
- `dotnet build Engine.slnx -c Release` exit 0 (see `build/logs/m0-dotnet.log`); `cmake --preset ci-linux` and `cmake --build` exit 0.

### Discoveries

- 2026-10-30: all 40 CTests pass on main, including the 8 export tests listed in §4.7 (`engine_export_publish_aot`, `engine_export_stage*`, `engine_export_coldboot`). The doc expects them to fail on machines whose .NET SDK lacks the NativeAOT ILCompiler package; this machine's SDK (10.0.403) evidently has it, so the M0 baseline is fully green. Later milestones compare against this all-pass baseline.
- 2026-10-30: a stray `.hermes/` scratch dir (Hermes runner cache) was present at repo root and showed in `git status`; removed it (`rm -rf .hermes`) since it is runner scratch, not a repo artifact. `build/` is git-ignored, so `build/logs/m0-*.log` are not committed.

## M1 spikes

- Date: 2026-10-30 (ILLU-M1). Spike sources created verbatim from `docs/IMPLEMENTATION_PROMPT.md:309-418` under `build/` (git-ignored, not committed): `build/spikes/flecs_spike.cpp`, `build/spikes/cs1503/cs1503.csproj`, `build/spikes/cs1503/Program.cs`.
- Flecs spike build/run (`c++ -std=c++20 -O2 -I../../deps/flecs/include -I../../deps/flecs/distr flecs_spike.cpp -L../ci-linux/deps/flecs -lflecs ...`):

```text
mover=456 mover2=456 health=457
lookup(sep="")=456  lookup(default)=0  has_parent_Spike=0
type_info size=12 align=4
query=0x6179464df9e0 field_count=2
 table count=3 mover_col=0x617946550b90 mod64=16 health_col=0x617946550ad0 entities=0x617946550b60
tables=1 rows=3
 e0 speed=0 x=100
 e1 speed=1 x=0
 e2 speed=2 x=100
 e3 speed=3 x=0
 e4 speed=4 x=100
fatal: entity.c: 1522: abort(): Spike\.Mover (INVALID_COMPONENT_SIZE)
exit=134
```

- CS1503 repro build (`dotnet build build/spikes/cs1503`):

```text
Chunk_Spike_Mover.g.cs(37,65): error CS1503: Argument 3: cannot convert from 'delegate* unmanaged[Cdecl]<Engine.Bindings.NativeApi.EcsChunkView*, void>' to 'delegate* unmanaged[Cdecl]<Engine.Bindings.NativeApi.EcsChunkView*, float, void>' [cs1503.csproj]
    0 Warning(s)
    1 Error(s)
```

### Discoveries

- 2026-10-30: spike confirms §4.1 facts on this machine — same id on same-size re-register (`mover == mover2 == 456`), abort (exit 134, SIGABRT) on different-size re-register, `lookup(default)=0` so callers must use `ecs_lookup_path_w_sep` with empty seps, `size=12 align=4`, only 3 of 5 rows matched (Health-gated). Column `mod64=16` here vs `mod64=32` in the doc's capture — addresses vary per run, but the point stands: user columns are NOT 64-byte aligned.
- 2026-10-30: CS1503 repro error carries a `to '...EcsChunkView*, float, void>'` target suffix (doc acceptance cites only the `from '...EcsChunkView*, void>'` prefix), i.e. the generated chunk thunk drops the `float dt` parameter the `Tick` signature requires. M2's ABI work must reconcile the native `EcsChunkView*`-only callback shape with the managed `Tick(..., float dt)` signature.

## M2 ABI 4.2 header

- 2026-10-30: applied the 3 doc-ordered edits to `native/bridge/include/engine/bridge/engine_abi.h` (IMPLEMENTATION_PROMPT.md:420-610): edit 1 bumped `ENGINE_ABI_VERSION_MINOR` 1 -> 2 with the 4.2 comment tail; edit 2 added the generic tail (`ENGINE_ABI_MAX_TERMS 8`, ACCESS IN/INOUT, ERR codes -1..-6, `EngineChunkColumns`, `EngineSystemCallback`, 6 fn typedefs); edit 3 appended the 6 table fields ending with `systems_run` as last field before `} EngineApiTable;`. Each Find text matched exactly once.
- 2026-10-30: `grep -c 'ENGINE_ABI_VERSION_MINOR 2'` -> `1`; `grep -nE 'EngineSystemsRunFn systems_run;'` -> one line, last field before `} EngineApiTable;`; `grep -c 'typedef struct EngineChunkColumns'` -> `1`.
- 2026-10-30: `cmake --preset ci-linux && cmake --build build/ci-linux -j4` fails as the source doc predicts (M2 acceptance block: "expected to fail until M3"): `engine_abi.cpp:1013: error: missing field 'component_register' initializer [-Werror,-Wmissing-field-initializers]`. Header is 4.2, `kApiTable` has no new initializers yet — M3 fills them in. No drift: all 3 Find texts matched exactly once.

## M3A GenericSystems registration

- 2026-10-30: created `native/bridge/src/generic_systems.hpp` verbatim from `docs/IMPLEMENTATION_PROMPT.md` `<!-- file: native/bridge/src/generic_systems.hpp -->` block (programmatic fence diff: match=True, 2022 bytes).
- 2026-10-30: created `native/bridge/src/generic_systems.cpp` part 1 (through `get()`): `register_component` with `ecs_get_type_info` pre-check returning 0 on size/alignment mismatch (never calls `ecs_component_init` on mismatch — §4.1 abort rule), plus `create_entity`/`set`/`get` with NULL-arg/in-run guards. `register_system`/`run` intentionally deferred to ILLU-M3B (child card `t_e07a5570` owns them plus all `engine_abi.cpp` wiring); fence diff vs doc block is exactly the split-note header + missing register/run tail.
- 2026-10-30: TDD temporary spike `build/spikes/m3a_reregister_spike.cpp` (git-ignored, DELETED after run): `a=470 b=470 same=1`, `mismatch=0 (expect 0)`, set/get round-trip `speed=1.5 x=2.5`, `SPIKE PASS`, exit 0. Spike compiled `generic_systems.cpp` with `-Wall -Wextra -Werror` clean (flecs.c compiled as C; `engine_core_export.hpp` stubbed in git-ignored spike dir only — header includes it under `#ifdef __cplusplus` only so no repo change needed).
- 2026-10-30: split note — full `cmake --build build/ci-linux` still fails at `kApiTable` missing 6 initializers (M2-predicted state); M3B adds the wrappers + table entries that fix it. No `engine_abi.cpp` change in this commit by design (avoids colliding with M3B's 6 doc-ordered edits).

## M3B systems run + shutdown (ILLU-M3B)
- `native/bridge/src/generic_systems.cpp` completed verbatim from the M3 doc fence (register_system + serial run() appended; file now byte-matches the doc fence minus the M3A-split NOTE header comment). `grep -c ENGINE_ABI_ERR_IN_RUN` = 3 (set, register_system, run in-run refusal paths).
- `native/bridge/src/engine_abi.cpp`: applied the 6 doc-ordered edits (each Find matched exactly 1x): generic_systems.hpp include, EngineChunkColumns static asserts (sizeof 120, offsets 64/96/104/108/112), GenericSystems member on EngineWorldImpl, generic.shutdown() before ecs fini in ApiWorldDestroy, 6 Api* wrappers (component_register/entity_create/entity_set+plan-invalidate/entity_get/system_register/systems_run with NULL-dispatch dt=0), 6 kApiTable tail entries.
- `native/tests/test_abi_layout.cpp`: 312→360B table-size edit applied.
- Build: `cmake --build build/ci-linux -j4` exit 0. `ctest -R engine_abi_layout -V` passes, shows `abi: EngineApiTable 360B (ABI 4.2 generic tail)`.
- `engine_compositor_live` fails post-M3B with `ABI version mismatch: native 4.2.0` — expected: managed `NativeApi.g.cs` still pins AbiVersionMinor=1; regeneration is owned by ILLU-M5 (abi_codegen). Not a native regression: native-side `engine_abi_layout` is green.
- Card said "7 engine_abi.cpp edits" but the doc contains exactly 6 engine_abi.cpp edit blocks + 1 test_abi_layout.cpp edit (step 3); all 7 applied.

## M4 native test of the generic tail (ILLU-M4)
- `native/tests/test_generic_systems.cpp` created verbatim from the M4 doc fence (programmatic fence diff: BYTE-MATCH, 149 lines).
- `native/tests/CMakeLists.txt`: applied M4 edit #1 verbatim (engine_generic_systems add_executable/link/flags/add_test, TIMEOUT 120). Edit #2 deferred to M9 per doc.
- 312→360B table-size edit was already applied in M3B (test_abi_layout.cpp:218) — verified present, not re-applied.
- Build: `cmake --build build/ci-linux --target engine_generic_systems engine_abi_layout` exit 0. `ctest -R engine_abi_layout|engine_generic_systems`: 100% passed (2/2). Verbose tail shows `generic_systems: all checks passed` plus the doc acceptance lines `[PASS] systems_run with NULL dispatch` and `[PASS] row 4: speed=4.0 x=25.0 (want speed=4 x=25.0)`.
- No fix-forward needed: first-run green, `generic_systems.cpp` untouched.

## M5A codegen parser rejects unknown 4.2 types (ILLU-M5A)
- Applied doc edits #6 (`ValidateCType`) + #7 (`CsType`) ONLY to `tools/abi_codegen/Program.cs`: added `const void*`, `const uint64_t*`, `const uint32_t*`, `const EngineChunkColumns*`, `EngineSystemCallback` (plain Cdecl callback shape, no SuppressGCTransition).
- Pre-change baseline: `dotnet run --project tools/abi_codegen -c Release -- .` exited 1 with `unsupported ABI type 'const EngineChunkColumns*'` at engine_abi.h:492 (fail-closed confirmed).
- Post-change: still exits 1, now at `Fn typedef(s) not used by EngineApiTable: EngineSystemCallback` — the required-entry + unused-typedef checks (edits #5/#12) and emission belong to ILLU-M5B. No `.g.cs` touched.

## M5B 4.2 bindings emitted (ILLU-M5B)
- Applied doc edits #1-5 + #8-13 to `tools/abi_codegen/Program.cs` (M5A already owned #6+#7): GenericDefineRe + ParseGenericDefines (9 required consts, MAX_TERMS==8 gate), EngineChunkColumns required struct, 6 required fn entries, Emit signature carries `generic` dict, MaxTerms..ErrInternal consts emission, EmitChunkColumns with fail-closed 120-byte field check, EngineSystemCallback unused-typedef removal, loader drift guard extended with EngineChunkColumns 120B.
- `dotnet run --project tools/abi_codegen -c Release -- .` prints `abi_codegen: 44 entries`, exit 0. Second run: `git diff` on NativeApi.g.cs unchanged (idempotent).
- TDD red: pre-test-edit `dotnet run --project managed/Engine.Tests` showed `AbiLayoutTests: 2 FAILED` (minor==1, ApiTable 312B) against the new 360B table. Applied 4 AbiLayoutTests edits (minor 1->2, ApiTable 312->360B + component_register @312 + systems_run @352 + ChunkColumns 120B, columns offsets 96/104/108/112, system_register Cdecl assertion). Post-edit: `AbiLayoutTests: all checks passed`, `grep -c FAIL` = 0.
- `grep -c SuppressGCTransition NativeApi.g.cs` = 3 (unchanged); systems_run entry is plain Cdecl; `Size = 120` appears once.

## M6 Ecs helpers (ILLU-M6)
- `managed/Engine.Runtime/Ecs.cs` created verbatim from `docs/IMPLEMENTATION_PROMPT.md:1919-2067` doc fence (programmatic fence diff: VERBATIM, 5043 bytes).
- `dotnet build managed/Engine.Runtime -c Release`: `Build succeeded. 0 Warning(s) 0 Error(s)`.
- TDD spike `build/spikes/ecs6/` (git-ignored, DELETED after run): called `Ecs.ComponentId<Mover>` twice on a live `EngineWorldCreate` world — `GREEN` (same non-zero id). Note: `ApiLoader.Load()` resolves `engine_core` by SONAME only, so the spike preloaded `/home/hermes/game-engine/build/ci-linux/native/libengine_core.so` via `NativeLibrary.Load(absolute)` and bound `engine_get_api_table` by export instead of using `ApiLoader.Load()`.
- `dotnet build Engine.slnx -c Release --nologo`: `Build succeeded. 0 Warning(s) 0 Error(s)`.

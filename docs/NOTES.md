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

## M7A generator Register emission (ILLU-M7A)
- Replaced `managed/Engine.Analyzers/ComponentSystemGenerator.cs` per `docs/IMPLEMENTATION_PROMPT.md:2068-2821` up to and including `Register` emission: doc-verbatim header/pipeline/discovery/resolution/`EmitOne`/`EmitSizeRegistration`/`SystemName`, plus doc-verbatim `ComponentParams`/`RowArgs`/`IsRunnable`/`IsFloatScalar` helpers. `EmitChunkRunner` is an inert M7A stub with the stable ABI 4.2 signature (`RunChunk(EngineChunkColumns*, float dt)`); the span-unpacking/row-loop/try-catch body is owned by ILLU-M7B (child card `t_d8676106`).
- `Ecs.cs` note: ILLU-M6's commit (`1078062`) exists only on its dispatch branch, not on `feat/illusion-layer-abi-4.2` in this worktree, so the M7A spike build borrowed that file uncommitted for compilation only. It is NOT staged in the M7A commit — M6's merge will supply it. (Flagging for the orchestrator: M6 shows complete but its file is unreachable from the feat branch.)
- Acceptance: `/tmp/m7a_spike` (cs1503 sources retargeted at this worktree's `managed/` projects): `Build succeeded. 0 Warning(s) 0 Error(s)` — CS1503 gone. The main-repo `build/spikes/cs1503` still fails because it references main-repo `managed/` (old generator + missing 4.2 bindings there); that is a spike-path artifact, not a generator defect.
- Emitted `Chunk_Spike_Mover.g.cs`: `ComponentSize`, `Faulted`, and `Register` blocks byte-match the §4.6 reference modulo whitespace (incl. `table->EngineSystemRegister(world, name, ids, access, 2u, fn, userData)` and `Spike.Mover\0` name); `RunChunk` signature matches, body inert by design (`_ = view; _ = dt;`) — full body is M7B's problem.

## M7B RunChunk emission (ILLU-M7B)
- Replaced the M7A inert `EmitChunkRunner` stub with the doc body (`IMPLEMENTATION_PROMPT.md:2679-2732`): unrunnable guard keeps `_ = view; _ = dt;`, runnable path emits `count`/`TermCount`/`s_faulted` guard, per-term `Span`/`ReadOnlySpan` unpacking (`col0` self type via `selfFqn`), `try`/`for` row loop `col0[i].Tick(...)` with `RowArgs`, `catch (Exception)` latching `Volatile.Write(ref s_faulted, 1)`.
- `managed/Engine.Analyzers/ComponentSystemGenerator.cs` is byte-identical to the doc fence (`2076..2809`); `dotnet build Engine.slnx -c Release`: `Build succeeded. 0 Warning(s) 0 Error(s)` (CS0649 gone — latch now written).
- `dotnet build build/spikes/cs1503` (copied into this worktree so refs resolve): `Build succeeded. 0 Warning(s) 0 Error(s)`. Emitted `Chunk_Spike_Mover.g.cs` is byte-identical to the §4.6 reference (`:154-235`) modulo the Roslyn BOM; contains `table->EngineSystemRegister(world, name, ids, access, 2u, fn, userData)` and `col0[i].Tick(ref col0[i], in col1[i], dt);`.

## M8 driver test (ILLU-M8)
- Created `managed/Engine.Tests/GeneratorDriverTests.cs` verbatim from `docs/IMPLEMENTATION_PROMPT.md:2830-3081` doc fence (10077 bytes); registered in `managed/Engine.Tests/Program.cs` per the `:3086-3108` edit block (`GeneratorDriverTests.Run()` after `GeneratorTests.Run()`).
- Driver passes the same `CSharpParseOptions(LanguageVersion.Preview)` to both the input tree and `CSharpGeneratorDriver.Create` (Facts :133 pitfall — avoids `Inconsistent language versions`).
- `dotnet build Engine.slnx -c Release`: `Build succeeded. 0 Warning(s) 0 Error(s)`. `dotnet run --project managed/Engine.Tests -c Release --no-build`: `GeneratorDriverTests: all checks passed`, no FAIL.
- Sabotage check (`? "in dt" : "dt"` → `? "in dt" : "0f"`, rebuild, rerun): exact doc-expected 4 lines — `[FAIL] Mover row call forwards in Health + dt`, `[FAIL] Mover RunChunk: X += Health*dt, Y += Speed*dt for 4 rows`, `[FAIL] Mover RunChunk ignores a view with the wrong term count`, `GeneratorDriverTests: 3 FAILED`. Reverted + rebuilt green.
- Worktree note: this worktree's branch was stale at M5B (M6/M7A/M7B lived on sibling dispatch branches); fast-forwarded with `git merge --ff-only efd55cb` (merge-base confirmed HEAD was ancestor) per the reuse-`feat/illusion-layer-abi-4.2` convention before writing the test.

## M9 sample + CTest gate (ILLU-M9)
- Created `samples/IllusionSample/` verbatim from `docs/IMPLEMENTATION_PROMPT.md:3139-3443` fences (csproj 1442B, Components.cs 1887B, Program.cs 7836B); applied both doc edits (`Engine.slnx` IllusionSample line, `native/tests/CMakeLists.txt` engine_illusion_sample gate with `PASS_REGULAR_EXPRESSION "ILLUSION-SAMPLE-OK"`).
- Worktree note: this worktree's branch was stale at M5B; merged M6/M7A/M7B/M8 branch (`game-engine/t_4cce9280-illu-m8-generator-driver-test`, HEAD 61e99f6) into this worktree before writing files, per the reuse-`feat/illusion-layer-abi-4.2` convention.
- `dotnet build Engine.slnx -c Release`: `Build succeeded. 0 Warning(s) 0 Error(s)`. Direct run `dotnet samples/IllusionSample/bin/Release/net10.0/IllusionSample.dll <libengine_core.so>`: all 21 checks `[PASS]`, `0 managed bytes over 100 frames x 100k rows (got 0)`, `[info] 100k Mover rows: 0.176 ms/frame`, `ILLUSION-SAMPLE-OK`, exit 0.
- `ctest --preset ci-linux -R engine_illusion_sample --output-on-failure -V`: `ILLUSION-SAMPLE-OK`, `100% tests passed, 0 tests failed out of 2`. Full `ctest --preset ci-linux`: `100% tests passed, 0 tests failed out of 42`.

## M10/M11 repair verification (orchestrator, feat tip 7f6ac55)
- Repaired 2026-10-30: archived 3 malformed auto-decomposed cards (no workspace_path, spawn-fail loops); cherry-picked the M6-M9 line onto feat (6a265a3 d70a3c6 ac6a04e f09dd63 7f6ac55); origin/main restored to 3112291 after an accidental push of ILLU commits (all work preserved on feat branch, pushed).
- M10 AGENTS.md edits NOT applied — approval-protected path, needs operator (see M11b report below for exact text).
- M11b full matrix from clean tree (logs build/logs/m11b-*.log), feat tip 7f6ac55:
  - cmake cfg=0, build=0, 0 warning/error lines; ctest 42/42 passed.
  - dotnet format --verify-no-changes: format=0; dotnet build slnx: 0; Engine.Tests exit 0, 0 FAIL lines, 10 suites `all checks passed`.
  - codegen 44 entries, re-run diff clean (codegen-clean=0).
  - Matches M0 baseline (no new failures; SDK 10.0.403 ships NativeAOT ILCompiler so export tests pass).
- Full M11 evidence (decisions table, spec check) lives in the t_56fafa04 worktree note (de148b8); this entry confirms the feat tip reproduces it.

## PR (ILLU-PR)
- End-to-end PR: https://github.com/Heretek-AI/game-engine/pull/52 — `[Illusion] ABI 4.2 end-to-end (M0-M11)`, base main, head feat/illusion-layer-abi-4.2 (15 commits M0-M11 + M10 AGENTS.md fix).
- Body cites spec Verification + Decisions log, states all 10 decisions followed. Gates pending at open (CI running); do NOT merge — to qa-gate final review.
- Merged 2026-10-30 as eabfb74; ILLU-FOLLOW proceeds on rebased main.

## ILLU-FOLLOW portability + format pass (ILLU-FOLLOW, pre-merge prep; rebased onto main eabfb74)
- Precondition met 2026-10-30: PR #52 merged as eabfb74; prep commit rebased onto new main.
- Merge-independent work done on feat base (worktree t_28b5f4dd):
  - §4.8 item 1 audit: `generic_systems.cpp` passes clang strict `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wundef -Werror` (macOS-analog flags) clean; full `cmake --preset ci-linux` build with ENGINE_WERROR=ON green, `engine_generic_systems` + `engine_abi_layout` 2/2 PASS.
  - One hardening committed: `register_system` name copy now checks `snprintf` return and re-pins NUL on truncation (explicit for `-Wformat-truncation`/MSVC /W4 readers; behavior unchanged — snprintf already NUL-terminates). No warnings disabled, WERROR untouched.
  - §4.8 item 4: `dotnet format Engine.slnx --verify-no-changes` clean on pinned SDK 10.0.403 (global.json `10.0.100` + rollForward latestFeature; CI setup-dotnet `10.0.x`) — "Formatted 0 of 99 files".
  - CI-matrix (Windows/macOS), Sonar, CodeRabbit, follow-up PR: unblocked by merge; in progress on rebased main.

## ILLU-FOLLOW findings on rebased main (post-merge, same branch)
- CI matrix on merge commit eabfb74: ALL green at merge time — native + managed on ubuntu-24.04, macos-15, windows-2025 (run 41124983149 success). CodeRabbit's flagged MSVC C4244 (`qd.terms[i].inout` narrowing) was already fixed pre-merge by 6f4abe3 (`static_cast<int16_t>`); no live Windows/macOS failure remains. Same for the IsComponent-gate thread — fixed by 6f4abe3.
- Remaining CodeRabbit thread (Minor, still valid on rebased main): `CheckBomb` in GeneratorDriverTests reads `stackalloc int[3]` without init — fixed here with `new Span<int>(ticks, 3).Clear()` (commit acb7e02), behavior unchanged, tests green.
- Extra-strict clang audit on rebased main (compile_commands.json flags + `-Wimplicit-int-conversion -Wshorten-64-to-32 -Werror -fsyntax-only`): `generic_systems.cpp` and `engine_abi.cpp` both clean. No `strncpy`, no warning suppressions (`/wd`, `-Wno-`, pragmas) in the new code; WERROR (`/WX`/`-Werror` via ENGINE_WERROR=ON) untouched.
- No Sonar integration in this repo: no sonar properties/config, no Sonar workflow, no Sonar findings on PR #52. Nothing to resolve — recorded as verified-absent.
- Linux verification on rebased branch: `cmake --preset ci-linux` build clean, ctest 42/42 PASS; `dotnet format --verify-no-changes` clean; `dotnet build` 0 errors; Engine.Tests 10/10 suites pass (289 [PASS] lines, incl. GeneratorDriverTests with the fix).
- §4.8 items 2-3 (NativeAOT with generated systems, >16-byte alignment): NOT touched — out of scope as risks, no evidence they surfaced.

## ILLU-FOLLOW-4: Sonar S6494 fix on PR #53 (snprintf-free CopyName)
- SonarCloud on PR #53 reported exactly ONE finding: cpp:S6494 MINOR "Use std::format or std::format_to_n instead of snprintf" at `generic_systems.cpp:184` (the return-checked snprintf hardening) — Quality Gate B vs required A. Earlier "Sonar verified-absent" note was wrong for the follow-up branch: SonarCloud IS integrated at org level and analyzes PRs without repo-side config.
- Fix (branch `game-engine/t_28b5f4dd-illu-follow-windows-macos-sonar-pass`): replaced both snprintf copies with a bounded `CopyName` template (`strlen` + `min(len, N-1)` + `memcpy` + NUL; `(std::min)` parens for the Windows `min` macro). Applied in `register_system` (generic_systems.cpp) and `ApiRegisterSystem` (engine_abi.cpp — that site was missing from PR #53). Includes: `<algorithm>`/`<cstddef>` added, unused `<cstdio>` dropped from both TUs. Behavior unchanged (clamped NUL-terminated copy, same as snprintf "%s"). No suppressions, WERROR untouched.
- Verified Linux: `cmake --preset ci-linux` build clean, ctest 42/42 PASS, extra-strict clang (`-Wimplicit-int-conversion -Wshorten-64-to-32 -Werror -fsyntax-only` over compile_commands flags) CLEAN on both files, `dotnet format --verify-no-changes` clean.
- CodeRabbit on PR #53: "No actionable comments" (latest review), 0 inline threads. Pushed to PR #53 head; CI + Sonar + CodeRabbit re-run on push.
- §4.8 items 2-3: not surfaced, untouched.

## ILLU-FOLLOW-4b: S5813 + duplication fix (shared header)
- Push of the S6494 fix closed S6494 but Sonar opened 3 new items on the new code: cpp:S5813 MAJOR x2 ("make sure use of strlen is safe" — one per TU, since `strlen` on an unbounded caller pointer can over-read before the clamp) + 76.3% duplication on new code (the helper was pasted into both TUs; gate requires <=3%).
- Fix: single shared `native/bridge/src/name_copy.hpp` with `CopySystemName` — bounded scan via `memchr(src, 0, N-1)` so at most N-1 bytes are ever read (no pre-scan over unbounded input), then `memcpy` + NUL. Both call sites use it; no `strlen`/`snprintf` remain in the new code, one definition kills the duplication. `engine_abi.cpp` calls it qualified (`engine::bridge::`) since that TU is in global scope.
- Verified: WERROR build clean, ctest 42/42 PASS, extra-strict clang audit CLEAN on both TUs, standalone unit check of the helper (empty/nullptr/exact-fit/63/64/300-char/tiny-buffer) PASS. Pushed to PR #53 head; Sonar re-runs on push.

## ILLU-FOLLOW-4c: all gates green on PR #53
- Final state (head 2cd64d1): Sonar PASS (0 open issues; S6494 closed, S5813 x2 resolved by memchr-bounded scan, duplication cleared by shared header), CodeRabbit "no actionable comments" + 0 inline threads, CI native+managed PASS on ubuntu-24.04/macos-15/windows-2025, Analyze C++ PASS, CodeQL PASS, supply-chain PASS, merge state CLEAN/MERGEABLE.
- Local: WERROR build clean, ctest 42/42, extra-strict clang audit CLEAN, `dotnet format --verify-no-changes` clean, `git status` clean.
- PR #53 `[Illusion] Windows/macOS + Sonar follow-up` was opened by sibling task t_28b5f4dd (done/reviewed); this card's implementation-phase work landed on the same branch (commits dcff5be engine_abi parity, a3f3181 S6494 fix, fbeffab + this NOTES entry). No duplicate PR opened. §4.8 items 2-3 never surfaced.

## ILLU-FOLLOW-4d: CodeRabbit doc-contract fix (memchr safety claim)
- CodeRabbit's first real review of PR #53 (after OSS rate-limit reset; `@coderabbitai review` retried x4 until "Review triggered" then "Review finished") posted 1 Minor thread: `name_copy.hpp` header comment claimed unterminated input "can never drive an over-read", but `memchr(src,0,N-1)` itself reads up to N-1 bytes — a shorter unterminated buffer would over-read. Valid finding, doc-only fix: header + function comments now state the contract (src must be NUL-terminated within its readable range or expose >= N-1 readable bytes; shorter unterminated input needs an explicit length this helper doesn't take). No behavior change; both call sites pass valid C strings.
- qa-gate round 1 verdict: code correct, Sonar green, but CI matrix was still in_progress at reviewed HEAD 6adfcf3 and CodeRabbit had never run (rate-limit). Re-review requires CI green at final HEAD + CodeRabbit run with threads resolved.

## P0-04: `engine_export_publish_aot` is environment-only (2026-10-31 decision)

- Slice: P0-04 (issue #68). Question: is the local `NETSDK1047: Assets file ... doesn't have a target for 'net10.0/linux-x64'` failure a repo bug or an environment-only SDK defect?
- Verdict: environment-only. On a fresh clone with the Microsoft .NET 10 SDK, the full §3.2 sequence passes, including all 22 `engine_export*` tests. No `native/tests/CMakeLists.txt` change needed; NOT adding the `-r ${EXPORT_SAMPLE_RID}` restore step.
- Evidence (fresh clone `fresh/`, HEAD `e763de6`, Debian 13, SDK `10.0.403` at `/usr/share/dotnet/sdk/10.0.403`):
  - `cmake --preset ci-linux` → exit 0 (configure 4.3 s).
  - `cmake --build --preset ci-linux` → exit 0 (473/473 targets).
  - `SDL_VIDEO_DRIVER=offscreen ENGINE_REQUIRE_GPU=1 ctest --preset ci-linux --output-on-failure` → `100% tests passed, 0 tests failed out of 42`, Total Test time 11.19 s.
  - `ctest --preset ci-linux -R engine_export --output-on-failure` → `100% tests passed, 0 tests failed out of 22`, Total Test time 2.38 s; `engine_export_publish_aot` Passed in 1.66 s.
  - `dotnet build Engine.slnx -c Release --nologo` → exit 0; `dotnet run --project managed/Engine.Tests -c Release --no-build` → exit 0 (`HotReloadTests: all checks passed`).
- Mechanism: the failing setup ran Ubuntu's source-built `dotnet-sdk-10.0` 10.0.114 (per issue #68), which lacks the `Microsoft.NETCore.App.Runtime.NativeAOT.linux-x64` ILCompiler restore path — hence `project.assets.json` had no `net10.0/linux-x64` target after a plain (RID-less) build, and the `--no-build -r linux-x64` publish failed with NETSDK1047. With the Microsoft SDK, the publish step's implicit restore pulls `runtime.linux-x64.microsoft.dotnet.ilcompiler` 10.0.14 (present in `~/.nuget/packages`) and `ExportSample/obj/project.assets.json` contains both `net10.0` and `net10.0/linux-x64` targets (12 `linux-x64` references).
- Follow-up: close issue #68 as not-planned with this PR. If a contributor must use the Ubuntu source-built SDK, the fix is a setup test running `dotnet restore -r ${EXPORT_SAMPLE_RID}` before the `--no-build` publish in `native/tests/CMakeLists.txt` (`engine_export_publish_aot` block) — deliberately not added here since CI (Microsoft SDK via `actions/setup-dotnet`) and this machine both pass without it.

## P4-00 spike: visual fidelity facts (2026-10-31, branch parity/p4-00-spike-facts)

- Spec: added §4.12 to docs/PARITY_IMPLEMENTATION_PROMPT.md (entry points + inputs for PbrPipeline/GiFrameGraph/GpuCull/SceneSsbo, slangc v2026.20 CI facts, lavapipe timestamp support, P4-01..12 file list). Docs-only; no slice code, no golden (harness defined by P1-05, not yet in tree).
- PbrPipeline::create_gpu is a deliberate deferred stub (kInvalidArgument "deferred to compositor frame setup (M3-04)"); P4-06 wires vis->shade->post through the compositor, never calls it directly. PostPush is 20B (README "post 24B" line is a typo; header static_assert pins 20B).
- slangc: NOT in apt; the P4-00 spike manually fetched the shader-slang GitHub release asset (CI does not download it today). Latest at spike: v2026.20 (2026-10-17). Linux x86-64 glibc-2.28 tarball sha256 84ebd777968caceea26b21bfd014341078be6047be6c5a963a69f984f896cebd. `slangc -v` prints bare `2026.20`; `--version` is rejected (E00017). All 10 slang_build.py entry points compile to magic 07230203 / SPIR-V 1.5. slang_build.py --slangc refuses out-of-tree shader dirs (embed step requires in-repo paths).
- lavapipe timestamp queries WORK at the driver level: timestampPeriod=1.0ns, 64 valid bits, monotonic TOP/BOTTOM_OF_PIPE results (spike: raw-Vulkan g++ -lvulkan program, SDL_VIDEO_DRIVER=offscreen). Engine scene-pass profiler integration (P3-09) remains unverified.
- Evidence runs: engine_slang_pbr / engine_gi_zero_bake / engine_gpu_indirect all PASS on lavapipe (SDL_VIDEO_DRIVER=offscreen) from build/ci-linux binaries.
- Prior P4-00 attempt (d5bbfac/d21040d on feat/parity-p4-fidelity, never merged) was stale: header comment for create_gpu changed since, and its §4.12 claimed the golden harness "already exists" when golden_compare.hpp is not in tree on main. This PR re-verifies everything against current main.
- CodeRabbit round 1 (5 inline threads, all valid): destroy() runs BEFORE device shutdown (test destroys gi while dev alive); GpuCull create_gpu is sentinel marks, not VMA allocation — doc now names both counter buffers (CullPush::count_addr vs HizPush::draw_count_addr); second compute-to-DRAW_INDIRECT barrier in P4-06 frame order; slangc is a manual spike fetch, not CI; timestamp verdict scoped to driver-level probe.

## P4-01: glTF metallic-roughness materials (2026-10-31, branch parity/p4-01-pbr-materials)

- `Material` asset + component: `native/renderer/include/engine/renderer/pbr_material.hpp`
  (`PbrMaterialAsset`, `MaterialComponent` 64B blittable twin, `PbrMaterialLibrary`
  slot allocator, `to_scene_record()` packing the exact 64B `SceneMaterial` field order
  with bit-exact u32 lanes, `PbrValidateMaterial()` fail-closed on NaN/Inf/blend+mask).
- cgltf import: `material_import.hpp/.cpp` (`PbrImportMaterials`/`PbrImportMaterial`,
  sRGB->linear factor decode, emissive_strength via the `KHR_materials_emissive_strength`
  EXTENSION object — not a bare `emissiveStrength` key, which cgltf silently ignores).
- Shade/post stages move from slang to GLSL (`pbr_shade.vert/frag`,
  `post_composite.vert/frag`, glslangValidator `--target-env vulkan1.3`, blobs re-embedded
  into the same `*_spv.inc` names): slangc 2026.20 emits physical-addressing SPIR-V that
  lavapipe's spirv-val rejects (`VUID-08737 OpStore not a logical pointer`) — even the
  UNMODIFIED M3-03 blobs recompiled with 2026.20 fail, while glslang output passes clean.
  Visibility pass stays slang. P4-00's §4.12 slangc facts stand (version/sha/entry points);
  this adds the lavapipe-consumability caveat.
- Bindless-set-0 reality check (validation layers): set 0 carries ONLY scene/material
  SSBOs + texture/sampler arrays — no lights/grid/index. Light SSBOs + visibility image
  ride the test-owned set 1 (shade set-1: vis STORAGE_IMAGE[0], albedo COMBINED array[2],
  lights/grid/index SSBOs[3/4/5]). glslang maps texture-only `usampler2D` decls to
  SAMPLED_IMAGE (not COMBINED) — storage-image `r32ui readonly uimage2D` keeps the type exact.
- Root-cause gotcha (worth a §7 line): the SceneInstance ids row is UINT32 — declaring the
  record as `vec4[]` in GLSL reinterprets small ints as denormal floats, and `uint(denormal)`
  is 0, silently pinning every instance to material 0 (image rendered but flat/wrong).
  Fix: `uvec4 sceneRows[]` + `uintBitsToFloat` for float lanes. Debugged via push-flag
  visualization (metallic/vis.x/row5.y/row4.w probes) + upload verification by mapping
  the staging buffers after submit.
- Golden: `native/tests/golden/p4_01_spheres_320x240.png` (5x5 grid, metallic x 0..1,
  roughness y 0.04..1.0, one point light, shade->post offscreen + readback) via new
  `native/tests/golden_compare.hpp` + `golden_stb.cpp` (P1-05 contract: ch±2, 0.1% pixels,
  `ENGINE_UPDATE_GOLDENS=1` overwrites, actual/diff PNGs on failure). Vulkan NDC
  is already Y-down: the fullscreen-triangle vertex shaders pass pos through
  (no Y negation — a `-pos.y` flip samples the HDR target upside-down) so vUv 0
  is top-left, matching readback rows.
- Evidence: `ctest --preset ci-linux` 44/44 pass with
  `SDL_VIDEO_DRIVER=offscreen ENGINE_REQUIRE_GPU=1`; `engine_pbr_materials` +
  `engine_pbr_golden` all PASS incl. `[PASS] golden: p4_01_spheres_320x240` and the
  metallic-varies sanity check; managed `Engine.Tests` pass; `slang_build.py` untouched
  (slang sources it compiles — vis/gi/shadow — are unchanged).

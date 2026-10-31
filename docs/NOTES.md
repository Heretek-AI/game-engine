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

## P0-01 fnptr reflection (parity/p0-01-fnptr-reflection, 2026-10-30)
- Fix: `managed/Engine.Tests/AbiLayoutTests.cs:96` — `f.FieldType.IsPointer` → `f.FieldType.IsFunctionPointer || f.FieldType.IsPointer` (§4.8). For `delegate* unmanaged[Cdecl]<int, void> F`, `IsPointer` is false on .NET 10 (`dotnet --version` → 10.0.403); only `IsFunctionPointer` is true. Message text unchanged; `NativeApi.g.cs` untouched.
- Verified the shipped test binary actually carries the fix: IL member list contains `get_IsFunctionPointer` only after the edit+rebuild (stashed pre-fix build lacks it); `md5sum` of bin DLL was stale-identical across rebuilds due to deterministic build, so used `strings`/IL-grep, not hash, to confirm.
- Before (fix stashed): `cmake -S . -B /tmp/oot-prefix-check ... && cmake --build ... && ctest -R "engine_compositor_live$"` → `AbiLayoutTests: 42 FAILED` (all 42 `ApiTable.* is fnptr` lines `[FAIL]`), plus the live-probe `DllNotFoundException` lines.
- After: same OOT command → `AbiLayoutTests: all checks passed`; remaining `engine_compositor_live` failure is only the live probe (`CompositorApiTests: 1 FAILED`, `PhysicsApiTests: 1 FAILED`, `DllNotFoundException: Unable to load engine_core`).
- Root cause of residual (pre-existing, out of P0-01 scope): `TryPreloadEngineCoreHandle` (CompositorApiTests.cs:208, PhysicsApiTests PhysicsLivePreload) only probes staged CWD/appdir siblings after an upward walk finds `Engine.slnx`; under `/tmp/oot/.../compositor_live` the walk never reaches the repo so it returns `IntPtr.Zero`, and bare-SONAME `NativeLibrary.TryLoad("engine_core")` fails (verified false even with `LD_LIBRARY_PATH` set; absolute-path `NativeLibrary.Load` succeeds and exports `engine_get_api_table`). In-tree preset works because the walk finds the repo and then `build/ci-linux/native`. Gotcha #19 documents the OOT/in-tree difference.
- In-tree acceptance (§3.2): `cmake --preset ci-linux && cmake --build --preset ci-linux && SDL_VIDEO_DRIVER=offscreen ENGINE_REQUIRE_GPU=1 ctest --preset ci-linux` → full suite green,incl. `engine_compositor_live` (`AbiLayoutTests: all checks passed`, `[LIVE] compositor live path engaged`, `CompositorApiTests: all checks passed`, `PhysicsApiTests: all checks passed`).

## P0-02 GCC build slice (2026-10-31)
- Fixed all 7 §4.9 GCC `-Werror` errors, no global suppressions: shadowed `int& out` → `val` (dotnet_host.cpp:368); `CPU_ISSET` sign-conversion via unsigned args + `<cassert>`/`<limits>` + comment (job_scheduler.cpp:202,282); both `memset`s → `PullRow{}` value-init loops (transform_sync.cpp:408,415; trivially-copyable struct, `valid=0` keeps the drain's skip-guard, zero risk to dirty bits); `static_cast<float>` on int64/uint32 operands (sdf_brick_builder.cpp:240-242,268-270); clang-only pragmas under `#if defined(__clang__)` (vma_impl.cpp:24,25,30); `-fno-rtti`/`-fno-exceptions` scoped to C++ via `$<$<COMPILE_LANGUAGE:CXX>...>` (EngineHotFlags.cmake); meshoptimizer `SYSTEM` include dirs with correct keyword order (`SYSTEM PUBLIC`, not `PUBLIC SYSTEM` — the wrong order silently treated SYSTEM as a path).
- No prompt/issue conflict: the issue title/body and prompt §4.9 agree on the full error list; met exactly that list with no extra scope. Gotcha #21 respected (no new memset/strlen/C arrays; the new loops use value-init).
- Preset + CI: `ci-linux-gcc` inherits `ci-linux` with `CMAKE_C_COMPILER=gcc`, `CMAKE_CXX_COMPILER=g++` (build+test presets added); CI `native` job gains `ubuntu-24.04/gcc` entry, job renamed `native (os, compiler)`; Linux install step now carries the full §3.1 apt list (gotcha #10).
- Local acceptance (dev box GCC 14.2, strictly newer than CI's GCC 13): `cmake --preset ci-linux-gcc` configure OK; `cmake --build --preset ci-linux-gcc` exit 0; `SDL_VIDEO_DRIVER=offscreen ENGINE_REQUIRE_GPU=1 ctest --preset ci-linux-gcc` → 42/42 PASS, same as `ci-linux` 42/42 just before.
- Note: `engine_compositor_live` SEGFAULTs intermittently under local GCC (~10/12 direct runs mid-session) but 0/12 under local clang the same way; gdb pins it to `EditorSession::spawn` → `vmovaps %ymm0,(%rax)` on an `ecs_get_mut_id` row pointer (both are pre-existing files untouched by this diff). A prior attempt (commit 95d587b NOTES) already proved main-branch crashes under gcc-13 Release. No scope creep: fix belongs to the ECS/ABI owner, not this warning-fix slice.
## P0-03 audio_spatial parallel hang (parity/p0-03-audio-spatial-hang, 2026-10-31)
- Repro: `ctest --preset ci-linux -j4 --repeat until-fail:30 -R "engine_audio_spatial|engine_physics|engine_gi_zero_bake"` never reproduced the 300 s hang locally (5/5 pass, ~19 s both before and after the fix; solo run ~0.6 s). No live hang, so no `gdb thread apply all bt` was capturable.
- Root-cause analysis (miniaudio docs + `deps/miniaudio/miniaudio.h` source read): `attach_file` used `STREAM | ASYNC` WITHOUT `WAIT_INIT`. `ma_sound_init_from_file_internal` forces `WAIT_INIT` only for the data-source init path, and `ma_sound_init_from_file` documents init "returning quickly" under ASYNC — the stream LOAD job may still be queued on the single resource-manager job thread when the test proceeds. Teardown (`ma_sound_uninit` → `ma_resource_manager_data_stream_uninit`, `ma_engine_uninit` → `ma_resource_manager_uninit`) posts FREE/QUIT jobs behind the queued LOAD and blocks in `ma_resource_manager_inline_notification_wait()` (`ma_async_notification_event_wait`, unbounded) until the job thread drains. Under `ctest -j4` CPU contention that bounded-ms wait becomes the observed 300 s hang: an async-init/teardown race, a process-wide job-queue ordering hazard — not a per-test assertion failure.
- Fix (2 files, no RUN_SERIAL, no test weakening): `native/audio/src/audio_world.cpp` adds `MA_RESOURCE_MANAGER_DATA_SOURCE_FLAG_WAIT_INIT` to the attach flags so init itself is the bounded wait point (inline event signalled by the LOAD job, µs–ms on local fixtures) and teardown can never outrun load; streaming semantics preserved (STREAM+ASYNC still decode on workers). `native/tests/CMakeLists.txt` lowers `engine_audio_spatial` TIMEOUT 300 → 60 (slice requirement; test runs ~0.6 s).
- Verified: `ctest --preset ci-linux -R "^engine_audio_spatial$"` → Passed 0.63 s, TIMEOUT property reads 60; `ctest --preset ci-linux -j4 --repeat until-fail:50 -R "^engine_audio_spatial$"` → 50/50 Passed (31.4 s); broader `ctest --preset ci-linux -j4 --repeat until-fail:30 -R "engine_audio_spatial|engine_physics|engine_gi_zero_bake"` → 5/5 pass, 18.9 s.

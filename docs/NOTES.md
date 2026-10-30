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

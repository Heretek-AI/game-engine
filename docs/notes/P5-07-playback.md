# P5-07 playback slice (t_f128e37c) — 2026-11-04

- Baseline: ported parent t_460178b7 (ABI 4.18 data model) + sibling
  t_31348e90 (ABI 4.19 sequencing) diffs into this worktree so the
  playback tail builds on both. Final rebase must order 4.18 -> 4.19 ->
  4.20 in one table; sibling concurrently extends the same region
  (hotspot flagged on my card).
- ABI 4.20.0 appends 13 entries after timeline_evaluate_active @ 1080:
  timeline_player_create @ 1088 ... timeline_player_looping @ 1184
  (ApiTable 1088B -> 1192B). player_create(NULL) -> NULL; destroy
  idempotent on NULL; play/pause/stop/seek/advance return 0 or
  ENGINE_ABI_ERR_*; state/time/rate/looping read back via out-params.
- State machine: STOPPED initial; play from STOPPED at/past duration
  restarts at 0, from PAUSED resumes in place; pause freezes in place
  (no-op unless PLAYING); stop rewinds to 0/STOPPED. seek clamps into
  [0, live duration], preserves state, NaN rejected. advance(dt*rate)
  only while PLAYING; negative/NaN dt rejected; hits end ->
  non-loop pins at duration + auto-STOP, loop wraps modulo duration.
  Empty (duration 0) timeline never leaves STOPPED. Duration is live
  4.19 timeline_duration per call (clip edits take effect immediately).
- Verification:
  - `dotnet run --project tools/abi_codegen -c Release -- .` -> 149
    entries regenerated, TimelinePlayer bindings present.
  - `dotnet build Engine.slnx -c Release` -> 0 warnings/errors.
  - `dotnet run --project managed/Engine.Tests` -> 757 PASS, 0 FAIL
    (incl ABI minor == 20, ApiTable 1192B, player offsets, stub +
    type-shape playback probes).
  - native `engine_abi_layout` rebuilt -> all 396 checks pass, incl
    17 new player checks + full play/advance/pause/seek/rate/loop/
    restart/stop round-trip.
  - `ctest -R abi_layout` -> passed.
  - `--live-native-probe` not runnable in this env (engine_core
    DllNotFoundException, same pre-existing failure as sibling/other
    suites); timing contract verified via CTest round-trip + stub.
- Managed: Timeline.CreatePlayer() binds a TimelinePlayer (Timeline
  must outlive it); TimelinePlayer exposes Play/Pause/Stop/Seek/
  Advance + State/Time/Rate/Looping; TimelineState 0/1/2.

# Verify slice (t_68256e9d) — 2026-11-04

- Consolidated sibling commits into one worktree: cherry-picked
  3681df8 (ABI 4.19 sequencing, +5 entries, ApiTable 1088B) then
  777c15a (ABI 4.20 player, +13 entries, ApiTable 1088B -> 1192B).
  Verified 777c15a is a strict superset of 3681df8 (zero deletions
  apart from the MINOR 19->20 line); applied cleanly, no conflicts.
- ABI check: codegen regen (`dotnet run --project tools/abi_codegen`)
  -> 149 entries, bindings byte-identical (CODEGEN-CLEAN).
  `ctest -R abi_layout` -> passed. Native `engine_abi_layout` ->
  all checks passed (timeline 4.18/4.19/4.20 offsets incl player
  create @1088 ... looping @1184). No ABI break: append-only tail,
  earlier entries untouched.
- Managed: `dotnet run --project managed/Engine.Tests` -> 757 PASS,
  0 FAIL (layout + stub/type-shape probes). Staged
  build/native/libengine_core.so.0.1.0 for live probe ->
  `--live-native-probe` -> 1056 PASS incl timeline live round-trips
  (duration 7, clip_get, eval overlap/weights, trim clamp/drop,
  player STOPPED->PLAYING->PAUSED, rate 2 + loop wrap 0.5).
  2 remaining FAILs are pre-existing env issues: ui_create (no font
  device) + engine-cli binary not built — unrelated to timeline.
- Track/clip creation, sequencing, playback all covered by
  TimelineApiTests (shape + stub + live); no tests skipped.

# Rebase slice (t_808f5752) — 2026-11-04
- Rebased sequencer onto origin/main @ 001f8f3: main had taken ABI 4.19
  (P5-04-followup renderer_attach_ui @ 1000, ApiTable 1008B), colliding
  with the sequencer's 4.19 sequencing numbering. Renumbered sequencer
  tail: data-model 4.18 -> 4.20 (@1008...1048), sequencing 4.19 -> 4.21
  (@1056...1088), playback 4.22 (@1096...1192), MINOR 20 -> 22,
  ApiTable 1192B -> 1200B. Append-only; main's entries untouched.
- Fixed Sonar C-reliability gate: replaced `new (std::nothrow)` +
  null-check with value-initialized `new EngineTimelineImpl()` /
  `new EngineTimelinePlayerImpl()` (throws std::bad_alloc on OOM —
  fail-fast, no null-deref path).
- Transplanted: header comment/typedefs/table, cpp asserts/impl/table
  wiring, codegen Timeline mirrors, native layout checks, managed
  AbiLayoutTests offsets, Timeline.cs/Runtime, TimelineApiTests,
  Program.cs registration.

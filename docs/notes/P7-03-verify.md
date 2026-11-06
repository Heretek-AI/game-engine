# P7-03 verify — CI export-boot + cold-boot gate (qa-gate)

Worktree `wt/t_5d96f5a1`. Parents: t_0cc685d5 (Build window UI) and
t_321722f3 (headless runtime + build-player CLI + PlayerBuild API),
both `done`. Root slice card: t_42e3fbfb (issue #132, `todo`, unblocks
when this card completes).

## 1. Dependency gate (2026-11-04, verified before code)

- `git merge-base --is-ancestor 17054e8 origin/main` → P2-07 IN origin/main
  (PR #204 `parity/p2-07-player`).
- `git merge-base --is-ancestor 26f73c2 origin/main` → P3-11 IN origin/main
  (`[P3-11] Script workflow and hot reload`, covers the P3-10 pak path via
  `ExportPipeline.PackAssets`, present in tree).
- Spec re-read: `docs/PARITY_IMPLEMENTATION_PROMPT.md` §2 (rules),
  §7 (gotchas 2, 11, 21, 22 honored by the player), §8 (done criteria),
  §6 P7 (`P7-03 Player builds from the editor … cold boot < 1 s measured`).

## 2. Change (one file, CI only)

`.github/workflows/ci.yml`: new `player-export-boot` job, 3 lanes
(ubuntu-24.04/linux-x64, macos-15/osx-arm64, windows-2025/win-x64),
each lane exports MarbleRun for its own host RID via
`tools/build-player` (native builds because target == host RID on every
lane), then boots the EXPORTED layout:

```
cd dist/<rid>/
./engine-player . --content ./game.pak --frames 10 \
  --screenshot "$RUNNER_TEMP/p703-boot.png" --hidden
```

then `Enforce cold boot < 1.0 s` greps `COLD_BOOT_S=<s>` from the tee'd
log, echoes it, and awks `exit 1` when missing or `>= 1.0`.
Screenshot + boot log upload as `player-boot-<rid>` /
`player-boot-log-<rid>` artifacts (`if-no-files-found: error`).
Env mirrors the native job: offscreen+`ENGINE_REQUIRE_GPU=1` on Linux,
MoltenVK ICD probing on macOS, Lavapipe SDK on Windows; `shell: bash`
forced so `PIPESTATUS`/export syntax works on Windows git-bash.
No `engine_abi.h` change (ABI stays 4.23); no test touched.

## 3. Local proof of the exact CI gate commands (2026-11-04, dev box)

Export (mirrors the `Export MarbleRun` step):

```
dotnet run --project tools/build-player -c Release -- \
  --target linux-x64 --project MarbleRun --out /tmp/p703-verify/dist
# build-player: native=built (engine-player)
# build-player: game=MarbleRun.dll pak=game.pak entries=0
# dist/linux-x64: engine-player + MarbleRun.dll + .runtimeconfig.json
#   + game.pak (24 B, 0 entries — MarbleRun ships no packable assets)
#   + project.json + cmake-configure.log + build-manifest.json
```

Boot + gate (mirrors `Boot exported player` + `Enforce` steps,
`ENGINE_REQUIRE_GPU=1`, offscreen):

```
cd /tmp/p703-verify/dist/linux-x64 && \
SDL_VIDEO_DRIVER=offscreen ENGINE_REQUIRE_GPU=1 \
./engine-player . --content ./game.pak --frames 10 \
  --screenshot /tmp/p703-verify/boot3.png --hidden
# rc=0
# COLD_BOOT_S=0.085
# PLAYER-OK
# cold boot gate passed: 0.085s < 1.0s
```

Three consecutive runs: 0.078 / 0.073 / 0.085 s — all < 1.0 s with ~12x
margin. Screenshot written every run (640x360 RGBA PNG, ~9 KB).
`dotnet_host bind FAILED … rc=-2147024894` lines in the log are the
designed demo-frame fallback: staged dist/ stages only dll +
runtimeconfig (missing deps.json + Engine.* siblings), so the loader
falls back to the HelloCube-equivalent frame; COLD_BOOT_S / screenshot /
PLAYER-OK path unaffected. Full MarbleRun course boot (COLD_BOOT_S=0.094)
already proven against `samples/` in t_321722f3 / docs/notes/P7-03-backend.md.

YAML validity: `python3 -c yaml.safe_load` → jobs
`['managed', 'native', 'player-export-boot', 'supply-chain']`.

## 4. Collision avoidance (hotspot note)

`sibling t_0cc685d5 owns managed/Engine.Editor/PlayerBuild.cs`
(contract `IPlayerBuilder`, 91 lines) while this card's backend parent
`t_321722f3` owns the same path (pipeline `PlayerBuild`, 534 lines).
Prior run of this card staged renamed `EditorPlayer*` copies of the UI
files — removed (deleted the untracked copies, reverted MainWindow /
Tests Program / ci.yml staging). This card now touches ONLY
`.github/workflows/ci.yml` + this notes file; the UI/backend filename
collision is left for the root slice owner (t_42e3fbfb) to reconcile at
PR time.

- `hotspot: managed/Engine.Editor/PlayerBuild.cs — UI contract (t_0cc685d5) vs backend pipeline (t_321722f3) claim the same path; root PR must reconcile`

## 5. Not proven here (follow-ups for the root PR)

- macOS/Windows lanes were validated by construction only (same script,
  per-OS env blocks copied from the green native job); no Mac/Windows
  hardware on hand. First CI run on the root PR is the real proof.
- No PR opened from this card (`completion_contract: local-only` scope);
  CodeRabbit/CodeQL threads can only be resolved once the root slice PR
  exists — flagged in the handoff metadata.
- `game.pak` 0-entry placeholder and non-self-contained staged dist/
  (demo-frame fallback) are known backend gaps, documented in
  docs/notes/P7-03-backend.md §4.

## Links

- Issue: https://github.com/Heretek-AI/game-engine/issues/132
- Deps: P2-07 PR #204 (merged 17054e8), P3-11 #175 (merged 26f73c2)
- Siblings: docs/notes/P7-03-ui.md (t_0cc685d5),
  docs/notes/P7-03-backend.md (t_321722f3)

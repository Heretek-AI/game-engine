# P7-03 backend — validation notes (qa-gate)

Worktree `t_f6af9fa5` (= `wt/t_eb44bc28` merged in: `657fe3f`
build-player CLI + PlayerBuild API, `2a16b2d` headless runtime +
COLD_BOOT_S + headless gate). No new code in this card; validation only.

## 1. build-player matrix (2026-11-04, dev machine)

```
dotnet run --project tools/build-player -c Release -- \
  --target <rid> --project MarbleRun --out /tmp/p703-dist
```

| target | result | dist/<target>/ contents |
|---|---|---|
| linux-x64 | native **built** | engine-player + MarbleRun.dll + MarbleRun.runtimeconfig.json + game.pak (0 entries) + project.json + cmake-configure.log + build-manifest.json |
| win-x64 | native **configured-only** | MarbleRun.dll + runtimeconfig + game.pak + project.json + configure log + manifest (no binary: no Windows toolchain here) |
| osx-x64 | native **configured-only** | same managed+pak set as win-x64 |
| osx-arm64 | native **configured-only** | same managed+pak set as win-x64 |

Exit 0 on all four. `game.pak` has 0 entries: MarbleRun ships no
packable assets under the sample dir, so `PackAssets` throws
"no packable assets" and `PlayerBuild` stages the zero-entry
placeholder (by design — dist/ stays a complete unit).

## 2. headless boot + cold-boot time

Correct invocation (project dir FIRST — `<project_dir>` is positional;
`--content` takes the pak path, not the other way round):

```
cd /tmp/p703-dist/linux-x64 && SDL_VIDEO_DRIVER=offscreen ENGINE_REQUIRE_GPU=0 \
  ./engine-player . --content ./game.pak --frames 10 --screenshot /tmp/p703-shot.png
```

Result: rc=0, `COLD_BOOT_S=0.076`, `[PASS] player: 10 frames without
close`, screenshot written (640x360 RGBA PNG), `PLAYER-OK`.
Acceptance gate (<1.0 s) passes with ~13x margin.

Two notes from the run:

- **Staged-dist boot falls back to the demo frame.** The staged dist/
  copies only `MarbleRun.dll` + `.runtimeconfig.json`; the loader then
  fails `load_assembly_and_get_function_pointer` (rc=-2147024894,
  FILE_NOT_FOUND — missing `MarbleRun.deps.json` and the
  `Engine.Bindings/Runtime.dll` siblings) and boots the
  HelloCube-equivalent demo instead of the marble course. The
  screenshot/COLD_BOOT_S/PLAYER-OK path is unaffected — the demo frame
  is the designed no-bootstrap fallback, and CTest's
  `engine_player_headless` accepts it.
- **Full MarbleRun boot verified separately.** Against
  `samples/MarbleRun` (deps + build outputs present) the same player
  binary binds `Game.Bootstrap.Boot` and boots the real course:
  `COLD_BOOT_S=0.094`, rc=0, PLAYER-OK, no bind FAILED lines.
  `engine_marble_autoplay` (1500-frame replay) passes in the same run.

## 3. test suites (nothing skipped, nothing weakened)

- Managed: `dotnet run --project managed/Engine.Tests -c Release` →
  rc=0, **832 PASS lines, zero FAIL**, all 22 suites report
  "all checks passed" incl. `PlayerBuildTests`. The 11 `[SKIP]` live
  lines are pre-existing no-native-lib guards, untouched by this slice.
- Native: `cmake --preset ci-linux` + `cmake --build --preset ci-linux`
  clean, then
  `SDL_VIDEO_DRIVER=offscreen ENGINE_REQUIRE_GPU=1 ctest --preset ci-linux
  -R "engine_player_headless|engine_marble_autoplay|engine_player_hello|
  engine_player_autoplay|engine_hello_cube"` →
  **8/8 passed** (incl. `engine_player_headless` COLD_BOOT_S + PLAYER-OK
  and `engine_marble_autoplay` exit-0 + `Finished in`).

## 4. gaps / follow-ups (not blockers for this card)

1. **Staged dist/ is not self-contained for bootstrapped games.**
   `PlayerBuild` stages only the game dll + runtimeconfig; the hostfxr
   load needs `deps.json` + referenced Engine.* assemblies alongside.
   For a real game project the publish layout may already provide them;
   for `dotnet build`-layout samples it does not. Follow-up:
   `PlayerBuild` should stage the full build-output closure (or publish
   the game project) so dist boots the *game*, not the demo frame.
2. **`game.pak` 0-entry placeholder.** Fine for MarbleRun (no assets),
   but the empty-pak path has no negative test (corrupt-pak exit-2 is
   covered only by the native `--content` fail-closed branch, not by a
   dedicated ctest).
3. **Cross targets are configure-only.** win/osx dists contain no
   native binary — expected (no toolchains on this machine), but no CI
   lane has yet built them, so first real cross build may surface
   toolchain issues.

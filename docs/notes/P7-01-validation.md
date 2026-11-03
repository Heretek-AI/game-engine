# P7-01 validation: Vulkan device log + zero GPU skips + goldens (2026-11-03)

Slice: P7-01 (issue #130). Depends on: P7-00 (#129, merged via #140) and
the P1-05 golden harness — both present on origin/main.
Validation run from worktree `wt/t_7ee1a8f8` at `a560fbc`
(P7-01 itself merged as `c9f9221`, PR #183, ancestor of this checkout —
no code changes needed, validation only).

## Depends-on check

- `git fetch && git log origin/main --oneline | grep`: `c9f9221 [130] [P7-01]
  Windows GPU lane in CI (#183)` present; `git merge-base --is-ancestor
  c9f9221 origin/main` → yes. P7-00 (`481f7d0 [P7-00] Spike ... (#140)`)
  also on origin/main. Proceeding.

## Windows CI log (run 41572940244, main push 2026-11-03T03:33:47Z, job 125709820529)

Probe step (`Probe Vulkan device (Windows)`, `vulkaninfoSDK.exe --summary`):

```text
apiVersion         = 1.4.318
deviceName         = llvmpipe (LLVM 21.1.4, 256 bits)
driverID           = DRIVER_ID_MESA_LLVMPIPE
driverName         = llvmpipe
```

Acceptance "log shows a Vulkan device" — met.

Test step (`unset SDL_VIDEO_DRIVER; ENGINE_REQUIRE_GPU=1; ctest`):

```text
6:        gpu: llvmpipe (LLVM 21.1.4, 256 bits) api=1.4.318
6:        portability: enumerate=portability_enumeration device=llvmpipe (LLVM 21.1.4, 256 bits)
8:        portability: enumerate=portability_enumeration device=llvmpipe (LLVM 21.1.4, 256 bits)
100% tests passed out of 74
```

`[SKIP]` count = 2, both in `engine_export_live` (test 25) and both
non-GPU, pre-existing opt-in gates unrelated to this slice:

```text
25: [SKIP] export live: AOT publish (set ENGINE_TEST_EXPORT_AOT=1 to run ~2min ILCompiler)
25: [SKIP] export live: no ExportSample binary staged (cold-boot via ctest export fixture instead)
```

`[FAIL]` count = 0. Zero GPU-test skips — every `gpu_missing()` fail-closed
site reports `[PASS] ... device init ok` on Windows. Goldens all pass
(`[PASS] golden: clear_orange_320x240`, `triangle_320x240`,
`cube_lit_320x240`, `grid_1000_640x360`, `sprites_10k_640x360`,
`tilemap_16x256x256`, plus 160 `golden:` lines total, zero
`[FAIL] golden` / `SWAPCHAIN_FAILURES`). Same main run: gcc lane
`100% tests passed, 0 tests failed out of 75`.

## Local Linux-equivalent run (this worktree, build `ci-linux`, clang)

`vulkaninfo` is not installed in this sandbox, but the loader ICD dir has
`lvp_icd.json` and the engine enumerates the CPU device directly:

```text
$ SDL_VIDEO_DRIVER=offscreen ENGINE_REQUIRE_GPU=1 ./build/ci-linux/native/tests/engine_swapchain
[PASS] swapchain: window 320x240
       portability: enumerate=portability_enumeration device=AMD Radeon Graphics (RADV GFX1151)
[PASS] swapchain: device init ok
...
[PASS] golden: clear_orange_320x240
...
SWAPCHAIN_OK
```

Full suite: `SDL_VIDEO_DRIVER=offscreen ENGINE_REQUIRE_GPU=1 ctest
--preset ci-linux` → `100% tests passed, 0 tests failed out of 72`
(15.51 s). GPU-focused filter (8 tests, full `-V` capture):
`[SKIP]` = 0, `[FAIL]` = 0, `[PASS]` = 340, goldens
`clear_orange / triangle / cube_lit / grid_1000 / p4_02_hdr /
p4_03_csm_pillar(+shifted) / p5_05_particles` all `[PASS]`.

## Gotcha re-confirmed (matches ci.yml review-round-2 comment)

An empty-but-set `VK_ICD_FILENAMES=""` DISABLES the loader's default ICD
search — all 11 GPU tests fail with `[FAIL] ... no physical device (no
ICD)`. The CI script unsets it on Linux for exactly this reason; local
runs must `unset VK_ICD_FILENAMES` (not set it empty).

## Windows-equivalent test command

Cannot execute MSVC/Win32 here; the equivalent is the command above plus
`unset SDL_VIDEO_DRIVER` (Windows uses a real Win32 window per
`ENGINE_TEST_VIDEO_ENV` in `native/tests/CMakeLists.txt:14`), which is
exactly what the CI `Test (CTest)` step runs. Windows evidence comes from
the CI log quoted above, per the card's approach (2).

## Conclusion

Acceptance met: CI log shows a Vulkan device (llvmpipe 21.1.4 /
api 1.4.318), zero GPU tests skipped (2 skips are pre-existing non-GPU
export opt-ins), goldens pass within tolerance on Windows and locally.
No repo changes made — validation-only run.

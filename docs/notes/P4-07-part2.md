# P4-07 part 2 — Real GI frame integration (resolve radiance into shade)

## Spike (2026-11-02, §4 facts before slice code)
- Real frame path: `ForwardRenderer::render_world` (ecs_render.cpp) draws
  ECS `MeshRenderer` entities through the **mesh pipeline**
  (`mesh.vert/mesh.frag`, P1-07 Lambert + 0.1 ambient, sRGB encode in
  shader). The PBR plates (`pbr_shade`) are NOT used by render_world —
  "select SHADE_GI_PROBE in render_world" therefore means adding the GI
  term to `mesh.frag`, not switching plate systems.
- `MeshPush` is 160B, no spare lane (`light_dir.w` = P4-03 shadow toggle,
  `base_color.w` = alpha). Mesh pipeline layout is push-only (no sets).
  Design: new set-0 (binding 0 = probe COMBINED_IMAGE_SAMPLER, binding 1
  = box UBO) + gain-in-UBO (`size.w = 0` disables, no push change).
- `GpuMesh` is GPU-only (VMA, no CPU verts). But `upload()` HAS the CPU
  verts — bake `MeshSdf` bricks there and keep them on the mesh.
  render_world gathers bricks from the registry (per-mesh world transform
  from ECS) → voxelize into occupancy+albedo grids → Jacobi baker
  (generalized part-1 Gather: ray-march grids instead of analytic
  spheres) → 3D probe image uploaded per frame (tiny scenes) → bound
  for the mesh draws.
- `GiFrameGraph` only builds pipelines (no record, no probe filling).
  `gi_resolve` binds `g_probes` but never samples it (sky-only output).
  Part-2 execution: record resolve in render_world into a real image;
  consume = mesh.frag already gets radiance from the baked probe volume,
  so resolve output stays occlusion-diagnostic until a radiance term
  lands (documented, no fake consumption).
- Golden harness: `test_ecs_render.cpp` renders through render_world
  with golden `grid_1000_640x360` — extend with a GI scene golden.
- ABI: none (native-only; box/push/set layout are renderer-internal).
  New ABI entries would take 4.15 — not needed.
- Sequencing: (1) GiBaker lib + unit tests, (2) brick bake at upload +
  brick→grid injection, (3) mesh.frag probe term + set layout, (4)
  render_world wiring + golden, (5) graph execute in frame. Green CI at
  each commit; no staged goldens; notes here.

## Fix log (2026-11-03 — ON-frame GI term invisible → passing differential)
- Symptom: `test_ecs_render_gi` masked differential failed
  (shift best=0.0106, need >0.03); pinned ON golden in HEAD was black
  (stale).
- Root cause 1 (framing): the GI eye sat OUTSIDE the room
  (19,12.5,29): the near (right/back) walls occluded the interior and
  the room rendered as a tiny exterior block in a black void (vision
  confirmed). Fix: eye INSIDE the room's open front (8.5,3.0,1.5),
  looking at the floor/back-wall junction — the interior now fills the
  frame (no void pixels).
- Root cause 2 (not a bug — documented non-issue): a mid-run attempt
  copied the GiBaker CPU-oracle scale (gain 4 x intensity 3) into the
  frame bake; that bakes irradiance ~12x hotter than the direct term
  and blows the ON frame to full white. The frame keeps UNIT scale
  (intensity 1 x gain 1, committed in b3ac557's parent scope): the
  mesh.frag indirect term (albedo x irradiance / pi) lands a ~20% red
  lift, visible in the differential, never clipped.
- Verification (tool-backed, xvfb + lavapipe-style software GL):
  `gi shift best=0.112245 tinted=52759 maxabs=43.0`; per-pixel probe of
  the ON `.actual.png` shows an R/G 1.00→1.03 floor gradient toward the
  red wall (x 200..560 at y=250/300); `ENGINE_GI_BOX_ONLY=1` bisection
  collapses the differential to exactly 0.0 (binding path live).
- Why the ON frame still looks GI-less to the eye: the sRGB-encode +
  8-bit rounding of a ~2% red lift is below visual threshold — the
  effect is real in pixel data (R/G ratio) but not obvious in a
  thumbnail. The differential mask therefore admits bright mid-tone
  floor pixels (ORR 60..235, spread 25) and asserts on the R/G ratio
  lift, not an absolute-code jump.
- Goldens regenerated from the fixed production path
  (`ENGINE_UPDATE_GOLDENS=1`, interior camera + unit bake):
  `gi_cornell_ecs_640x360.png` (ON) + `gi_tmp_off_640x360.png` (OFF);
  full `engine_ecs_render` run is green (`ECS_RENDER_OK`).
- Debug paths (cleanup verified 2026-11-03): the mid-diagnosis `GIDIAG`
  stderr lines, `ENGINE_GI_BOX_ONLY` bisection, and
  `ENGINE_DUMP_GI_DRAWS` eye/view-proj dumps are REMOVED from the tree
  (`grep GIDIAG|BOX_ONLY|DUMP_GI native/` is clean; the upload-proc
  null guard keeps its error return, minus the stderr line). Post-cleanup
  `engine_ecs_render` is green (`ECS_RENDER_OK`, same differential
  `best=0.112245 tinted=52759 maxabs=43.0`).
- Remaining for merge: CI green every OS; CodeRabbit run + all threads
  answered. OFF golden = direct path via `set_gi_enabled(false)` (the
  production OFF path — render_world skips bake, gain stays 0);
  ON golden = production bake+upload path.

## Rebase onto origin/main (2026-11-04 — part-2 port, no re-implementation)
- Old branch `origin/parity/p4-07-gi-frame-integration` (base e297a7b,
  pre-part-1-merge + PARITY-84 scene-IO) could not merge: main moved on
  (P4-12 LOD registry/select, P5-02 helper extraction, P5-05 propagate
  ensure). Ported the GI-only delta onto fresh `parity/p4-07-gi-resolve-shade`
  from origin/main: GiBaker+baker test (new files), GpuMesh brick bake,
  mesh.vert `v_world` + mesh.frag probe term + regen both `.inc` mirrors,
  ForwardRenderer probe set-0/resources/bake/upload/record-bind/destroy,
  `set_gi_enabled`, render_world bake hook, GI scene test + both goldens
  (reused bit-identical from the old branch).
- Fix-forward vs stale main (attempt 1 of 3 — root-caused, no golden regen):
  both goldens initially mismatched (ON frac=0.0294, OFF frac=0.0266, same
  triangular-hole pattern in the back wall in BOTH frames, differential
  still best=0.112245). Bisected out the bake, the set bind, and the SDF
  brick upload (holes persisted in OFF with all three disabled) → not the
  GI path. Root cause: `mesh_test_helpers.hpp` `-Z` cube-face corner order
  regressed by the P5-02 round-7 helper extraction (3637e0c): v2/v3 swapped
  vs the old branch, so Tri1/Tri2 overlap on one half and leave the other
  half (left triangle) uncovered — the +Z back face (bright, lit) shows
  through. One-line fix restores the old order; grid + GI + baker green,
  goldens match without regeneration:
  `gi shift best=0.112245 tinted=52759 maxabs=43.0`, `ECS_RENDER_OK`,
  `GAME_GI_BAKER_OK`. Shared-helper ownership note: the fix touches the
  P5-02-owned helper (additive one-line winding restore, no API change);
  grid/pick/forward-mesh goldens all still pass.
- ABI: none (native-only; no `engine_abi.h` change, no binding regen).

## CI rounds (2026-11-04/05 — fixes on top of the port)
- Round 1 (MSVC + MoltenVK, fixed in c5173b3): MSVC C4251 on
  `GiBaker::sdf_/albedo_` (`std::vector` in `ENGINE_CORE_API` class) —
  added the standard `_MSC_VER` 4251 push/pop pair (matches every other
  renderer header). MoltenVK: `texture()` with a combined sampler3D
  inside the divergent gain branch fails the MSL transpiler
  (`undeclared probeVolSmplr`) — switched the probe fetch to
  `textureLod(..., 0.0)` (single-mip volume, exact; derivative-free).
  Lavapipe output unchanged (goldens still match).
- Round 2 (layout mismatch + undefined image, real bugs found via
  validation layers): (a) the skinned pipeline shares `mesh.frag` but
  its set-0 layout was the joint STORAGE buffer — incompatible with the
  frag's set-0 sampler3D+UBO (lavapipe intermittent segfault/hang in
  `engine_forward_mesh`, MoltenVK pipeline refusal). Fix: joints move to
  set 1 (`mesh_skinned.vert`, `kSkinSetJointUbo = 1`), skinned pipe layout
  is `[shared probe set 0, joint set 1]` (probe layout+resources created
  on demand so skinned-only renderers work; shared when legacy builds
  first), `record_mesh_skinned` binds both. (b) `mesh_skinned.vert`
  never wrote the frag's `location = 2` (`v_world`) — added
  `v_world = (pc.model * skinned_pos).xyz`. (c) The probe image sat in
  UNDEFINED until the first bake, but every draw binds it as
  SHADER_READ_ONLY (VUID-vkCmdDraw-None-09600) — `create_probe_resources`
  now transitions UNDEFINED -> SHADER_READ_ONLY with a one-time submit;
  uploads barrier from there (dirty flag preserved as fallback).
  Full local matrix green after the fix: `engine_forward_mesh` 6/6 stable
  (was intermittent segfault), full `ctest -j` 74/74 pass
  (compositor_live staging link is order-dependent under `-j` but passes
  serially/alone; unrelated to this slice).

## CI round 2b (2026-11-04/05 — mac black frame, fixed)
- Symptom (macOS-15 only): ON frame fully black (frac=1.0, maxabs=211,
  shift=0) while OFF + all other tests passed; `gi probe live=1`
  (bake+upload reported success CPU-side).
- Root cause: the probe upload (barrier + buffer→3D-image copy) was
  recorded into the caller's command buffer INSIDE the open
  dynamic-rendering mesh pass. Legal Vulkan, but transfer work inside a
  render pass instance breaks tile-based drivers (MoltenVK renders the
  frame black; lavapipe tolerates it). Fix: `upload_probes` writes the
  box UBO inline (CPU-mapped, no GPU work) but performs the image
  barrier+copy as a fence-waited one-time submit on a transient pool —
  never inside an open pass. New stored procs + queue handle follow the
  file's existing stored-proc pattern; stub build keeps `void*`.
- Lavapipe output unchanged (goldens still match, best=0.112245).

## CI rounds 5-6 (2026-11-05 — CodeRabbit + Sonar, all green)
- CodeRabbit Majors (all addressed, verified locally):
  - ON->OFF stale gain: `render_world` now pushes a gain-0 box when
    disabled after a live bake; test asserts OFF-after-ON bit-identity.
  - `memcpy` of alignas(64) WorldTransform from flecs columns (potential
    AVX2 `vmovaps` fault): byte loop in the bake gather.
  - Shared box/staging UBO race across frames in flight: steady-state
    skip-if-unchanged write (bake is deterministic; identical bytes are
    not rewritten).
  - Skipped (stale): skinned frag interface (already fixed in round 2 by
    shared set-0 probe layout), transfer-inside-pass (fixed in round 2b
    by the fenced one-time submit), always-bake-bricks perf note (one
    capped bake per upload, out of scope).
- Sonar QG (B -> pass): S5945 C arrays -> `std::array` (+ `.data()` at
  Vulkan/C call sites), S5827/S1905 auto + dropped redundant `void*`
  casts, S6022 `std::byte` loop, S107 TraceCtx bundle, S1188/S3574
  helper extractions (AllocProbeHostBuffer, SampleSdfTrilerp,
  gather_probe, shared test helpers), S6004 init-statements, S5817
  const, S6181 bit_cast, S6164 pi_v, S6188 span bake params. Remaining
  S3630/S5008 are the file's established Vulkan proc-loading pattern
  (`reinterpret_cast` void*->PFN is legally required; `static_cast`
  is ill-formed) — same as merged PRs.
- MSVC C4702 (unreachable `return MakeOk` after upload split) fixed;
  GCC `-Wconversion` size_t indices fixed.
- Test-only cherry-pick a3e6e71 (P5-03 physics RigidBody zero-init)
  included: the branch's Windows run hit that known flake.
- Final matrix: full native (2x mac, 2x ubuntu-clang, 2x ubuntu-gcc,
  2x msvc) + managed + CodeQL + Analyze + Sonar + supply-chain green.

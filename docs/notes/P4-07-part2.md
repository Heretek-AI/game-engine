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

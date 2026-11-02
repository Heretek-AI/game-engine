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

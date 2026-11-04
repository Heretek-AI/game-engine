# P5-05-followup — Draw particle entities in the ECS frame

## 2026-11-03 — implementation (branch `parity/p5-05-particles-ecs-frame`)

Follow-up to #192 (P5-05 merged): `ParticleGpuSim` existed
(update/compact/emit/finalize + GPU golden) but `render_world` and
`renderer_bridge` never stepped or drew `ParticleSystemComponent` entities.

### What changed

- `native/renderer/include/engine/renderer/gpu_mesh.hpp` +
  `native/renderer/src/gpu_mesh.cpp`: new `GpuMesh::write_raw` — refresh an
  ALREADY-CREATED mesh in place (host-visible coherent mapping, same memcpy
  as `create_raw`). Capacities recorded at create time (`vert_bytes_` /
  `idx_bytes_` + persistent `vert_mapped_` / `idx_mapped_`); counts
  replaced per call so one persistent max-cap mesh serves a variable-size
  particle frame with no per-frame allocation. Both counts 0 + null data
  clears the draw. SDK-free stub returns `kVulkanLoaderFailed`.
- `native/renderer/include/engine/renderer/particle_renderer.hpp` +
  `native/renderer/src/particle_renderer.cpp`: new
  `ParticleRenderer::continue_particle_pass` — same color-only dynamic
  rendering pass as `begin_particle_pass` but LOAD (not CLEAR), so
  billboards composite OVER the already-drawn mesh frame. SDK-free stub
  included.
- `native/bridge/src/renderer_bridge.cpp`: bridge-owned emitter table —
  one `ParticleEmitter` per live `ParticleSystemComponent` entity (fixed
  array of 8; fixed array because unique_ptr sidecars are move-only and
  vector reallocation needs portable copy/move under -fno-exceptions).
  Each emitter owns a `ParticleGpuSim` + `ParticleRenderer` + persistent
  max-cap (512-slot) quads `GpuMesh` + reuse-only scratch vectors (no
  frame-path malloc/new — grown once, reused). Per frame (inside
  `RenderOne`, same `has_main` gate as the mesh draw):
  1. `ParticleSyncEmitters`: query `Engine.Runtime.ParticleSystem`,
     new entity -> create sim (reset params) + fx + max-cap quads;
     settings hash (FNV-1a) change -> `sim.reset` only; missing entity
     stalls `last_seen_frame` (reaped after 600 frames TTL).
     Emission clamped to the 512 cap so an oversized rate can never
     overrun the slots buffer.
  2. `ParticleStepEmitters`: one fixed 1/60 s compute step per emitter in
     a fence-waited one-shot submit from a lazily created TRANSIENT pool
     (read_back valid BEFORE the draw CB opens), then read back + sort
     by view depth (far first for the alpha pipe) + `ParticleEmitBillboards`
     + `write_raw` into the persistent quads. Emitter WorldTransform
     translation offsets the verts (slots stay sim-pure).
  3. `ParticleDrawEmitters`: one shared LOAD pass (first live emitter
     opens, every live emitter records one `record_draw`, same emitter
     closes); skipped entirely when no quads are live. View matrix comes
     from the real frame (`render_world`'s `out_view`), viewport from the
     swapchain extent. Fail-closed throughout (any particle error fails
     the frame, never a half-draw).
  Offscreen `render_to_buffer` / pick do NOT step particles (they own no
  sidecars; windowed frames are the real frame path the golden covers).
- `native/ecs/src/ecs_world.cpp` (`EcsWorld::propagate`): ensure the
  `WorldTransform` row exists for every `LocalTransform` entity before
  the cascade (idempotent `ecs_add_id` + identity seed). ABI-spawned
  entities (`entity_create` + `entity_set Local`) carry Local but no
  World row, so the cascade query (Local, World(cascade), World) never
  matched them and World stayed missing — the camera eye fell back to
  identity and every mesh/particle frame rendered black. Caught because
  the follow-up's cube control frame was black too.
- `native/tests/test_ecs_particles_frame.cpp` + CMake: new acceptance
  `engine_ecs_particles_frame` — builds the world via the ABI, runs 60
  frames through the REAL `RenderOne` path, screenshots the last frame,
  decodes via `GoldenLoadPng`, and compares against new golden
  `native/tests/golden/p5_05_ecs_frame_320x240.png` (deterministic plume:
  120/s, 2 s life, seed 1234, eye (0,1.2,4), end_size 0.5). A second
  emitter-free run must render a background-clear frame that DIFFERS
  (proves the plume comes from the particle path, not the clear color).

### Verification (lavapipe, `SDL_VIDEO_DRIVER=offscreen`)

- `engine_ecs_particles_frame`: all PASS, golden passes without update,
  empty-frame differs check passes.
- Neighbors still pass: `engine_ecs_render` (grid golden + 1000 draws),
  `engine_particles` (all checks incl. GPU golden).
- Golden PNG vision-verified: centered warm plume (pale creamy base ->
  golden middle -> muted orange-tan top puffs), ~150-180 px tall, pure
  black background, no mesh visible (mesh pass correctly draws nothing).
- `engine_core` (`engine_core_renderer` + `engine_core_bridge`) builds
  clean under `-Werror` (Clang).

### Gotchas (worth recording)

- `Gprof`: `GpuMesh::create_raw` index buffer sized `index_count * 4`
  bytes — the follow-up's first quads upload passed a 6-index buffer
  for a 2048-vert mesh, so every later `write_raw` failed the capacity
  check. Fix: upload the FULL cap×6 index topology up front (real quad
  indices, not zeros).
- `render_world`'s camera eye falls back to identity when the camera
  entity has no `WorldTransform` row — silent black frames. The ABI path
  never added the row; `propagate` now ensures it.
- `RendererImpl` emitters: `std::vector<ParticleEmitter>` does NOT
  compile (unique_ptr members, -fno-exceptions) — fixed array instead.
- Golden framing: perspective eye (0,1.2,8) makes the 2.5-unit plume
  ~15% of frame height (one center blob); eye z=4 + end_size 0.5 gives
  the golden its presence. (The P5-05 standalone golden used an ortho
  camera; the frame path uses the perspective main camera.)

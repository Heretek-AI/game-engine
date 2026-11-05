# P3-08-followup — #180: dropped-model mesh_id hash vs registry id

## 2026-11-04 — Spec read + branch + ABI sequencing
- Read `docs/PARITY_IMPLEMENTATION_PROMPT.md` §§2, 7, 8 + §6 P3 line + §4.5
  before code. Issue #180: `EditorSession::drop_asset` writes
  MeshRenderer.mesh_id as an FNV-1a content hash, but MeshRegistry hands
  out sequential ids from renderer uploads — dropped models never
  resolve/draw (rare hash collision risk too). #180 is extra scope (NOT
  in the #56 done-when #65–#138); still worth fixing since P3-08
  acceptance ("visible in one frame") depends on it.
- `git fetch`; branch `parity/p3-08-mesh-id-180` from `origin/main`
  (`2ac48ce`, P4-10 merge). P3-02 #88 is CLOSED (viewport path exists:
  `ViewportPresenter` + `Renderer.CreateOffscreen`/`MeshCreate` in-tree).
- ABI sequencing (rule: append at END, minor +1, next unused): base minor
  = **25** (P3-05 `entity_remove` tail, table 1288B). This slice takes
  **4.26** with ONE entry: `renderer_upload_mesh(renderer, asset_path) ->
  mesh_id`. No other open branch in this worktree touches the ABI.

## 2026-11-04 — Design (why a renderer-side upload, not an assets change)
- The MeshRegistry/RenderImpl/GpuMesh/cgltf-include all live on the
  renderer side, and the bridge already links assets+cgltf
  (`ENGINE_ASSETS_HAS_CGLTF` + include dir are PUBLIC usage from
  `engine_core_assets`; the bridge gets both for free). A new assets-side
  API would add a cross-layer surface for one viewport call — rejected.
- `EditorSession::drop_asset` keeps writing the FNV-1a content hash as
  the spawn-time placeholder (deterministic, fail-closed codes
  unchanged). The viewport uploads via path vertices (flat shaded when
  NORMAL absent) and rewrites `MeshRenderer.mesh_id` through `Ecs.Set`.
  Fail-closed: no-GPU → content id stays, draw skips since
  `registry.find` fails (never a collision-driven wrong draw).
- cgltf note: `asset_importer.cpp` is the single CGLTF_IMPLEMENTATION TU
  (dual definitions break the `engine_core` link — hit during build,
  fixed by header-only include here). Same POSITION/NORMAL/TEXCOORD_0
  decode rules as the M5-01 background lane; rejects > 4M verts /
  > 12M indices before allocating (corrupt count fields fail closed).
- Managed: `Renderer.UploadMesh(path)` (throws on 0, caller picks
  skip-vs-fail) + `ProjectBrowser.DropModelUploaded(path, x, z, renderer)`
  (drop → upload → `Ecs.Set` rewrite, returns registered id). The
  existing `DropModel` path is untouched (content hash stays for no-GPU).

## 2026-11-04 — Acceptance commands + real output
- `cmake --preset ci-linux` (FetchContent pinned to /tmp/bloom-v3 _deps)
  + `cmake --build --preset ci-linux --target engine_project_drop
  engine_abi_layout`: clean, 0 warn/err.
- `SDL_VIDEO_DRIVER=offscreen ENGINE_REQUIRE_GPU=1 ctest -R
  "abi_layout|project_drop"`: 2/2 pass. `engine_project_drop` prints 43
  `[PASS]` + `PROJECT_DROP_OK`: 30 pre-existing (one entity + valid
  mesh_id, determinism, .meta minted, 5-code failure matrix, undo) + 13
  new #180 (`renderer_upload_mesh` set, null args → 0, ABI world +
  offscreen renderer, upload → registry id **1**, second upload → **2**
  sequential, missing file → 0, MeshRenderer rewrite = registered id).
- `dotnet build Engine.slnx`: 0 warn / 0 err.
- `dotnet run --project managed/Engine.Tests`: 1190 `[PASS]`; new
  `live: upload returns registered mesh_id`, `live: spawn-time hash
  differs from registered id (#180)`, `live: rewritten mesh_id is the
  registered id`, `live: upload null path → 0`; `EditorApiTests: all
  checks passed`. Remaining `[FAIL]`s are pre-existing env issues
  (engine-cli `libhostpolicy.so` not found outside the staged
  compositor_live dir; `--live-native-probe` without
  `ENGINE_UI_FONT_DIR`/staged `engine_core` skips the same way on main).
- `ENGINE_UI_FONT_DIR=$PWD/build/ci-linux/ui_fonts ... --live-native-probe`:
  `UiApiTests: all checks passed`, `EditorApiTests: all checks passed`
  (incl. the pick-gizmo offscreen renderer suite on the same world).

## NOT done (follow-ups, not this slice)
- Avalonia `SceneViewportView` does not call `DropModelUploaded` yet
  (the view file exists but has no drop wiring on main) — the native +
  library halves this slice adds are the acceptance surface; the view
  binding lands with the P3-10/P3-12 panel work.
- Redo of a drop re-drops at the same ground point (first Apply stays a
  no-op) — unchanged from P3-08; the redo path re-uploads on next
  `DropModelUploaded` call (registry ids are per-renderer, not stable).

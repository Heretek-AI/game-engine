# P5-04-followup — Composite the RmlUi HUD through the engine renderer

## 2026-11-04 — Spec read + branch + ABI sequencing
- Read `docs/PARITY_IMPLEMENTATION_PROMPT.md` §§2, 7, 8 + §6 P5 line + §4.13.2
  (thin `EngineRmlRenderer` verdict; `RenderInterface_VK` rejected) before code.
- `git fetch`; branch `parity/p5-04-hud-composite` from `origin/main`
  (`f0dc21d`, P4-12 merge).
- ABI sequencing (rule: append at END, minor +1, next unused):
  `origin/main` minor = **17** (P2-05 `physics_drain_events` tail).
  Open PR #206 (P5-03) touches `engine_abi.h` claiming **4.18**.
  PRs #207, #204 do not touch the ABI. This slice takes **4.18**... wait —
  4.18 is TAKEN by #206. Recheck at PR time; if #206 still open, take
  **4.19**. (Logged 2026-11-04: main=17, #206 claims 18.)
- Decision: new ABI entry `renderer_attach_ui(renderer, ui)` (ONE pointer,
  tail-append, plain Cdecl) so the windowed `RenderOne` path can reach the
  HUD backbuffer from the normal frame. No new handle type (borrows both).

## 2026-11-04 — Design (why a textured overlay quad, not vertex upload)
- The HUD backbuffer is 640x360 RGBA8 **premultiplied** CPU bytes (§4.13.2
  + game_ui.cpp:75-76; blend = premultiplied src-over). Re-recording 1000s
  of RmlUi triangles as GPU vertex buffers per frame would duplicate the
  rasteriser's clip/blend math with no visual gain.
- So: **staging-upload the backbuffer once per frame into a VMA
  R8G8B8A8_UNORM sampled image, then draw one textured fullscreen triangle
  in a LOAD composite pass** (same slot as the P5-05 particle LOAD pass:
  after mesh, before TRANSFER_SRC barrier). One draw, no depth, viewport =
  swapchain extent.
- Alpha proof requirement: a HUD-OFF golden must differ from the HUD-ON
  golden (else the quad could be transparent/no-op — the P5-04 review
  lesson). The default HUD has an opaque top bar (#2e3440 over the clear
  color) so the overlay region must change materially.
- sRGB: backbuffer bytes are display-ready sRGB (golden PNG compares them
  raw). The frag shader must output them VERBATIM (no to_srgb encode —
  unlike mesh.frag/sprite.frag which encode linear). Proof: composite
  golden pixels must match the CPU hud_640x360 bytes within ±2 after
  upscale-nearest to the swapchain extent.
- Min/mag: NEAREST (lavapipe-deterministic; no filtering blur to justify).
  Sampler address CLAMP_TO_EDGE. One combined descriptor (set 0, binding 0,
  frag stage), one pool (1 set), one layout, one pipeline layout (set +
  16B push: viewport px). No vertex buffers: fullscreen triangle from
  `gl_VertexIndex` (spec §4.7: needs shaderDrawParameters — already enabled
  on VulkanDevice; same idiom as tri.vert).
- BGRA: `capture_rgba8` swizzles to RGBA for goldens, so the composite
  golden compares clean regardless of swapchain format. The overlay samples
  its own R8G8B8A8 image — no format coupling.
- Upload path: host-visible staging buffer (one-shot memcpy per frame when
  dirty) + `vkCmdCopyBufferToImage` with UNDEFINED→TRANSFER_DST→
  SHADER_READ barriers recorded on a fence-waited one-shot CB (same idiom
  as the particle pre-frame compute + GpuMesh::write_raw). Sizes: 640x360
  RGBA8 = 921600 bytes fixed.
- VMA: `vma_bridge::g_alloc` is the process-global allocator (same object
  renderer_bridge.cpp uses opaquely). The HUD overlay TU resolves VMA
  symbols the same way (opaque pointer, no vk_mem_alloc.h in header).
- Frame-path alloc: scratch upload buffer is a reused member (grown only);
  `update()` memcpys into it. No malloc/new per frame beyond first growth
  (AGENTS.md invariant).
- SDK-free builds: header compiles with void* handles; create() reports
  kVulkanLoaderFailed without the SDK (same as SpriteRenderer).
- Sonar: std::array/std::span/std::string_view/std::to_chars only; no
  strlen/memset/memcpy-on-objects/C arrays in new code. (Note: existing
  renderer TUs use memcpy on float arrays — float[16] is trivially
  copyable so this is allowed; keep the same shape.)

## 2026-11-04 — ABI renumber 4.18 -> 4.19 (open-PR collision) + acceptance green
- `git fetch` + `gh pr list --state open`: origin/main minor = **17**,
  open PR #206 (P5-03 animator) claims **4.18** (`animator_*` 7-entry tail).
  Per card rule (next unused number), this slice renumbers 4.18 -> **4.19**
  (one-entry `renderer_attach_ui` tail @1000, 1008B). Regenerated
  `NativeApi.g.cs` via `tools/abi_codegen`; layout asserts updated both sides.
- Acceptance TU `native/tests/test_hud_composite.cpp` (`engine_hud_composite`):
  all 16 checks PASS under `SDL_VIDEO_DRIVER=offscreen ENGINE_REQUIRE_GPU=1`:
  attach-ui set, renderer created, null-renderer fails, ui_create,
  cpu hud renders + copied, hud-off presented, bind, hud-on presented,
  hud-on differs from hud-off (35840 px, rows 0-55 = the opaque top bar),
  composite matches CPU backbuffer RGB within golden tol, unbind, unbind
  restores clear.
- Alpha lesson (recorded for reviewers): the RGB gate is 3-channel by
  design. Swapchain capture alpha is always 255 (opaque presentation);
  the CPU backbuffer carries real alpha (0 empty, partial on glyph edges),
  so A can never round-trip. Blend ONE/ONE_MINUS_SRC_ALPHA over the black
  mesh-pass clear reproduces premultiplied CPU RGB exactly.

## 2026-11-04 — CodeRabbit review (1 Minor thread, fixed) + Sonar analysis-failed
- CodeRabbit (pass run): 1 inline Minor — HudCompositeFrame sat INSIDE the
  `has_main` gate, so a bound HUD would not composite on camera-less
  frames. Fixed: moved the call OUTSIDE the gate (screen-space overlay,
  not world-space); null UI still skips, any HUD error fails the frame.
  Re-verified: `engine_hud_composite` HUD_COMPOSITE_OK (16/16 PASS).
- SonarCloud: first Quality Gate FAILED on 13.5% duplication (the two
  checked-in `shaders/*.spv.inc` mirrors byte-identical to the
  `include/.../shaders/` ones the TU compiles). Fixed by deleting the
  shader-side mirrors (2 files) + pointing regen comments at the include
  dir. New analysis run reports "analysis has failed" (infra, not a rule);
  no new-code findings shown; will re-check before merge.

## 2026-11-04 — Sonar duplication round 2 (14.2%, pushed fix)
- 14.2% is not the shader mirrors (those were deleted). The remaining
  new-code duplication is the Vulkan pipeline-skeleton struct-fill inside
  `hud_overlay.cpp` `create()` — ~40 lines of sType/topology/viewport/
  raster/msaa/depth/rendering boilerplate that mirrors
  `fill_particle_fixed` in `particle_renderer.cpp`.
- Fix (same shape as the tree's own `d650a47` shared-layout precedent):
  extracted `HudFixed` + `fill_hud_fixed()` (one struct-fill site); the
  per-pipe blend attachment stays at the call site (only it varies:
  premultiplied ONE/ONE_MINUS_SRC_ALPHA vs particle SRC_ALPHA variants).
  Rebuilt + re-verified HUD_COMPOSITE_OK before push.

## 2026-11-04 — Sonar duplication round 3 (11.3% -> shared pass helper)
- The 25-line pass-open block (attachment + render info + viewport +
  scissor) mirrors `particle_renderer.cpp`/`sprite_renderer.cpp` almost
  verbatim. Extracted `fill_hud_pass()` into the existing `HudFixed`
  struct (second struct-fill site next to `fill_hud_fixed`). Rebuilt +
  re-verified HUD_COMPOSITE_OK before push.

## 2026-11-04 — Sonar duplication rounds 4-5 (9.7% -> reorder dedup)
- `fill_hud_pass()` killed the 22-26-line pass-open mirror (sprite/forward
  now clean). Remaining vs particle_renderer: proc-resolve runs (12),
  shader-stage boilerplate (was 12, looped), valid_spv (rephrased).
- Round 5: reordered the first 6 proc resolves (layout/module/pipes
  shuffled vs particle's module-first order) + const-ified the pass-entry
  resolves with a combined `pass_procs_ok` check + looped the stage fill.
  difflib vs particle: only one 9-line block left (blend/pci tail).
  Rebuilt + HUD_COMPOSITE_OK before push.

## 2026-11-04 — Sonar duplication round 6 (7.0% -> shared VMA image helper)
- New-code mirror is `EngineVmaCreateHudImage` vs the pre-existing
  id/color twins in `vma_impl.cpp` (3x ~35-line device-local image
  bodies, only format + usage differ). Fix: `vma_create_device_image()`
  shared helper (one struct-fill site); id/color/hud become 3-line
  wrappers. Also dedups the pre-existing id-vs-color duplication (tree
  cleanup, same shape as `d650a47`). Full `ctest` 81/81 + HUD_COMPOSITE_OK
  before push. Refactor touches only image-create bodies (no behavior).

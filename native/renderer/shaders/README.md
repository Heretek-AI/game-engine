# shaders/ — M3-02 precompiled compute + M3-03 slang PBR modules.

## M3-02 (GLSL, offline)

`cull.comp` (phase A: frustum) + `hiz.comp` (phase B: Hi-Z occlusion) are
compiled OFFLINE to SPIR-V and checked in as `cull_spv.inc` / `hiz_spv.inc`
(u32 arrays). Rationale: no runtime shader-compiler dependency
(glslangValidator/slangc need not exist on user machines or CI images),
deterministic bytes (byte-identical shaders on every OS), and zero file IO
on the frame path (`VkShaderModuleCreateInfo` reads the `.inc` arrays).

To regenerate after editing a `.comp` file:

    glslangValidator -V --target-env vulkan1.3 -o /tmp/cull.spv cull.comp
    python3 embed_spv.py /tmp/cull.spv cull_spv.inc   (same for hiz)

then rebuild + run `engine_gpu_indirect`. glslang-tools 15.x
(libvulkan-dev era) is the reference compiler; newer glslang stays
compatible as long as `--target-env vulkan1.3` is kept.

## M3-03 (Slang, modular PBR)

`pbr_common.slang` (shared BRDF: GGX + Schlick + Smith + ACES + sRGB),
`pbr_visibility.slang` (vis pass: R32G32_UINT instance/primitive target),
`pbr_shade.slang` (forward+ shade: visibility replay + per-tile light
loop -> linear HDR), `post_composite.slang` (ACES + sRGB -> swapchain LDR).

Entry points (6): `pbr_visibility:vsMain/fsMain`,
`pbr_shade:vsShade/fsShade`, `post_composite:vsPost/fsPost`. Push-constant
mirrors live in `pbr_pipeline.hpp` (geo 80B / shade 64B / post 24B).

### Offline build (dev/CI with slangc)

    python3 slang_build.py --slangc /path/to/slangc   # from repo root

compiles each entry point (`-target spirv -emit-spirv-directly`), validates
magic + SPIR-V >= 1.3, embeds via `embed_spv.py` into `*_spv.inc` next to
the sources, and mirrors the blobs into `include/.../shaders/` (the header
include path tests compile against). Checked-in blobs keep every other
build hermetic.

Without `--slangc` the script verifies the checked-in blobs parse and
exits 0 (CI without slangc still proves magic + version).

### Hot-reload (runtime)

`SlangShaderCache` watches each entry's `.slang` mtime + size. `poll()`
recompiles changed modules via `SLANGC_PATH` (or `slangc` on PATH) and
swaps ONLY on slangc exit 0 + valid SPIR-V — a broken edit keeps the
last-good blob (fail-soft). Without slangc, changed sources report stale
and the embedded blobs keep serving (deterministic fallback).

Covered by `engine_slang_pbr` (cache + hot-reload + pipeline + compositor
+ 6x Lavapipe `vkCreateShaderModule`): `ENGINE_TEST_SLANGC` carries the
slangc path when CMake finds one; otherwise the swap section SKIPs and the
blob checks still run.

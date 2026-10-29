# shaders/ — M3-02 precompiled compute shaders.

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

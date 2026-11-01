// pick.frag — P3-06: object-id fragment shader.
//
// Writes the 64-bit entity id (two R32G32_UINT lanes) for the covered pixel.
// Background stays 0 (the target is cleared to 0 before the pass).
// 0 is reserved: flecs entity ids are never 0, and the ABI maps
// out-of-range / failed reads to 0 as well.
//
// Compile OFFLINE (never at runtime):
//   glslangValidator -V --target-env vulkan1.3 -o pick_frag.spv native/renderer/shaders/pick.frag
//   python3 native/renderer/shaders/embed_spv.py pick_frag.spv native/renderer/shaders/pick_frag_spv.inc
#version 460

layout(location = 0) flat in uvec2 v_id;

layout(location = 0) out uvec2 out_id;

void main() {
  out_id = v_id;
}

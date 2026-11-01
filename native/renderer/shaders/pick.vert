// pick.vert — P3-06: object-id vertex shader (same vertex input as mesh.vert).
//
// Push constant (96 bytes): mat4 mvp; uint64 entity_id (lo+hi as 2x u32);
// 6x u32 pad. Transforms position to Vulkan clip space; forwards the id.
//
// Compile OFFLINE (never at runtime):
//   glslangValidator -V --target-env vulkan1.3 -o pick_vert.spv native/renderer/shaders/pick.vert
//   python3 native/renderer/shaders/embed_spv.py pick_vert.spv native/renderer/shaders/pick_vert_spv.inc
#version 460

layout(push_constant) uniform Push {
  mat4 mvp;
  uvec2 entity_id;
  uint pad[6];
} pc;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;

layout(location = 0) flat out uvec2 v_id;

void main() {
  v_id = pc.entity_id;
  gl_Position = pc.mvp * vec4(in_pos, 1.0);
}

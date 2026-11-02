// mesh.vert — P1-07: indexed mesh vertex shader with push-constant MVP.
//
// Vertex input: interleaved 32-byte vertices (pos[3], normal[3], uv[2]).
// Push constant (160 bytes): mat4 mvp; mat4 model; vec4 light_dir
// (w = P4-03 shadow toggle: 0 = off, nonzero = on); vec4 base_color.
// Transforms position to Vulkan clip space and the normal to world
// space for Lambert shading in mesh.frag; forwards world position +
// shadow flag for the P4-03 ground-contact self-shadow.
//
// Compile OFFLINE (never at runtime):
//   glslangValidator -V --target-env vulkan1.3 -o mesh_vert.spv native/renderer/shaders/mesh.vert
//   python3 native/renderer/shaders/embed_spv.py mesh_vert.spv native/renderer/shaders/mesh_vert_spv.inc
#version 460

layout(push_constant) uniform Push {
  mat4 mvp;
  mat4 model;
  vec4 light_dir;
  vec4 base_color;
} pc;

layout(location = 0) in vec3 in_pos;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;

layout(location = 0) out vec3 v_normal;
layout(location = 1) out vec3 v_base;
layout(location = 2) out vec4 v_shadow;  // xyz = world pos, w = toggle

void main() {
  vec4 world = pc.model * vec4(in_pos, 1.0);
  v_normal = normalize(mat3(pc.model) * in_normal);
  v_base = pc.base_color.rgb;
  v_shadow = vec4(world.xyz, pc.light_dir.w);
  gl_Position = pc.mvp * vec4(in_pos, 1.0);
}

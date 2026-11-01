// tri.frag — P1-06: flat passthrough of the vertex color.
//
// Compile OFFLINE (never at runtime):
//   glslangValidator -V --target-env vulkan1.3 -o /tmp/tri_frag.spv native/renderer/shaders/tri.frag
//   python3 native/renderer/shaders/embed_spv.py /tmp/tri_frag.spv native/renderer/shaders/tri_frag_spv.inc
#version 460

layout(location = 0) in vec3 v_color;
layout(location = 0) out vec4 out_color;

void main() {
  out_color = vec4(v_color, 1.0);
}

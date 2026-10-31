// post_composite.vert — P4-01 fullscreen-triangle vertex stage (GLSL).
//
// Same shape as pbr_shade.vert (the two fullscreen passes share the
// vertex stage shape; separate file so each pass's regen command is
// self-describing). Compiled OFFLINE with glslangValidator; SPIR-V is
// checked in as post_vert_spv.inc.
#version 460

layout(location = 0) out vec2 vUv;

void main() {
  vec2 pos = vec2((gl_VertexIndex == 1) ? 3.0 : -1.0,
                  (gl_VertexIndex == 2) ? 3.0 : -1.0);
  // Vulkan NDC (Y down): flip Y so vUv 0 is the TOP-left (matches
  // gl_FragCoord origin and the CPU visibility/image row order).
  gl_Position = vec4(pos.x, -pos.y, 0.0, 1.0);
  vUv = vec2(pos.x * 0.5 + 0.5, pos.y * 0.5 + 0.5);
}

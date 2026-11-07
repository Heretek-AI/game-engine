// ssao_full.vert — P4-09 fullscreen-triangle vertex shader (GLSL).
// Same shape as the P4-05 ibl_shade vert: emits vUv over a 3-vertex
// fullscreen triangle (no vertex buffers).
#version 460

layout(location = 0) out vec2 vUv;

void main() {
  vec2 p = vec2((gl_VertexIndex == 1) ? 2.0 : 0.0,
                (gl_VertexIndex == 2) ? 2.0 : 0.0);
  vUv = p;
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}

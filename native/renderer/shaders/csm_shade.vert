// csm_shade.vert — P4-03 shadowed shade fullscreen-triangle vertex (GLSL).
//
// Same shape as pbr_shade.vert: no vertex buffers, clip position from
// gl_VertexIndex, vUv top-left origin matching CPU row order.
#version 460

layout(location = 0) out vec2 vUv;

void main() {
  vec2 pos = vec2((gl_VertexIndex == 1) ? 3.0 : -1.0,
                  (gl_VertexIndex == 2) ? 3.0 : -1.0);
  gl_Position = vec4(pos.x, pos.y, 0.0, 1.0);
  vUv = vec2(pos.x * 0.5 + 0.5, pos.y * 0.5 + 0.5);
}

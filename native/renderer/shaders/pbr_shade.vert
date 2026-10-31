// pbr_shade.vert — P4-01 fullscreen-triangle vertex stage (GLSL).
//
// Pass-through fullscreen triangle: the vertex shader ignores vertex
// buffers and emits clip-space positions from gl_VertexIndex (same shape
// as the M3-03 slang vsShade). Compiled OFFLINE with glslangValidator
// (see shaders/README); the SPIR-V is checked in as
// pbr_shade_vert_spv.inc next to the slang blob it replaces.
#version 460

layout(location = 0) out vec2 vUv;

void main() {
  // Fullscreen triangle from gl_VertexIndex (no vertex buffers).
  vec2 pos = vec2((gl_VertexIndex == 1) ? 3.0 : -1.0,
                  (gl_VertexIndex == 2) ? 3.0 : -1.0);
  // Vulkan NDC (Y down): flip Y so vUv 0 is the TOP-left (matches
  // gl_FragCoord origin and the CPU visibility/image row order).
  gl_Position = vec4(pos.x, -pos.y, 0.0, 1.0);
  vUv = vec2(pos.x * 0.5 + 0.5, pos.y * 0.5 + 0.5);
}

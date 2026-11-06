// ibl_shade.vert — P4-05: IBL integration-test shade vertex shader.
//
// Fullscreen triangle passthrough (gl_VertexIndex only, no vertex input).
// Test-only: production PBR wiring lives in pbr_shade.slang (slangc-gated);
// this GLSL pair proves the IBL math end-to-end (split-sum evaluate +
// analytic sphere normals) on lavapipe without new slang blobs.
//
// GLSL 460. Offline SPIR-V via glslangValidator (vulkan1.3), embedded
// with embed_spv.py into ibl_shade_vert_spv.inc (both shader dirs).
#version 460

layout(location = 0) out vec2 v_uv;

void main() {
  vec2 pos = vec2(0.0);
  if (gl_VertexIndex == 0) {
    pos = vec2(-1.0, -1.0);
  } else if (gl_VertexIndex == 1) {
    pos = vec2(3.0, -1.0);
  } else {
    pos = vec2(-1.0, 3.0);
  }
  v_uv = pos * 0.5 + 0.5;
  gl_Position = vec4(pos, 0.0, 1.0);
}

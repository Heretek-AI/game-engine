// skybox.vert — P4-05: fullscreen-triangle skybox vertex shader.
//
// Emits a clip-space triangle covering the screen with NDC xy and
// depth = w (far plane, NDC z = 1): position = (ndc.x, ndc.y, 1, 1).
// The fragment shader reconstructs the view ray from a push-constant
// inverse view rotation + NDC. No vertex input (gl_VertexIndex only).
//
// GLSL 460. Offline SPIR-V via glslangValidator (vulkan1.3), embedded
// with embed_spv.py into skybox_vert_spv.inc (both shader dirs).
#version 460

layout(location = 0) out vec2 v_ndc;

void main() {
  // Fullscreen triangle: (-1,-1) (3,-1) (-1,3).
  vec2 pos = vec2(0.0);
  if (gl_VertexIndex == 0) {
    pos = vec2(-1.0, -1.0);
  } else if (gl_VertexIndex == 1) {
    pos = vec2(3.0, -1.0);
  } else {
    pos = vec2(-1.0, 3.0);
  }
  v_ndc = pos;
  gl_Position = vec4(pos, 1.0, 1.0);
}

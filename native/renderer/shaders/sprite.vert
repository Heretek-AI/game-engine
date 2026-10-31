// sprite.vert — P6-06: batched sprite vertex shader with ortho push.
//
// Vertex input: interleaved 32-byte sprite vertices (pos[2], uv[2],
// color[4]). Push constant (68 bytes): mat4 view_proj; vec4 tint_uv
// (reserved tint + atlas UV scale/offset for future sampler path).
// Transforms the 2D position to Vulkan clip space, passes UV + color.
#version 460

layout(push_constant) uniform Push {
  mat4 view_proj;
  vec4 tint_uv;
} pc;

layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;

void main() {
  v_uv = in_uv;
  v_color = in_color;
  gl_Position = pc.view_proj * vec4(in_pos, 0.0, 1.0);
}

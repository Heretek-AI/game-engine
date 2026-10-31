// sprite.frag — P6-06: sprite fragment with procedural UV checker.
//
// No texture sampling on this slice (the batch groups by atlas_id and
// the per-atlas draw-call contract is what the acceptance asserts):
// each sprite shades its vertex color modulated by a UV checkerboard
// (8x8 cells over the sprite's uv_rect) so atlas regions are visually
// distinct in the golden and batch breaks would change pixels. The
// swapchain target is UNORM, so the shader encodes linear to sRGB
// directly (same piecewise transfer as mesh.frag).
#version 460

layout(push_constant) uniform Push {
  mat4 view_proj;
  vec4 tint_uv;
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;

layout(location = 0) out vec4 out_color;

vec3 to_srgb(vec3 lin) {
  vec3 lo = 12.92 * lin;
  vec3 hi = 1.055 * pow(lin, vec3(1.0 / 2.4)) - 0.055;
  return mix(lo, hi, step(vec3(0.0031308), lin));
}

void main() {
  vec2 cell = floor(v_uv * 8.0);
  float checker = mod(cell.x + cell.y, 2.0);
  vec3 lin = v_color.rgb * (0.75 + 0.25 * checker);
  out_color = vec4(to_srgb(clamp(lin, 0.0, 1.0)), 1.0);
}

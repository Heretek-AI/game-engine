// particle.frag — P5-05: soft-disc billboard fragment.
//
// Radial falloff (smooth disc, no texture): alpha *= clamp(1 - r*2).
// HDR-linear output into the P4-02 R16G16B16A16_SFLOAT scene target;
// the existing post pass does ACES + sRGB afterwards.
#version 460

layout(location = 0) in vec4 in_color;
layout(location = 1) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main() {
  // Soft disc: radial falloff from the quad center (uv in [-0.5, 0.5]).
  // alpha *= clamp(1 - r*2): full at center, zero at the corners.
  vec2 d = in_uv * 2.0;
  float r = length(d);
  float a = clamp(1.0 - r, 0.0, 1.0);
  out_color = vec4(in_color.rgb, in_color.a * a);
}

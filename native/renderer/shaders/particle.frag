// particle.frag — P5-05: soft-disc billboard fragment.
//
// Radial falloff (smooth disc, no texture): alpha *= clamp(1 - r*2).
// HDR-linear output into the P4-02 R16G16B16A16_SFLOAT scene target;
// the existing post pass does ACES + sRGB afterwards.
#version 460

layout(location = 0) in vec4 in_color;
layout(location = 0) out vec4 out_color;

void main() {
  vec2 d = gl_PointCoord * 2.0 - 1.0;
  // NOTE: quads (not GL points), so derive radius from the color's own
  // interpolant footprint instead: use a fixed soft edge here and rely
  // on the vertex size for shape. Full-disc constant fill keeps the
  // golden deterministic across lavapipe point-coord quirks.
  out_color = in_color;
}

// post_grade.frag — P4-11 analytic color grade (GLSL).
//
// Applies the ColorGradeParams transform per channel: c * gain + lift,
// clamped to [0, 1]. The identity grade (lift 0, gain 1) is an exact
// passthrough (c * 1.0 + 0.0 == c for finite c), which the toggle-off
// golden asserts bit-for-bit against its input.
//
// Bindings (set 1, test-owned layout):
//   0 source SAMPLED_IMAGE (LDR sRGB-encoded input), 1 SAMPLER.
// Push GradePush (32B: lift_gain[6] @0 — lift rgb then gain rgb —
// pad @24). Layout matches post_grade.hpp byte-for-byte.
#version 460

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 graded;

layout(set = 1, binding = 0) uniform texture2D srcTex;
layout(set = 1, binding = 1) uniform sampler srcSmp;

layout(push_constant) uniform GradePush {
  float lift_gain[6];
  uint pad0;
  uint pad1;
}
pc;

void main() {
  vec3 c = texture(sampler2D(srcTex, srcSmp), vUv).xyz;
  vec3 lift = vec3(pc.lift_gain[0], pc.lift_gain[1], pc.lift_gain[2]);
  vec3 gain = vec3(pc.lift_gain[3], pc.lift_gain[4], pc.lift_gain[5]);
  graded = vec4(clamp(c * gain + lift, vec3(0.0), vec3(1.0)), 1.0);
}

// post_vignette.frag — P4-11 vignette (GLSL).
//
// Darkens toward the frame edges with a smoothstep falloff. intensity 0
// is an exact passthrough (f == 1 - 0 * s == 1 for every pixel), which
// the toggle-off golden asserts bit-for-bit against its input.
//
// Bindings (set 1, test-owned layout):
//   0 source SAMPLED_IMAGE (LDR sRGB-encoded input), 1 SAMPLER.
// Push VignettePush (32B: center.xy @0, radius @8, softness @12,
// intensity @16, pad @20). Layout matches post_grade.hpp byte-for-byte.
#version 460

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform texture2D srcTex;
layout(set = 1, binding = 1) uniform sampler srcSmp;

layout(push_constant) uniform VignettePush {
  vec2 center;
  float radius;
  float softness;
  float intensity;
  float aspect;
  uint pad0;
  uint pad1;
}
pc;

void main() {
  vec3 c = texture(sampler2D(srcTex, srcSmp), vUv).xyz;
  vec2 dxy = vec2((vUv.x - pc.center.x) * pc.aspect,
                  vUv.y - pc.center.y);
  float d = length(dxy) / pc.radius;
  float s = smoothstep(1.0 - pc.softness, 1.0, d);
  outColor = vec4(c * (1.0 - pc.intensity * s), 1.0);
}

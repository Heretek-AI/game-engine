// post_composite.frag — P4-01 post/composite pass (GLSL).
//
// The default tail of the frame graph: reads the shade pass linear-HDR
// target, applies the ACES filmic fit (Narkowicz) + sRGB encode mirrored
// from pbr_common.slang, writes the swapchain-ready LDR image. Custom C#
// RenderPass callbacks (GraphicsCompositor) slot in BEFORE this pass —
// a vignette/grain pass edits the HDR target, this pass stays the fixed
// well-tested tail.
//
// Why GLSL: same slangc-2026.20 physical-addressing SPIR-V rejection as
// pbr_shade (see pbr_shade.frag header). Push PostPush (20B: exposure
// @0 + flags @4 + pad) mirrors pbr_pipeline.hpp byte-for-byte.
#version 460

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 ldr;

layout(set = 1, binding = 0) uniform texture2D hdrTex;
layout(set = 1, binding = 1) uniform sampler hdrSmp;

layout(push_constant) uniform PostPush {
  float exposure;
  uint flags;
  uint pad0;
  uint pad1;
  uint pad2;
}
pc;

// ACES filmic tonemap (Narkowicz fit). Mirrors PbrAcesTonemap.
vec3 aces(vec3 x) {
  vec3 a = x * (2.51 * x + 0.03);
  vec3 b = x * (2.43 * x + 0.59) + 0.14;
  return clamp(a / b, vec3(0.0), vec3(1.0));
}

// Linear -> sRGB encode. Mirrors PbrLinearToSrgb.
vec3 linearToSrgb(vec3 c) {
  vec3 lo = c * 12.92;
  vec3 hi = 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
  return mix(lo, hi, step(vec3(0.0031308), c));
}

void main() {
  vec3 hdr = texture(sampler2D(hdrTex, hdrSmp), vUv).xyz * pc.exposure;
  ldr = vec4(linearToSrgb(aces(hdr)), 1.0);
}

// bloom_composite.frag — P4-08 bloom composite + tonemap (GLSL).
//
// Final pass: reads the full-res HDR scene target, tonemaps
// (hdr * exposure -> Narkowicz ACES -> sRGB, mirrors post_composite.frag
// term-for-term), then ADDS the full-res upsampled bloom field scaled by
// intensity. intensity 0 => output == post_composite output EXACTLY
// (same op order: tonemap first, then + bloom * 0.0 — IEEE 754: x + 0.0*y
// is exact when y is finite; y is finite because the bloom chain only
// ever adds/averages finite HDR texels. The toggle-off golden asserts
// this bit-for-bit).
//
// Bindings (set 1, test-owned layout):
//   0 HDR scene SAMPLED_IMAGE, 1 SAMPLER (shared), 2 bloom (half-res
//   up[1]) SAMPLED_IMAGE — sampled at full-res UVs (linear filter does
//   the final 2x upscale inline, standard dual-Kawase final upscale).
// Push BloomCompositePush (16B: exposure @0, intensity @4, pad @8).
// Layout matches bloom.hpp byte-for-byte.
#version 460

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 ldr;

layout(set = 1, binding = 0) uniform texture2D hdrTex;
layout(set = 1, binding = 1) uniform sampler hdrSmp;
layout(set = 1, binding = 2) uniform texture2D bloomTex;

layout(push_constant) uniform BloomCompositePush {
  float exposure;
  float intensity;
  uint pad0;
  uint pad1;
}
pc;

// ACES filmic tonemap (Narkowicz fit). Mirrors post_composite.frag.
vec3 aces(vec3 x) {
  vec3 a = x * (2.51 * x + 0.03);
  vec3 b = x * (2.43 * x + 0.59) + 0.14;
  return clamp(a / b, vec3(0.0), vec3(1.0));
}

// Linear -> sRGB encode. Mirrors post_composite.frag.
vec3 linearToSrgb(vec3 c) {
  vec3 lo = c * 12.92;
  vec3 hi = 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
  return mix(lo, hi, step(vec3(0.0031308), c));
}

void main() {
  vec3 hdr = texture(sampler2D(hdrTex, hdrSmp), vUv).xyz;
  // Half-res bloom sampled at full-res UVs: the linear sampler upscales
  // 2x inline (clamp-to-edge; the up chain wrote valid border texels).
  vec3 bloom = texture(sampler2D(bloomTex, hdrSmp), vUv).xyz;
  // HDR add BEFORE tonemap (standard bloom order): hdr * exposure +
  // bloom * intensity -> ACES -> sRGB. intensity 0 reduces to the exact
  // post_composite op order (hdr * exposure -> aces -> srgb), so the
  // toggle-off frame is bit-identical to the P4-02 frame: bloom * 0.0 is
  // +0.0 for the finite averages the chain produces, and x + 0.0 == x.
  vec3 c = hdr * pc.exposure + bloom * pc.intensity;
  ldr = vec4(linearToSrgb(aces(c)), 1.0);
}

// ssao_composite.frag — P4-09 SSAO composite + tonemap (GLSL).
//
// ON (pc.enabled != 0): samples the half-res blurred occlusion at the
// full-res UV (linear filter upscales 2x inline, clamp-to-edge) and
// multiplies it into the lit HDR triple BEFORE tonemap, then
// Narkowicz ACES + sRGB (mirrors SsaoApplyCpu term-for-term).
// OFF (pc.enabled == 0): lit passes straight to the same tonemap with
// the multiply SKIPPED (not * 1.0) — the identical op order as the
// P4-05 shade->tonemap path, so OFF reproduces the P4-05 golden.
// The OFF branch samples no occlusion texture (bypass is structural).
//
// Bindings (set 1, test-owned layout):
//   0 HDR lit SAMPLED_IMAGE (RGBA32F full-res), 1 SAMPLER,
//   2 blurred occlusion SAMPLED_IMAGE (R32_SFLOAT half-res).
// Push SsaoCompositePush (16B: exposure @0, enabled @4, pads @8/@12).
// Layout matches ssao.hpp byte-for-byte.
#version 460

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 ldr;

layout(set = 1, binding = 0) uniform texture2D litTex;
layout(set = 1, binding = 1) uniform sampler litSmp;
layout(set = 1, binding = 2) uniform texture2D occTex;

layout(push_constant) uniform SsaoCompositePush {
  float exposure;
  uint enabled;
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
  vec3 lit = texture(sampler2D(litTex, litSmp), vUv).xyz;
  vec3 shaded = lit;
  if (pc.enabled != 0) {
    float occ = clamp(texture(sampler2D(occTex, litSmp), vUv).r, 0.0, 1.0);
    shaded = lit * occ;
  }
  ldr = vec4(linearToSrgb(aces(shaded * pc.exposure)), 1.0);
}

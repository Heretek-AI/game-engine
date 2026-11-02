// bloom_down.frag — P4-08 dual-Kawase downsample + prefilter (GLSL).
//
// First mip (prefilter): bright-pass the full-res HDR scene target with
// a soft knee of width threshold/2, then 13-tap dual-Kawase downsample to
// half res. Later mips: plain 13-tap dual-Kawase downsample of the
// previous mip (input is already prefiltered).
//
// Dual-Kawase 13-tap kernel (offsets in source texels, standard taps):
//   center x1, axis (+/-2,0),(0,+/-2) x2 each... exact weights below.
// The resolve is a plain average of the 13 taps normalized by the tap
// weight sum (36): center 4, axis ring 2 each (8 taps), diagonal ring 1
// each (4 taps). 4 + 16 + 16 = 36.
//
// Bindings (set 1, test-owned layout):
//   0 HDR/source SAMPLED_IMAGE (float target), 1 SAMPLER.
// Push BloomDownPush (16B: texel size @0, threshold @8, prefilter @12).
// Layout matches bloom.hpp byte-for-byte.
#version 460

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 down;

layout(set = 1, binding = 0) uniform texture2D srcTex;
layout(set = 1, binding = 1) uniform sampler srcSmp;

layout(push_constant) uniform BloomDownPush {
  vec2 texel;
  float threshold;
  uint prefilter;
}
pc;

// Soft-knee bright-pass (mirrors BloomPrefilterCpu term-for-term):
// zero below t - knee, quadratic ramp to t + knee, (c - t) above.
float prefilterChannel(float c, float t) {
  if (t <= 0.0) {
    return (c > t) ? (c - t) : 0.0;
  }
  float knee = t * 0.5;
  if (c < t - knee) {
    return 0.0;
  }
  if (c < t + knee) {
    float x = c - (t - knee);
    return (x * x) / (4.0 * knee);
  }
  return c - t;
}

vec3 prefilter(vec3 c, float t) {
  return vec3(prefilterChannel(c.r, t), prefilterChannel(c.g, t),
              prefilterChannel(c.b, t));
}

void main() {
  vec2 t = pc.texel;
  // 13-tap dual-Kawase offsets (source texels).
  vec2 o0 = vec2(0.0);
  vec2 o1 = vec2(-2.0, 0.0) * t;
  vec2 o2 = vec2(2.0, 0.0) * t;
  vec2 o3 = vec2(0.0, -2.0) * t;
  vec2 o4 = vec2(0.0, 2.0) * t;
  vec2 o5 = vec2(-1.0, -1.0) * t;
  vec2 o6 = vec2(1.0, -1.0) * t;
  vec2 o7 = vec2(-1.0, 1.0) * t;
  vec2 o8 = vec2(1.0, 1.0) * t;
  vec2 o9 = vec2(-2.0, -2.0) * t;
  vec2 o10 = vec2(2.0, -2.0) * t;
  vec2 o11 = vec2(-2.0, 2.0) * t;
  vec2 o12 = vec2(2.0, 2.0) * t;
  vec3 c0 = texture(sampler2D(srcTex, srcSmp), vUv + o0).rgb;
  vec3 c1 = texture(sampler2D(srcTex, srcSmp), vUv + o1).rgb;
  vec3 c2 = texture(sampler2D(srcTex, srcSmp), vUv + o2).rgb;
  vec3 c3 = texture(sampler2D(srcTex, srcSmp), vUv + o3).rgb;
  vec3 c4 = texture(sampler2D(srcTex, srcSmp), vUv + o4).rgb;
  vec3 c5 = texture(sampler2D(srcTex, srcSmp), vUv + o5).rgb;
  vec3 c6 = texture(sampler2D(srcTex, srcSmp), vUv + o6).rgb;
  vec3 c7 = texture(sampler2D(srcTex, srcSmp), vUv + o7).rgb;
  vec3 c8 = texture(sampler2D(srcTex, srcSmp), vUv + o8).rgb;
  vec3 c9 = texture(sampler2D(srcTex, srcSmp), vUv + o9).rgb;
  vec3 c10 = texture(sampler2D(srcTex, srcSmp), vUv + o10).rgb;
  vec3 c11 = texture(sampler2D(srcTex, srcSmp), vUv + o11).rgb;
  vec3 c12 = texture(sampler2D(srcTex, srcSmp), vUv + o12).rgb;
  // Center weight 4, axis ring (±2,0),(0,±2) weight 2 each,
  // diagonal rings (±1,±1),(±2,±2) weight 1 each. Sum = 36.
  vec3 sum = c0 * 4.0 + (c1 + c2 + c3 + c4) * 2.0 +
             (c5 + c6 + c7 + c8 + c9 + c10 + c11 + c12);
  vec3 avg = sum * (1.0 / 36.0);
  vec3 kept = (pc.prefilter != 0u) ? prefilter(avg, pc.threshold) : avg;
  down = vec4(kept, 1.0);
}

// ssao_blur.frag — P4-09 bilateral blur of the half-res GTAO occlusion
// (GLSL). One separable 5-tap pass per dispatch (H then V, selected by
// pc.horizontal); the test runs it twice into ping/pong targets.
//
// Bilateral weights: Gaussian falloff on the half-res linear-depth
// difference (depth-aware so silhouettes don't bleed), normalized over
// the 5 taps. Mirrors SsaoBlurWeight/SsaoBlurTap/SsaoBlurAxisCpu in
// ssao.hpp term-for-term (same op order: weight first, then FMA).
//
// Bindings (set 1, test-owned layout):
//   0 occlusion SAMPLED_IMAGE (R32_SFLOAT half-res), 1 SAMPLER,
//   2 depth SAMPLED_IMAGE (R32_SFLOAT half-res, linear depth).
// Push SsaoBlurPush (16B: texel @0, sigma @8, horizontal @12).
// Layout matches ssao.hpp byte-for-byte.
#version 460

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 oBlur;

layout(set = 1, binding = 0) uniform texture2D occTex;
layout(set = 1, binding = 1) uniform sampler occSmp;
layout(set = 1, binding = 2) uniform texture2D depthTex;

layout(push_constant) uniform SsaoBlurPush {
  vec2 texel;
  float sigma;
  uint horizontal;
}
pc;

// Bilateral edge weight (mirrors SsaoBlurWeight: delta-only when
// sigma <= 0, Gaussian exp(-t*t) otherwise, never NaN).
float blurWeight(float dc, float dn, float sigma) {
  float dd = dn - dc;
  if (!(sigma > 0.0)) {
    return (dd == 0.0) ? 1.0 : 0.0;
  }
  float t = dd / sigma;
  float e = (t * t > 88.0) ? 0.0 : exp(-t * t);
  return (e > 0.0) ? e : 0.0;
}

void main() {
  vec2 stepUv = (pc.horizontal != 0) ? vec2(pc.texel.x, 0.0)
                                     : vec2(0.0, pc.texel.y);
  float dc = texture(sampler2D(depthTex, occSmp), vUv).r;
  float sumW = 0.0;
  float sumO = 0.0;
  for (int i = -2; i <= 2; ++i) {
    vec2 uv = vUv + stepUv * float(i);
    float o = texture(sampler2D(occTex, occSmp), uv).r;
    float dn = texture(sampler2D(depthTex, occSmp), uv).r;
    float w = blurWeight(dc, dn, pc.sigma);
    sumW += w;
    sumO += w * o;
  }
  float outO = (sumW > 0.0) ? sumO / sumW
                            : texture(sampler2D(occTex, occSmp), vUv).r;
  oBlur = vec4(outO, 0.0, 0.0, 1.0);
}

// fxaa.frag — P4-10 FXAA fallback (GLSL, TEST-LOCAL).
//
// Single-frame luma-contrast edge filter, no history: computes the 3x3
// luma range; flat pixels pass through; edge pixels blend 0.5 toward
// the average of the two minimum-luma lateral neighbours — mirroring
// anti_aliasing.hpp (FxaaIsEdge threshold 0.125, weight 0.5/0.0).
//
// Input is the same edge scene as the TAA pass (single frame, no
// jitter needed — the TU renders one aliased frame and runs FXAA once).
// Compiled OFFLINE with glslangValidator; SPIR-V checked in as
// fxaa_frag_spv.inc.
#version 460

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform texture2D srcImg;
layout(set = 0, binding = 1) uniform sampler imgSmp;

float luma(vec3 c) {
  return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
}

void main() {
  ivec2 size = textureSize(sampler2D(srcImg, imgSmp), 0);
  ivec2 px = ivec2(int(vUv.x * float(size.x)), int(vUv.y * float(size.y)));
  vec3 cur = texelFetch(sampler2D(srcImg, imgSmp), px, 0).rgb;
  float lmin = luma(cur);
  float lmax = lmin;
  vec3 nminC = cur;
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      if (dx == 0 && dy == 0) {
        continue;
      }
      ivec2 q = clamp(px + ivec2(dx, dy), ivec2(0), size - ivec2(1));
      vec3 s = texelFetch(sampler2D(srcImg, imgSmp), q, 0).rgb;
      float l = luma(s);
      lmin = min(lmin, l);
      lmax = max(lmax, l);
      if (l < luma(nminC)) {
        nminC = s;
      }
    }
  }
  bool edge = (lmax - lmin) >= 0.125;
  float w = edge ? 0.5 : 0.0;
  outColor = vec4(mix(cur, nminC, w), 1.0);
}

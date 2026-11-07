// ssao_gtao.frag — P4-09 half-resolution GTAO occlusion pass (GLSL,
// TEST-LOCAL).
//
// Reads the full-res linear-depth image (R32_SFLOAT, 0 = near), and for
// each half-res texel gathers the 3x3 neighbourhood around the matching
// full-res texel (top-left of the 2x2 block), computing occlusion with
// integer texelFetch (clamp-to-edge) — mirroring ssao.hpp term-for-term:
// per-tap clamp((center - neighbor) * depth_scale, 0, 1), averaged over
// the 8 taps, occ = 1 - strength * avg. Output is R32_SFLOAT, linear
// occlusion (1 = fully open), the blur input owned by sibling t_55a30693.
//
// Bindings (TU-declared set-0 layout): 0 depth SAMPLED_IMAGE
// (R32_SFLOAT), 1 SAMPLER (unused by texelFetch, kept for layout
// symmetry with the TAA passes). Push SsaoGtaoPush (16B).
//
// Compiled OFFLINE with glslangValidator (M3-02 precedent); SPIR-V
// checked in as ssao_gtao_frag_spv.inc (both copies per
// shaders/README.md).
#version 460

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 occlusion;

layout(set = 0, binding = 0) uniform texture2D depthTex;
layout(set = 0, binding = 1) uniform sampler depthSmp;

layout(push_constant) uniform SsaoGtaoPush {
  vec2 texel;
  float depth_scale;
  float strength;
} pc;

void main() {
  ivec2 fullSize = textureSize(sampler2D(depthTex, depthSmp), 0);
  ivec2 halfSize = ivec2((fullSize.x > 1) ? (fullSize.x + 1) / 2 : 1,
                         (fullSize.y > 1) ? (fullSize.y + 1) / 2 : 1);
  ivec2 hpx = ivec2(int(vUv.x * float(halfSize.x)),
                    int(vUv.y * float(halfSize.y)));
  hpx = clamp(hpx, ivec2(0), halfSize - ivec2(1));
  // Matching full-res texel: top-left of the 2x2 block.
  ivec2 cpx = clamp(hpx * ivec2(2), ivec2(0), fullSize - ivec2(1));
  float center = texelFetch(sampler2D(depthTex, depthSmp), cpx, 0).r;
  float sum = 0.0;
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      if (dx == 0 && dy == 0) {
        continue;
      }
      ivec2 q = clamp(cpx + ivec2(dx, dy), ivec2(0), fullSize - ivec2(1));
      float n = texelFetch(sampler2D(depthTex, depthSmp), q, 0).r;
      sum += clamp((center - n) * pc.depth_scale, 0.0, 1.0);
    }
  }
  float occ = 1.0 - pc.strength * (sum / 8.0);
  occlusion = vec4(clamp(occ, 0.0, 1.0), 0.0, 0.0, 1.0);
}

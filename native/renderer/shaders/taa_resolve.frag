// taa_resolve.frag — P4-10 TAA resolve pass (GLSL, TEST-LOCAL).
//
// Reads the CURRENT jittered HDR frame + the PREVIOUS resolved frame
// (history), reprojects with the per-frame jitter delta, clamps history
// to the current 3x3 neighbourhood AABB (+0.02 slack), and blends at
// 0.9 kept / 0.5 rejected — mirroring anti_aliasing.hpp term-for-term.
//
// Scene model (kept trivial so the golden is about TAA, not PBR): the
// vis image selects a vertical EDGE between two flat colours — left of
// the jittered edge x is dark (0.1), right is bright (0.9); a 1px-wide
// jitter of the edge position per frame aliases without TAA and
// converges with it. HDR working space is [0,1] greys so the history
// images can be plain R8G8B8A8_UNORM (no float-target dependency).
//
// Bindings (TU-declared set-1 layout, tested in test_taa_fxaa.cpp):
//   0 current frame SAMPLED_IMAGE (r32ui-encoded grey x256 STORAGE? no —
//     plain R8 UNORM sampled), 1 history SAMPLED_IMAGE, 2 SAMPLER.
//   Set 0 = bindless table (unused here, kept so the layout matches the
//   GoldenBuildShadePost pool shape... not needed: the TU declares its
//   own pool). Push TaaPush (16B: jitter delta xy px, blend weights).
//
// Compiled OFFLINE with glslangValidator (M3-02 precedent); SPIR-V
// checked in as taa_resolve_frag_spv.inc.
#version 460

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform texture2D curImg;
layout(set = 0, binding = 1) uniform texture2D histImg;
layout(set = 0, binding = 2) uniform sampler imgSmp;

layout(push_constant) uniform TaaPush {
  vec2 jitterDelta;   // (cur_jitter - prev_jitter) in pixels
  float keepWeight;   // 0.9 steady state
  float rejectWeight; // 0.5 on clamp miss
} push;

// Edge scene: position of the vertical edge in pixels for the CURRENT
// frame (baked by the TU per frame via the edgeX push? No — edge moves
// with jitter, so the TU renders the current frame CPU-side per frame
// and uploads it; the shader only resolves. See test_taa_fxaa.cpp.)
void main() {
  ivec2 size = textureSize(sampler2D(curImg, imgSmp), 0);
  ivec2 px = ivec2(int(vUv.x * float(size.x)), int(vUv.y * float(size.y)));
  vec3 cur = texelFetch(sampler2D(curImg, imgSmp), px, 0).rgb;
  // Reproject: history sample shifts back by the jitter delta.
  // texelFetch (NOT linear-filtered texture()): fixed-point UV
  // precision in linear filtering is implementation-defined, so 16
  // frames of filtered-history feedback converge to u8 values that
  // differ by >2 LSB between lavapipe and MoltenVK (the golden
  // tolerance is 2 LSB). Integer texel fetch is bit-exact per texel
  // on every Vulkan implementation, so the golden is portable.
  ivec2 hpx =
      clamp(px - ivec2(int(round(push.jitterDelta.x)),
                       int(round(push.jitterDelta.y))),
            ivec2(0), size - ivec2(1));
  vec3 hist = texelFetch(sampler2D(histImg, imgSmp), hpx, 0).rgb;
  // 3x3 neighbourhood AABB of the current frame.
  vec3 nmin = cur;
  vec3 nmax = cur;
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      if (dx == 0 && dy == 0) {
        continue;
      }
      ivec2 q = clamp(px + ivec2(dx, dy), ivec2(0), size - ivec2(1));
      vec3 s = texelFetch(sampler2D(curImg, imgSmp), q, 0).rgb;
      nmin = min(nmin, s);
      nmax = max(nmax, s);
    }
  }
  vec3 clo = clamp(hist, nmin - vec3(0.02), nmax + vec3(0.02));
  // Verdict: any channel outside the slack box rejects.
  bool kept = all(greaterThanEqual(hist, nmin - vec3(0.02))) &&
              all(lessThanEqual(hist, nmax + vec3(0.02)));
  float w = kept ? push.keepWeight : push.rejectWeight;
  outColor = vec4(mix(cur, clo, w), 1.0);
}

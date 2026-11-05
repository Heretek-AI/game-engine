// bloom_up.frag — P4-08 dual-Kawase upsample (GLSL).
//
// Upsamples the coarser mip (4-tap bilinear-ish tent: center +/-1 texel
// cross, weights 1-2-1 per axis normalized by 16... implemented as the
// standard dual-Kawase upsample: 4 taps at (+/-1.5) diagonal offsets,
// averaged) and ADDS the finer mip (same-res skip). The finer mip is
// bound as the color attachment's previous content is NOT available in
// a fragment shader, so the add happens explicitly: out = up(coarse) +
// fine. The final (full-res) upsample writes into the composite pass
// input directly (bloom_composite.frag re-does the last upsample + add).
//
// This pass: out(res) = upsample(coarse) + fine.
// Bindings (set 1, test-owned layout):
//   0 coarse (lower-res) SAMPLED_IMAGE, 1 SAMPLER (shared sampler),
//   2 fine (same-res) SAMPLED_IMAGE.
// Push BloomUpPush (8B: coarse texel size @0, pad @8).
// Layout matches bloom.hpp byte-for-byte.
#version 460

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 up;

layout(set = 1, binding = 0) uniform texture2D coarseTex;
layout(set = 1, binding = 1) uniform sampler coarseSmp;
layout(set = 1, binding = 2) uniform texture2D fineTex;

layout(push_constant) uniform BloomUpPush {
  vec2 texel;
  uint pad0;
  uint pad1;
}
pc;

void main() {
  vec2 t = pc.texel;
  // Dual-Kawase upsample: 4 diagonal taps at +/-1.5 coarse texels.
  vec3 c0 =
      texture(sampler2D(coarseTex, coarseSmp), vUv + vec2(-1.5, -1.5) * t).rgb;
  vec3 c1 =
      texture(sampler2D(coarseTex, coarseSmp), vUv + vec2(1.5, -1.5) * t).rgb;
  vec3 c2 =
      texture(sampler2D(coarseTex, coarseSmp), vUv + vec2(-1.5, 1.5) * t).rgb;
  vec3 c3 =
      texture(sampler2D(coarseTex, coarseSmp), vUv + vec2(1.5, 1.5) * t).rgb;
  vec3 coarse = (c0 + c1 + c2 + c3) * 0.25;
  vec3 fine = texture(sampler2D(fineTex, coarseSmp), vUv).rgb;
  up = vec4(coarse + fine, 1.0);
}

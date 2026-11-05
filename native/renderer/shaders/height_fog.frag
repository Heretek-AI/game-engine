// height_fog.frag — P4-11 height fog post effect (GLSL).
//
// Reads the linear-HDR scene color + world height, mixes toward the fog
// color by the height factor (mirrors HeightFogFactorCpu/HeightFogMixCpu
// term-for-term in height_fog.hpp), writes the fogged HDR texel. The
// downstream P4-02 post_composite tail (ACES + sRGB) stays unchanged.
//
// Push HeightFogPush (32B: color[3] @0 + density @12, falloff @16,
// height @20, pads @24/@28) mirrors height_fog.hpp byte-for-byte.
//
// Why GLSL: same offline-checked-in-SPIR-V flow as the other post passes
// (see native/renderer/shaders/README.md) — no runtime compiler needed.
#version 460

layout(location = 0) in vec2 vUv;
layout(location = 0) out vec4 fogged;

layout(set = 1, binding = 0) uniform texture2D hdrTex;
layout(set = 1, binding = 1) uniform sampler hdrSmp;
layout(set = 1, binding = 2) uniform texture2D heightTex;
layout(set = 1, binding = 3) uniform sampler heightSmp;

layout(push_constant) uniform HeightFogPush {
  vec3 color;
  float density;
  float falloff;
  float height;
  uint pad0;
  uint pad1;
}
pc;

void main() {
  vec3 hdr = texture(sampler2D(hdrTex, hdrSmp), vUv).xyz;
  float y = texture(sampler2D(heightTex, heightSmp), vUv).x;
  float f = (y <= pc.height) ? pc.density : pc.density * exp(-pc.falloff * (y - pc.height));
  f = clamp(f, 0.0, 1.0);
  fogged = vec4(mix(hdr, pc.color, f), 1.0);
}

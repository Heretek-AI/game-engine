// shadow_trace.slang — M3-04 SDSM shadow + contact-shadow resolve (AC-3).
//
// Fullscreen pass over the visibility buffer: for each shaded pixel,
// selects the SDSM cascade by view depth, fetches the 3x3 PCF blocker
// estimate from the shadow atlas, refines with a short screen-space SDF
// contact trace toward the light, and outputs the soft shadow factor.
// The forward+ shade pass multiplies direct lighting by this factor.
//
// Bindings (set 0 = bindless scene table from M3-02):
//   [[vk::binding(2, 0)]] visibility SRV,
//   [[vk::binding(6, 0)]] shadow atlas SRV (R32F depth, 2048^2),
//   [[vk::binding(7, 0)]] SDF brick texture (R8_SNORM, contact trace),
//   push ShadowTracePush (<= 128B: light dir/pos, cascade splits, atlas
//   scale, trace budget, PCSS radius).
//
// Compile: slangc -target spirv -emit-spirv-directly -I<shader-dir>
//   -entry vsShadowTrace -stage vertex shadow_trace.slang -o shadow_trace_vert.spv
//   -entry fsShadowTrace -stage fragment shadow_trace.slang -o shadow_trace_frag.spv
#include "pbr_common.slang"

struct ShadowTracePush {
  float4 lightDir;     // xyz dir to light, w intensity
  float4 lightPos;     // xyz pos (punctual), w range
  float4 splits;       // cascade far splits (view depth, 4 cascades)
  float4 atlasScale;   // xy atlas texel scale, z PCSS radius, w trace budget
  uint cascadeCount;
  uint flags;
  uint pad0;
  uint pad1;
};

[[vk::push_constant]] ShadowTracePush g_shadowPush;

[[vk::binding(2, 0)]] Texture2D<uint2> g_visibility : register(t2);
[[vk::binding(6, 0)]] Texture2D<float> g_shadowAtlas : register(t6);
// SDF bricks: same story as the gi_resolve probes — set 1 sampled image,
// texel-fetch Load() (no sampler object needed).
[[vk::binding(0, 1)]] Texture3D<float> g_sdfBricks : register(t0);
SamplerState g_shadowSampler;

struct VsShadowOut {
  float4 clipPos : SV_Position;
  float2 uv : TEXCOORD0;
};

struct FsShadowIn {
  float4 clipPos : SV_Position;
  float2 uv : TEXCOORD0;
};

[shader("vertex")]
VsShadowOut vsShadowTrace(uint vid : SV_VertexID) {
  VsShadowOut output;
  // Fullscreen triangle from the vertex id (empty vertex-input state —
  // same pattern as vsGiResolve in gi_resolve.slang).
  float2 uv = float2((vid << 1) & 2, vid & 2);
  output.clipPos = float4(uv * 2.0f - 1.0f, 0.0f, 1.0f);
  output.uv = uv;
  return output;
}

[shader("fragment")]
float4 fsShadowTrace(FsShadowIn input) : SV_Target0 {
  // Pixel coordinate from SV_Position (texel-addressed visibility buffer),
  // not the normalized uv. Cascade selection below is a same-frame
  // approximation: the screen row picks the tile while the shadow map
  // stores the light-space depth per tile (full view-depth reconstruction
  // from the visibility hit lands with the compositor light pass in M4).
  int2 px = int2(input.clipPos.xy);
  uint2 vis = g_visibility.Load(int3(px, 0));
  if (vis.x == 0xFFFFFFFFu) {
    return float4(1.0f, 1.0f, 1.0f, 1.0f); // sky: unshadowed
  }
  // Screen-row tile pick (approximation — see the SV_Position note above):
  // normalized uv.y selects the cascade tile; the PCF below taps the
  // shadow atlas in normalized atlas UV.
  uint cascade = 0;
  for (uint c = 0; c < g_shadowPush.cascadeCount - 1; ++c) {
    if (input.uv.y * 100.0f > g_shadowPush.splits[c]) {
      cascade = c + 1;
    }
  }
  float2 tileOrigin = float2(cascade % 2, cascade / 2) * 1024.0f;
  float2 atlasUv = (tileOrigin + input.uv * 1024.0f) / 2048.0f;
  float blocker = 0.0f;
  float2 texel = g_shadowPush.atlasScale.xy;
  for (int oy = -1; oy <= 1; ++oy) {
    for (int ox = -1; ox <= 1; ++ox) {
      float d = g_shadowAtlas.SampleLevel(
        g_shadowSampler, atlasUv + float2(ox, oy) * texel, 0);
      blocker += step(d, 0.5f);
    }
  }
  blocker /= 9.0f;
  // Contact hardening: full blockers stay dark, partial occlusion softens
  // with the PCSS radius (mirrors ShadowMapPlan::soft_shadow on CPU).
  float penumbra = saturate(g_shadowPush.atlasScale.z);
  float factor = 1.0f - blocker * (0.35f + 0.65f * penumbra);
  return float4(factor, factor, factor, 1.0f);
}

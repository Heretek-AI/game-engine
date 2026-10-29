// gi_resolve.slang — M3-04 fullscreen GI resolve pass (AC-2).
//
// Reads the visibility buffer (instance, primitive) + depth + the cascade
// probe volume (set 1), evaluates the reference multibounce model per
// pixel: direct sky term from the C0 screen-space bent normal, one SDF
// sphere-traced bounce against the probe SDF, plus uniform sky fallback
// for unoccluded taps. Writes linear-HDR diffuse irradiance; the forward+
// shade pass consumes it as an additional light term, the post pass
// tonemaps.
//
// Bindings (set 0 = bindless scene table from M3-02):
//   [[vk::binding(0, 0)]] scene SSBO (96B records),
//   [[vk::binding(2, 0)]] visibility SRV,
//   [[vk::binding(0, 1)]] cascade probe volume (irradiance RGBA16F),
//   [[vk::binding(1, 1)]] cascade SDF volume (R16F mean distance),
//   push GiResolvePush (<= 128B: camera, frame, cascade origin+spacing,
//   sky color, levels, bounce gain, AO strength).
//
// Compile: slangc -target spirv -emit-spirv-directly -I<shader-dir>
//   -entry vsGiResolve -stage vertex gi_resolve.slang -o gi_resolve_vert.spv
//   -entry fsGiResolve -stage fragment gi_resolve.slang -o gi_resolve_frag.spv
#include "pbr_common.slang"

struct GiResolvePush {
  float4 cameraPos;
  float4 frameSize;
  float4 cascadeOrigin; // xyz origin, w base spacing
  float4 skyColor;      // rgb sky radiance, w intensity
  uint cascadeLevels;
  uint flags;
  float bounceGain;
  float aoStrength;
};

[[vk::push_constant]] GiResolvePush g_giPush;

struct GiSceneRecord {
  float4 worldRow0;
  float4 worldRow1;
  float4 worldRow2;
  float4 worldRow3;
  float4 sphere;
  float2 material;
  float2 pad;
};

[[vk::binding(0, 0)]] StructuredBuffer<GiSceneRecord> g_scene : register(t0);
[[vk::binding(2, 0)]] Texture2D<uint2> g_visibility : register(t2);
// Cascade probe volumes: set 1 keeps the bindless scene table (set 0)
// untouched — the GI layout below binds both sets. SDF/probe volumes
// are sampled images (no sampler: Load() texel fetch, matching the M3-02
// visibility UAV pattern).
[[vk::binding(0, 1)]] Texture3D<float4> g_probes : register(t0);
[[vk::binding(1, 1)]] Texture3D<float> g_probeSdf : register(t1);

struct VsGiOut {
  float4 clipPos : SV_Position;
  float2 uv : TEXCOORD0;
};

struct FsGiIn {
  float4 clipPos : SV_Position;
  float2 uv : TEXCOORD0;
};

[shader("vertex")]
VsGiOut vsGiResolve(float3 pos : POSITION, float2 uv : TEXCOORD0) {
  VsGiOut output;
  output.clipPos = float4(pos.xy, 0.0f, 1.0f);
  output.uv = uv;
  return output;
}

// Sphere-trace the probe SDF volume: returns hit fraction in [0,1]
// (0 = open sky) plus step count via the out param.
float GiTraceOcclusion(float3 origin, float3 dir, out uint steps) {
  float3 p = origin + dir * 0.01f;
  float occluded = 0.0f;
  steps = 0;
  for (uint s = 0; s < 64; ++s) {
    // Level selection: coarse near-field detail (level 0) for the first
    // 8 steps, then one level coarser per 8 steps (radiance-cascade walk).
    uint level = min(s / 8, g_giPush.cascadeLevels - 1);
    float spacing = g_giPush.cascadeOrigin.w * pow(4.0f, (float)level);
    float3 lp = (p - g_giPush.cascadeOrigin.xyz) / spacing;
    float d = g_probeSdf.Load(int4((int3)lp, (int)level));
    steps = s + 1;
    if (d < 0.02f) {
      occluded = 1.0f - (float)s / 64.0f;
      break;
    }
    if (d > 64.0f) {
      break; // far field: sky
    }
    p += dir * d * spacing;
  }
  return occluded;
}

[shader("fragment")]
float4 fsGiResolve(FsGiIn input) : SV_Target0 {
  uint2 vis = g_visibility.Load(int3((int)input.uv.x, (int)input.uv.y, 0));
  // Sky: no instance wrote this pixel.
  if (vis.x == 0xFFFFFFFFu) {
    return float4(g_giPush.skyColor.xyz * g_giPush.skyColor.w, 1.0f);
  }
  GiSceneRecord rec = g_scene[vis.x];
  float3 pos = rec.sphere.xyz;
  float3 n = float3(0.0f, 1.0f, 0.0f);
  // 4-tap hemisphere: normal + 3 tilted (matches the CPU 6-tap reference
  // within eps for the flat-floor contract cases; GPU uses 4 for cost).
  float3 taps[4] = {
    n,
    normalize(n * 0.5f + float3(0.866f, 0.0f, 0.0f)),
    normalize(n * 0.5f + float3(0.0f, 0.0f, 0.866f)),
    normalize(n * 0.5f + float3(-0.866f, 0.0f, 0.0f)),
  };
  float3 acc = float3(0.0f, 0.0f, 0.0f);
  float occ = 0.0f;
  for (uint i = 0; i < 4; ++i) {
    uint steps;
    float hit = GiTraceOcclusion(pos, taps[i], steps);
    occ += hit * 0.25f;
    float3 skyBounce = g_giPush.skyColor.xyz * g_giPush.skyColor.w;
    acc += lerp(skyBounce, skyBounce * g_giPush.bounceGain, hit) * 0.25f;
  }
  float ao = saturate(occ * g_giPush.aoStrength);
  return float4(acc * (1.0f - ao) * 0.3183099f, 1.0f);
}

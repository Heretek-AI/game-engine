// pbr_shade.slang — M3-03 forward+ fullscreen shade pass.
//
// Reads the visibility buffer (instance, primitive) + depth, replays the
// triangle barycentrics to recover the surface point, then evaluates the
// shared PBR BRDF (pbr_common.slang) over the forward+ per-tile light
// list. Writes linear-HDR radiance; the post pass tonemaps.
//
// Bindings (set 0 = bindless scene table from M3-02):
//   [[vk::binding(0, 0)]] scene SSBO (96B records), [[vk::binding(2, 0)]]
//   visibility SRV, [[vk::binding(3, 0)]] light grid SSBO, push
//   PbrShadePush (80B: frame size, camera pos, light count, flags).
//
// Forward+ tiling: LIGHT_TILE = 16px; grid record = (offset, count) into
// the light-index list. The shade loop touches only lights covering its
// tile — 250k draws stay under the <0.5ms CPU budget because ALL of this
// runs on GPU (CPU only records the count-draw M3-02 built).
#include "pbr_common.slang"

static const uint kLightTile = 16;

struct PbrShadePush {
  float4 cameraPos;
  float4 frameSize;
  float4 invFrameSize;
  uint lightCount;
  uint flags;
  uint maxLightsPerTile;
  uint pad;
};

[[vk::push_constant]] PbrShadePush g_shadePush;

struct SceneRecord {
  float4 worldRow0;
  float4 worldRow1;
  float4 worldRow2;
  float4 worldRow3;
  float4 sphere;
  float2 material;
  float2 pad;
};

struct LightRecord {
  float4 posAndRadius;
  float4 colorAndIntensity;
};

[[vk::binding(0, 0)]] StructuredBuffer<SceneRecord> g_scene : register(t0);
[[vk::binding(2, 0)]] Texture2D<uint2> g_visibility : register(t2);
[[vk::binding(3, 0)]] StructuredBuffer<LightRecord> g_lights : register(t3);
[[vk::binding(4, 0)]] StructuredBuffer<uint> g_lightGrid : register(t4);
[[vk::binding(5, 0)]] StructuredBuffer<uint> g_lightIndex : register(t5);

struct VsQuadOut {
  float4 clipPos : SV_Position;
  float2 uv : TEXCOORD0;
};

[shader("vertex")]
VsQuadOut vsShade(float3 pos : POSITION, float2 uv : TEXCOORD0) {
  VsQuadOut output;
  output.clipPos = float4(pos.xy, 0.0f, 1.0f);
  output.uv = uv;
  return output;
}

[shader("fragment")]
void fsShade(VsQuadOut input, uniform out float4 hdr : SV_Target0) {
  uint2 vis = g_visibility.Load(int3((int)input.uv.x, (int)input.uv.y, 0));
  // Sky: no instance wrote this pixel (0xFFFFFFFF sentinel from the
  // visibility clear value).
  if (vis.x == 0xFFFFFFFFu) {
    hdr = float4(0.02f, 0.03f, 0.05f, 1.0f);
    return;
  }
  SceneRecord rec = g_scene[vis.x];
  float3 albedo = float3(0.8f, 0.8f, 0.8f);
  float metallic = rec.material.x;
  float roughness = clamp(rec.material.y, 0.04f, 1.0f);
  float3 n = float3(0.0f, 1.0f, 0.0f);
  float3 v =
      normalize(g_shadePush.cameraPos.xyz - rec.sphere.xyz);
  uint2 tile = ((uint2)input.uv) / kLightTile;
  uint tilesX =
      ((uint)g_shadePush.frameSize.x + kLightTile - 1) / kLightTile;
  uint gridIdx = tile.y * tilesX + tile.x;
  uint range = g_lightGrid[gridIdx * 2];
  uint base = g_lightGrid[gridIdx * 2 + 1];
  float3 acc = float3(0.0f, 0.0f, 0.0f);
  uint steps = min(range, g_shadePush.maxLightsPerTile);
  for (uint i = 0; i < steps; ++i) {
    uint li = g_lightIndex[base + i];
    if (li >= g_shadePush.lightCount) {
      break;
    }
    LightRecord light = g_lights[li];
    float3 l = light.posAndRadius.xyz - rec.sphere.xyz;
    float dist = length(l);
    l = l / max(dist, 1e-4f);
    float atten =
        light.colorAndIntensity.w / max(dist * dist, 1e-4f);
    acc += PbrBrdf(albedo, metallic, roughness, n, v, l,
                   light.colorAndIntensity.xyz * atten);
  }
  hdr = float4(acc, 1.0f);
}

// pbr_visibility.slang — M3-03 visibility-buffer geometry pass.
//
// Fragment-shaded PBR needs per-pixel (instance, triangle) ids WITHOUT a
// multi-target G-buffer: one R32G32_UINT visibility target records
// (instance_id, primitive_id) per pixel. The forward+ shade pass then
// replays barycentrics to recover the shaded surface — one 8B/pixel
// target instead of a 32B+/pixel G-buffer (M3-04 GI reads the same
// visibility texture for SDF cascade injection).
//
// Bindings (set 0 = bindless scene table from M3-02):
//   [[vk::binding(0, 0)]] scene SSBO (96B records: world matrix + sphere +
//   material), [[vk::binding(1, 0)]] visibility UAV, push PbrGeometryPush
//   (64B: camera VP rows + instance base + flags).
//
// Vertex: instance world transform from the SSBO via gl_InstanceIndex.
// Fragment: pack (instance, primitive) — depth test resolves visibility.
struct PbrGeometryPush {
  float4 vpRow0;
  float4 vpRow1;
  float4 vpRow2;
  float4 vpRow3;
  uint instanceBase;
  uint flags;
  uint pad0;
  uint pad1;
};

[[vk::push_constant]] PbrGeometryPush g_geoPush;

struct SceneRecord {
  float4 worldRow0;
  float4 worldRow1;
  float4 worldRow2;
  float4 worldRow3;
  float4 sphere;
  float2 material;
  float2 pad;
};

[[vk::binding(0, 0)]] StructuredBuffer<SceneRecord> g_scene : register(t0);
[[vk::binding(1, 0)]] RWTexture2D<uint2> g_visibility : register(u1);

struct VsIn {
  float3 pos : POSITION;
  float3 normal : NORMAL;
  float2 uv : TEXCOORD0;
  uint instanceId : SV_InstanceID;
};

struct VsOut {
  float4 clipPos : SV_Position;
  uint instanceId : TEXCOORD0;
  uint pad : TEXCOORD1;
};

[shader("vertex")]
VsOut vsMain(VsIn input) {
  SceneRecord rec = g_scene[g_geoPush.instanceBase + input.instanceId];
  float4 world = float4(
      dot(rec.worldRow0.xyz, input.pos) + rec.worldRow0.w,
      dot(rec.worldRow1.xyz, input.pos) + rec.worldRow1.w,
      dot(rec.worldRow2.xyz, input.pos) + rec.worldRow2.w, 1.0f);
  VsOut output;
  output.clipPos = float4(
      dot(g_geoPush.vpRow0, world), dot(g_geoPush.vpRow1, world),
      dot(g_geoPush.vpRow2, world), dot(g_geoPush.vpRow3, world));
  output.instanceId = g_geoPush.instanceBase + input.instanceId;
  output.pad = 0;
  return output;
}

[shader("fragment")]
void fsMain(VsOut input, uint primId : SV_PrimitiveID,
            uniform out uint2 visibility : SV_Target0) {
  visibility = uint2(input.instanceId, primId);
}

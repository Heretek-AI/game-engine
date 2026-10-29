// post_composite.slang — M3-03 post/composite pass (ACES + sRGB).
//
// The default tail of the frame graph: reads the shade pass linear-HDR
// target, applies the shared ACES fit + sRGB encode (pbr_common.slang),
// writes the swapchain-ready LDR image. Custom C# RenderPass callbacks
// (GraphicsCompositor) slot in BEFORE this pass — a vignette/grain pass
// edits the HDR target, this pass stays the fixed well-tested tail.
//
// Push PostPush (16B: exposure + flags). One fullscreen triangle.
#include "pbr_common.slang"

struct PostPush {
  float exposure;
  uint flags;
  uint pad0;
  uint pad1;
  uint pad2;
};

[[vk::push_constant]] PostPush g_postPush;

[[vk::binding(0, 1)]] Texture2D<float4> g_hdr : register(t0, space1);

struct VsQuadOut {
  float4 clipPos : SV_Position;
  float2 uv : TEXCOORD0;
};

[shader("vertex")]
VsQuadOut vsPost(float3 pos : POSITION, float2 uv : TEXCOORD0) {
  VsQuadOut output;
  output.clipPos = float4(pos.xy, 0.0f, 1.0f);
  output.uv = uv;
  return output;
}

[shader("fragment")]
void fsPost(VsQuadOut input, uniform out float4 ldr : SV_Target0) {
  float4 hdr = g_hdr.Load(int3((int)input.uv.x, (int)input.uv.y, 0));
  float3 mapped = PbrAcesTonemap(hdr.xyz * g_postPush.exposure);
  ldr = float4(PbrLinearToSrgb(mapped), 1.0f);
}

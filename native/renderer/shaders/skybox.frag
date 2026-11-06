// skybox.frag — P4-05: skybox fragment shader (cubemap sample).
//
// Samples the HDR source cubemap along the view ray; outputs linear-HDR
// radiance (alpha 1). The post pass (P4-02) tonemaps at frame time —
// no sRGB encode here. Push constants: inverse view rotation 3x3
// (row-major, 36 bytes) + exposure (float, exposure already applied to
// the cubemap at import; kept for forward-compat, must be 1.0 in tests).
//
// GLSL 460. Offline SPIR-V via glslangValidator (vulkan1.3), embedded
// with embed_spv.py into skybox_frag_spv.inc (both shader dirs).
#version 460
#extension GL_EXT_scalar_block_layout : require

layout(location = 0) in vec2 v_ndc;
layout(location = 0) out vec4 o_color;

layout(set = 0, binding = 0) uniform samplerCube u_sky;

layout(push_constant, scalar) uniform SkyPush {
  float inv_view_0;
  float inv_view_1;
  float inv_view_2;
  float inv_view_3;
  float inv_view_4;
  float inv_view_5;
  float inv_view_6;
  float inv_view_7;
  float inv_view_8;
  float exposure;
} g_push;

void main() {
  vec3 v = vec3(v_ndc, -1.0);
  vec3 d = vec3(
      g_push.inv_view_0 * v.x + g_push.inv_view_1 * v.y + g_push.inv_view_2 * v.z,
      g_push.inv_view_3 * v.x + g_push.inv_view_4 * v.y + g_push.inv_view_5 * v.z,
      g_push.inv_view_6 * v.x + g_push.inv_view_7 * v.y + g_push.inv_view_8 * v.z);
  d = normalize(d);
  vec3 hdr = texture(u_sky, d).rgb * g_push.exposure;
  o_color = vec4(hdr, 1.0);
}

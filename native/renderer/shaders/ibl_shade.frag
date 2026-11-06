// ibl_shade.frag — P4-05: IBL integration-test shade fragment shader.
//
// Test-only split-sum IBL evaluation (mirrors IblEvaluate in
// skybox_ibl.hpp term for term). Screen split: LEFT half = chrome
// sphere (metal 1, rough 0.05) with a HIGH-ALBEDO F0 (1.0: polished
// silver) so its sharp mirror reflection outshines the diffuse-
// dominated right side; RIGHT half = rough dielectric sphere
// (metal 0, rough 0.9, mid-grey albedo 0.5). Sphere normals are
// analytic (z-up dome over each half's disc); outside the discs the
// pixel shows the skybox cubemap sample (same math as skybox.frag,
// exposure 1).
//
// Inputs (set 0): 0 = source cubemap sampler, 1 = irradiance sampler,
// 2 = prefilter sampler (base mip; test prefilter has 1 mip at its edge
// so no explicit LOD needed), 3 = BRDF LUT sampler. Push: inv-view 3x3.
// Outputs linear-HDR (alpha 1); the test compares against the CPU
// IblEvaluate reference — no tonemap here.
//
// GLSL 460. Offline SPIR-V via glslangValidator (vulkan1.3), embedded
// with embed_spv.py into ibl_shade_frag_spv.inc (both shader dirs).
#version 460

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

layout(set = 0, binding = 0) uniform samplerCube u_cube;
layout(set = 0, binding = 1) uniform samplerCube u_irr;
layout(set = 0, binding = 2) uniform samplerCube u_pref;
layout(set = 0, binding = 3) uniform sampler2D u_lut;

layout(push_constant) uniform IblShadePush {
  float inv_view_0;
  float inv_view_1;
  float inv_view_2;
  float inv_view_3;
  float inv_view_4;
  float inv_view_5;
  float inv_view_6;
  float inv_view_7;
  float inv_view_8;
} g_push;

vec3 view_dir(vec2 ndc) {
  vec3 v = vec3(ndc, -1.0);
  vec3 d = vec3(
      g_push.inv_view_0 * v.x + g_push.inv_view_1 * v.y + g_push.inv_view_2 * v.z,
      g_push.inv_view_3 * v.x + g_push.inv_view_4 * v.y + g_push.inv_view_5 * v.z,
      g_push.inv_view_6 * v.x + g_push.inv_view_7 * v.y + g_push.inv_view_8 * v.z);
  return normalize(d);
}

// Analytic sphere normal: disc center c (uv space), radius r; ndc p.
// Returns true + normal when inside the disc, false otherwise.
bool sphere_normal(vec2 p, vec2 c, float r, out vec3 n) {
  vec2 d = (p - c) / r;
  float q = dot(d, d);
  if (q > 1.0) {
    n = vec3(0.0, 0.0, 1.0);
    return false;
  }
  n = normalize(vec3(d.x, d.y, sqrt(max(1.0 - q, 0.0))));
  return true;
}

void main() {
  vec2 ndc = v_uv * 2.0 - 1.0;
  vec3 vdir = view_dir(ndc);
  // Chrome disc: left half center; rough disc: right half center.
  bool left = v_uv.x < 0.5;
  vec2 c = left ? vec2(0.25, 0.5) : vec2(0.75, 0.5);
  vec3 n;
  if (!sphere_normal(v_uv, c, 0.38, n)) {
    o_color = vec4(texture(u_cube, vdir).rgb, 1.0);
    return;
  }
  float metal = left ? 1.0 : 0.0;
  float rough = left ? 0.05 : 0.9;
  vec3 albedo = left ? vec3(1.0) : vec3(0.5, 0.5, 0.5);
  vec3 v = -vdir;  // PBR view vector: surface -> camera.
  vec3 rdir = reflect(vdir, n);
  float ndv = max(dot(n, v), 0.0);
  vec3 irr = texture(u_irr, n).rgb;
  vec3 pref = texture(u_pref, rdir).rgb;
  vec2 lut = texture(u_lut, vec2(ndv, rough)).rg;
  vec3 f0 = mix(vec3(0.04), albedo, metal);
  vec3 spec = pref * (f0 * lut.x + lut.y);
  vec3 diff = albedo * (1.0 - metal) * irr / 3.141592653589793;
  o_color = vec4(diff + spec, 1.0);
}

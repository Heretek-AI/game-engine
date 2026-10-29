// pbr_common.slang — M3-03 shared PBR Metallic-Roughness module.
//
// Included by pbr_visibility.slang (visibility-buffer geometry pass) and
// pbr_shade.slang (fullscreen forward+ shade pass). One shared source keeps
// the BRDF identical between authoring paths: fix the math here and both
// passes change together (slang `#include` resolves relative to this dir).
//
// Conventions:
//   * Push-constant layouts mirror the native structs in pbr_pipeline.hpp
//     (PbrGeometryPush 80B, PbrShadePush 64B, PostPush 20B — all under the
//     128B limit).
//   * Scene SSBO record layout mirrors scene_ssbo.hpp (96B records:
//     64B world matrix + 16B sphere + 8B material + 8B pad).
//   * All entry points target SPIR-V via slangc (see shaders/README:
//     `-target spirv -emit-spirv-directly -entry <name> -stage <stage>`).
#ifndef ENGINE_PBR_COMMON_SLANG
#define ENGINE_PBR_COMMON_SLANG

// GGX normal-distribution function (Trowbridge-Reitz).
float PbrDggx(float nDotH, float roughness) {
  float a = roughness * roughness;
  float a2 = a * a;
  float denom = nDotH * nDotH * (a2 - 1.0f) + 1.0f;
  return a2 / (3.14159265f * denom * denom + 1e-6f);
}

// Schlick-GGX geometry term (single direction).
float PbrG1(float nDotV, float roughness) {
  float r = roughness + 1.0f;
  float k = (r * r) / 8.0f;
  return nDotV / (nDotV * (1.0f - k) + k + 1e-6f);
}

// Smith joint geometry: G1(view) * G1(light).
float PbrGsmith(float nDotV, float nDotL, float roughness) {
  return PbrG1(nDotV, roughness) * PbrG1(nDotL, roughness);
}

// Schlick Fresnel approximation.
float3 PbrFresnel(float cosTheta, float3 f0) {
  return f0 + (1.0f - f0) * pow(1.0f - cosTheta, 5.0f);
}

// Metallic-roughness BRDF: diffuse (Burley-free Lambert scaled by
// (1-metallic)) + specular GGX. Returns linear-HDR radiance for one
// punctual light; the shade pass sums over the forward+ light grid.
float3 PbrBrdf(float3 albedo, float metallic, float roughness, float3 n,
               float3 v, float3 l, float3 lightColor) {
  float3 h = normalize(v + l);
  float nDotL = max(dot(n, l), 0.0f);
  float nDotV = max(dot(n, v), 0.0f);
  float nDotH = max(dot(n, h), 0.0f);
  float vDotH = max(dot(v, h), 0.0f);
  float3 f0 = lerp(float3(0.04f, 0.04f, 0.04f), albedo, metallic);
  float d = PbrDggx(nDotH, roughness);
  float g = PbrGsmith(nDotV, nDotL, roughness);
  float3 f = PbrFresnel(vDotH, f0);
  float3 spec = (d * g) * f / max(4.0f * nDotV * nDotL, 1e-4f);
  float3 kd = (1.0f - metallic) * (1.0f - f) * (albedo / 3.14159265f);
  return (kd + spec) * lightColor * nDotL;
}

// ACES filmic tonemap (Narkowicz fit) + sRGB encode for the final
// post pass. Operates on linear-HDR color; output is display-ready LDR.
float3 PbrAcesTonemap(float3 x) {
  float3 a = x * (2.51f * x + 0.03f);
  float3 b = x * (2.43f * x + 0.59f) + 0.14f;
  return saturate(a / b);
}

float3 PbrLinearToSrgb(float3 c) {
  float3 lo = c * 12.92f;
  float3 hi = 1.055f * pow(max(c, 0.0f), 1.0f / 2.4f) - 0.055f;
  return lerp(lo, hi, step(0.0031308f, c));
}

#endif // ENGINE_PBR_COMMON_SLANG

// brdf_lut.hpp — P4-05: split-sum BRDF LUT (Karis 2013) CPU reference.
//
// The LUT integrates the GGX geometry term and Schlick Fresnel over the
// hemisphere for one (NdotV, roughness) pair per texel:
//
//   scale  = int( F * G * VdH / (NdotV * NdotH) )   (F=1 part)
//   bias   = int( (1-F) * G * VdH / (NdotV * NdotH) ) (F=0 part)
//
// so the PBR shader can reconstruct specular IBL as
//   F0 * scale + bias  (times the prefiltered env sample).
//
// Layout contract (shared with brdf_lut.comp and test_brdf_lut.cpp):
//   - 512x512 texels, R32G32_SFLOAT (R=scale, G=bias).
//   - Texel (x = NdotV axis, y = roughness axis); texel centers:
//       ndv = (x + 0.5) / size, rough = (y + 0.5) / size.
//   - Import-time generated with the GPU compute shader; this header is
//     the CPU reference the test compares against. No Vulkan objects here
//     (compiles without the SDK, like hdr_camera.hpp).
//
// PBR wiring contract (coordinated with t_04b48e31 irradiance/prefilter):
//   - Vulkan image format: VK_FORMAT_R32G32_SFLOAT (matched storage +
//     sampled; RG32F storage-image write needs no extra feature).
//   - Sampling: CLAMP_TO_EDGE in U and V, LINEAR filter.
//   - Binding slot: bindless set binding 6 (set 0, binding 6) — free in
//     the M3-02..M3-03 fixed set (0..5 taken: scene/material/textures/
//     samplers/visibility/lights/grid/index — see bindless_table.hpp +
//     pbr_shade.slang). The IBL sibling (t_04b48e31) owns irradiance +
//     prefiltered cubemaps and the skybox/PBR integration; this card owns
//     only the LUT generation + CPU reference + sampled-texture contract.
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::renderer {

// Default LUT edge in texels (spec P4-05 §6: 512).
inline constexpr std::uint32_t kBrdfLutSize = 512;
// Sane import bounds.
inline constexpr std::uint32_t kBrdfLutMin = 16;
inline constexpr std::uint32_t kBrdfLutMax = 2048;
// Importance-sample count used by both CPU ref and GPU shader.
inline constexpr std::uint32_t kBrdfLutSamples = 1024;

// Import parameters: LUT edge (0 = default kBrdfLutSize).
struct BrdfLutImportParams {
  std::uint32_t lut_size{0};
};

// Validated import plan: resolved LUT edge. validate returns false (out
// untouched) on null out or edge outside [kBrdfLutMin, kBrdfLutMax].
struct BrdfLutImportPlan {
  std::uint32_t lut_size{kBrdfLutSize};
  std::uint64_t lut_bytes{0};  // edge^2 * 8 (RG32F: 2 floats)
};

[[nodiscard]] inline auto BrdfLutValidateImport(const BrdfLutImportParams& p,
                                               BrdfLutImportPlan* out) noexcept
    -> bool {
  if (out == nullptr) {
    return false;
  }
  const std::uint32_t edge = (p.lut_size != 0) ? p.lut_size : kBrdfLutSize;
  if (edge < kBrdfLutMin || edge > kBrdfLutMax) {
    return false;
  }
  out->lut_size = edge;
  out->lut_bytes =
      static_cast<std::uint64_t>(edge) * static_cast<std::uint64_t>(edge) * 8u;
  return true;
}

// --- Split-sum integration -----------------------------------------------
// Hammersley low-discrepancy sequence (i/N, radical inverse base 2).
inline auto BrdfRadicalInverseVdC(std::uint32_t bits) noexcept -> float {
  std::uint32_t reversed = bits;
  reversed = ((reversed << 16) | (reversed >> 16));
  reversed = (((reversed & 0x55555555u) << 1) | ((reversed & 0xAAAAAAAAu) >> 1));
  reversed = (((reversed & 0x33333333u) << 2) | ((reversed & 0xCCCCCCCCu) >> 2));
  reversed = (((reversed & 0x0F0F0F0Fu) << 4) | ((reversed & 0xF0F0F0F0u) >> 4));
  reversed = (((reversed & 0x00FF00FFu) << 8) | ((reversed & 0xFF00FF00u) >> 8));
  return static_cast<float>(reversed) * 2.3283064365386963e-10F;
}

inline auto BrdfHammersley(std::uint32_t i, std::uint32_t n,
                           float* u_out, float* v_out) noexcept -> void {
  const float u = static_cast<float>(i) / static_cast<float>(n);
  const float v = BrdfRadicalInverseVdC(i);
  if (u_out != nullptr) {
    *u_out = u;
  }
  if (v_out != nullptr) {
    *v_out = v;
  }
}

// GGX importance-sample direction for roughness a (= rough^2), N = +Z.
inline auto BrdfImportanceSampleGgx(float u, float v, float roughness,
                                    float h[3]) noexcept -> void {
  const float a = roughness * roughness;
  const float a2 = a * a;
  const float denom = 1.0F + (a2 - 1.0F) * v;
  const float cos_theta =
      (denom > 1e-7F) ? sqrtf((1.0F - v) / denom) : 1.0F;
  constexpr float kPi = 3.141592653589793F;
  const float phi = 2.0F * kPi * u;
  const float sin_theta =
      sqrtf(1.0F - (cos_theta < 1.0F ? cos_theta * cos_theta : 1.0F));
  h[0] = sin_theta * cosf(phi);
  h[1] = sin_theta * sinf(phi);
  h[2] = cos_theta;
}

// Schlick-GGX geometry term (k = (rough+1)^2/8 remap, Epic's fit).
inline auto BrdfGeometrySchlickGgx(float n_dot_v, float roughness) noexcept
    -> float {
  const float r = roughness + 1.0F;
  const float k = (r * r) / 8.0F;
  const float denom = n_dot_v * (1.0F - k) + k;
  return (denom > 1e-7F) ? n_dot_v / denom : 0.0F;
}

inline auto BrdfGeometrySmith(float n_dot_v, float n_dot_l,
                              float roughness) noexcept -> float {
  return BrdfGeometrySchlickGgx(n_dot_v, roughness) *
         BrdfGeometrySchlickGgx(n_dot_l, roughness);
}

// Integrate one (ndv, roughness) pair. sample_count 0 means the default
// kBrdfLutSamples. Out: scale (F=1 part), bias (F=0 part).
inline auto BrdfIntegrate(float ndv, float roughness,
                          std::uint32_t sample_count, float* scale_out,
                          float* bias_out) noexcept -> void {
  const std::uint32_t n =
      (sample_count != 0) ? sample_count : kBrdfLutSamples;
  // View vector in the N=+Z frame (NdotV = ndv, grazing-safe clamp).
  const float vx =
      (ndv < 1.0F) ? sqrtf(1.0F - ndv * ndv) : 0.0F;
  const float v[3] = {vx, 0.0F, (ndv < 0.0F ? 0.0F : ndv)};
  float scale = 0.0F;
  float bias = 0.0F;
  for (std::uint32_t i = 0; i < n; ++i) {
    float u = 0.0F;
    float vv = 0.0F;
    BrdfHammersley(i, n, &u, &vv);
    float h[3] = {0.0F, 0.0F, 1.0F};
    BrdfImportanceSampleGgx(u, vv, roughness, h);
    const float vdoth = v[0] * h[0] + v[1] * h[1] + v[2] * h[2];
    float l[3] = {2.0F * vdoth * h[0] - v[0], 2.0F * vdoth * h[1] - v[1],
                  2.0F * vdoth * h[2] - v[2]};
    const float n_dot_l = (l[2] > 0.0F) ? l[2] : 0.0F;
    if (n_dot_l > 0.0F) {
      const float n_dot_h = (h[2] > 0.0F) ? h[2] : 0.0F;
      if (n_dot_h > 0.0F && vdoth > 0.0F) {
        const float g = BrdfGeometrySmith(ndv, n_dot_l, roughness);
        const float g_vis = (g * vdoth) / ((n_dot_h * ndv) > 1e-7F
                                               ? (n_dot_h * ndv)
                                               : 1e-7F);
        const float fc = 1.0F - vdoth;
        const float fc5 = fc * fc * fc * fc * fc;
        scale += (1.0F - fc5) * g_vis;
        bias += fc5 * g_vis;
      }
    }
  }
  if (scale_out != nullptr) {
    *scale_out = scale / static_cast<float>(n);
  }
  if (bias_out != nullptr) {
    *bias_out = bias / static_cast<float>(n);
  }
}

// Texel-center inputs: x = NdotV axis, y = roughness axis.
inline auto BrdfLutTexelInputs(std::uint32_t x, std::uint32_t y,
                              std::uint32_t size, float* ndv_out,
                              float* rough_out) noexcept -> bool {
  if (ndv_out == nullptr || rough_out == nullptr || size == 0) {
    return false;
  }
  *ndv_out = (static_cast<float>(x) + 0.5F) / static_cast<float>(size);
  *rough_out = (static_cast<float>(y) + 0.5F) / static_cast<float>(size);
  return true;
}

// Reference LUT sample at texel (x, y) of a size-edge LUT.
inline auto BrdfLutReference(std::uint32_t x, std::uint32_t y,
                             std::uint32_t size, float* scale_out,
                             float* bias_out) noexcept -> bool {
  float ndv = 0.0F;
  float rough = 0.0F;
  if (!BrdfLutTexelInputs(x, y, size, &ndv, &rough)) {
    return false;
  }
  BrdfIntegrate(ndv, rough, kBrdfLutSamples, scale_out, bias_out);
  return scale_out != nullptr && bias_out != nullptr;
}

}  // namespace engine::renderer

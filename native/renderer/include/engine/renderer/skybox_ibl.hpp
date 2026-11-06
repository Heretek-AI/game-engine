// skybox_ibl.hpp — P4-05: diffuse irradiance + specular prefilter + skybox/PBR IBL.
//
// Import-time IBL generation from the P4-05 cubemap (t_933284d7) using the
// BRDF LUT contract (t_20b358bd, binding 6). This header owns ONLY the
// CPU-side surface + shared math; it creates no Vulkan objects and compiles
// without the SDK (like skybox_image_based_lighting.hpp / brdf_lut.hpp).
//
// Pipeline (all linear HDR, no sRGB anywhere — P4-02 tonemaps at frame time):
//   equirect .hdr --[equirect_to_cube.comp]--> cubemap (RGBA32F, 6 layers)
//     --[irradiance.comp]--> irradiance map (RGBA32F cube, 32px, 1 mip)
//     --[prefilter.comp]--> prefiltered map (RGBA32F cube, N mips, mip = rough)
//     --[brdf_lut.comp]--> BRDF LUT (RG32F 2D, binding 6)
//   frame: skybox.vert/frag samples cubemap; PBR evaluates
//     diffuse  = albedo * irradiance(N)               (Lambert, metals 0)
//     specular = prefiltered(R, rough) * (F0 * scale + bias)
//   with (scale, bias) = brdf_lut(NdotV, rough), F0 = mix(0.04, albedo, metal).
//
// Format contract:
//   - Irradiance: R32G32B32A32_SFLOAT cube, 6 layers, 1 mip, alpha 1.
//   - Prefilter: R32G32B32A32_SFLOAT cube, 6 layers, mip chain down to 1px
//     (mip_count = floor(log2(edge)) + 1, same layout rules as mipmaps).
//     Storage-image writes need one descriptor per mip (image2DArray views
//     are per-mip), but the CPU contract here only fixes sizes/counts.
//   - Sampling: CLAMP_TO_EDGE in all axes for both maps (no wrap: cubemap
//     faces carry their own edge continuity via SkyboxFaceDir); LINEAR
//     filter, LINEAR_MIPMAP_LINEAR for the prefilter (roughness -> mip).
//   - Bindings (set 0, coordinated with brdf_lut.hpp): irradiance = 7,
//     prefiltered = 8. (0..5 taken by scene/material/textures/samplers/
//     visibility/lights-grid/index per pbr_shade.slang comment; 6 = BRDF
//     LUT.) The compute shaders use their own throwaway set-0 layouts
//     (binding 0 = source cube sampler, binding 1 = dest storage image);
//     these numbers are the SAMPLED slots in the PBR/skybox frame set.
//
// Face convention: identical to skybox_image_based_lighting.hpp
// (SkyboxFaceDir, +X -X +Y -Y +Z -Z layer order, y-down texel rows).
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::renderer {

// Default import sizes (spec P4-05: irradiance low-res, prefilter from cube).
inline constexpr std::uint32_t kIrradianceFaceSize = 32;
inline constexpr std::uint32_t kIrradianceMin = 8;
inline constexpr std::uint32_t kIrradianceMax = 128;
// Prefilter base edge matches the source cubemap edge (spec default 512,
// test sizes smaller); mips run to 1px.
inline constexpr std::uint32_t kPrefilterMin = 16;
inline constexpr std::uint32_t kPrefilterMax = 2048;
// Importance-sample counts shared by CPU ref and compute shaders.
// Irradiance uses FEWER samples (diffuse kernel is smooth): 512.
// Prefilter matches the BRDF LUT: 1024.
inline constexpr std::uint32_t kIrradianceSamples = 512;
inline constexpr std::uint32_t kPrefilterSamples = 1024;
// Frame-set sampled binding slots (set 0).
inline constexpr std::uint32_t kIblIrradianceBinding = 7;
inline constexpr std::uint32_t kIblPrefilterBinding = 8;

// --- Irradiance import params ----------------------------------------------
// face_size 0 = default kIrradianceFaceSize.
struct IrradianceImportParams {
  std::uint32_t face_size{0};
};

struct IrradianceImportPlan {
  std::uint32_t face_size{kIrradianceFaceSize};
  std::uint64_t total_bytes{0};  // face^2 * 16 * 6
};

[[nodiscard]] inline auto IrradianceValidateImport(
    const IrradianceImportParams& p, IrradianceImportPlan* out) noexcept
    -> bool {
  if (out == nullptr) {
    return false;
  }
  const std::uint32_t face =
      (p.face_size != 0) ? p.face_size : kIrradianceFaceSize;
  if (face < kIrradianceMin || face > kIrradianceMax) {
    return false;
  }
  out->face_size = face;
  out->total_bytes =
      static_cast<std::uint64_t>(face) * static_cast<std::uint64_t>(face) *
      16u * 6u;
  return true;
}

// --- Prefilter import params ------------------------------------------------
// base_size 0 = source cubemap edge (import-time: kSkyboxCubemapSize 512).
struct PrefilterImportParams {
  std::uint32_t base_size{0};
};

struct PrefilterImportPlan {
  std::uint32_t base_size{512};
  std::uint32_t mip_count{0};    // floor(log2(base)) + 1
  std::uint64_t total_bytes{0};  // sum over mips of edge^2 * 16 * 6
};

[[nodiscard]] inline auto PrefilterMipCount(std::uint32_t base) noexcept
    -> std::uint32_t {
  if (base == 0) {
    return 0;
  }
  std::uint32_t n = 0;
  while (base > 0) {
    ++n;
    base >>= 1;
  }
  return n;
}

[[nodiscard]] inline auto PrefilterValidateImport(const PrefilterImportParams& p,
                                                 PrefilterImportPlan* out) noexcept
    -> bool {
  if (out == nullptr) {
    return false;
  }
  const std::uint32_t base = (p.base_size != 0) ? p.base_size : 512u;
  if (base < kPrefilterMin || base > kPrefilterMax) {
    return false;
  }
  out->base_size = base;
  out->mip_count = PrefilterMipCount(base);
  std::uint64_t bytes = 0;
  std::uint32_t edge = base;
  for (std::uint32_t m = 0; m < out->mip_count; ++m) {
    bytes += static_cast<std::uint64_t>(edge) * edge * 16u * 6u;
    edge = (edge > 1) ? (edge >> 1) : 1;
  }
  out->total_bytes = bytes;
  return true;
}

// Mip edge + roughness mapping: mip m of a base-edge chain has
// edge = max(base >> m, 1) and roughness = m / (mip_count - 1)
// (mip 0 = smooth mirror, last mip = fully rough). Single-mip chain
// (base 1, never valid here) would map to rough 0.
[[nodiscard]] inline auto PrefilterMipRoughness(std::uint32_t mip,
                                               std::uint32_t mip_count) noexcept
    -> float {
  if (mip_count <= 1) {
    return 0.0F;
  }
  if (mip >= mip_count) {
    return 1.0F;
  }
  return static_cast<float>(mip) / static_cast<float>(mip_count - 1);
}

inline auto PrefilterMipEdge(std::uint32_t base, std::uint32_t mip) noexcept
    -> std::uint32_t {
  std::uint32_t edge = base >> mip;
  return (edge == 0) ? 1 : edge;
}

// --- Shared sampling math ---------------------------------------------------
// Cosine-weighted hemisphere sample about +Z (irradiance kernel):
// u1 = uniform, u2 = uniform -> dir with pdf = cos/sqrt(pi)-free form.
// Matches irradiance.comp term for term.
inline auto IblCosineSample(float u1, float u2, float out[3]) noexcept -> void {
  constexpr float kPi = 3.141592653589793F;
  const float r = std::sqrt(u1 > 0.0F ? u1 : 0.0F);
  const float phi = 2.0F * kPi * u2;
  out[0] = r * std::cos(phi);
  out[1] = r * std::sin(phi);
  out[2] = std::sqrt(1.0F - (u1 < 1.0F ? u1 : 1.0F));
}

// Build an orthonormal basis about normal n (n unit). t/b are unit and
// orthogonal to n. Matches both compute shaders (Frisvad branchless-free
// form: pick the axis least aligned with n).
inline auto IblBasis(const float n[3], float t[3], float b[3]) noexcept
    -> void {
  float ax = 0.0F;
  float ay = 0.0F;
  float az = 0.0F;
  const float anx = (n[0] < 0.0F ? -n[0] : n[0]);
  const float any = (n[1] < 0.0F ? -n[1] : n[1]);
  const float anz = (n[2] < 0.0F ? -n[2] : n[2]);
  if (anx < any && anx < anz) {
    ax = 1.0F;
  } else if (any < anz) {
    ay = 1.0F;
  } else {
    az = 1.0F;
  }
  // t = normalize(cross(n, a)).
  float tx = n[1] * az - n[2] * ay;
  float ty = n[2] * ax - n[0] * az;
  float tz = n[0] * ay - n[1] * ax;
  const float tl = std::sqrt(tx * tx + ty * ty + tz * tz);
  if (tl > 1e-7F) {
    tx /= tl;
    ty /= tl;
    tz /= tl;
  }
  // b = cross(n, t).
  t[0] = tx;
  t[1] = ty;
  t[2] = tz;
  b[0] = n[1] * tz - n[2] * ty;
  b[1] = n[2] * tx - n[0] * tz;
  b[2] = n[0] * ty - n[1] * tx;
}

// Normal -> equirect-free ambient probe: cosine-weighted average of a
// procedural sky function over the hemisphere. `sample_sky(dir, rgb)` is
// the caller's sky radiance (test: analytic gradient; production: cubemap
// sample). sample_count 0 = default kIrradianceSamples. Out is irradiance
// (already includes the 1/pi... no: returns the MEAN radiance * pi, i.e.
// the diffuse irradiance for a Lambert term albedo * irr / pi? No —
// returns irradiance E = mean * pi so the shader does albedo * E / pi).
// We return E directly: shader divides by pi once.
template <typename SkyFn>
inline auto IblIrradianceReference(const float n[3], std::uint32_t sample_count,
                                  SkyFn sample_sky, float out[3]) noexcept
    -> void {
  const std::uint32_t count =
      (sample_count != 0) ? sample_count : kIrradianceSamples;
  float t[3] = {1.0F, 0.0F, 0.0F};
  float b[3] = {0.0F, 0.0F, 1.0F};
  IblBasis(n, t, b);
  double acc[3] = {0.0, 0.0, 0.0};
  for (std::uint32_t i = 0; i < count; ++i) {
    // Stratified jitter (Hammersley-like without VdC: deterministic grid +
    // golden-ratio offset keeps the CPU ref dependency-free).
    const float u1 =
        (static_cast<float>(i) + 0.5F) / static_cast<float>(count);
    float u2 = static_cast<float>(i) * 0.618033988749895F;
    u2 = u2 - std::floor(u2);
    float h[3] = {0.0F, 0.0F, 1.0F};
    IblCosineSample(u1, u2, h);
    // World dir = t*h.x + b*h.y + n*h.z.
    float d[3] = {t[0] * h[0] + b[0] * h[1] + n[0] * h[2],
                  t[1] * h[0] + b[1] * h[1] + n[1] * h[2],
                  t[2] * h[0] + b[2] * h[1] + n[2] * h[2]};
    float rgb[3] = {0.0F, 0.0F, 0.0F};
    sample_sky(d, rgb);
    acc[0] += rgb[0];
    acc[1] += rgb[1];
    acc[2] += rgb[2];
  }
  constexpr float kPi = 3.141592653589793F;
  const float k = kPi / static_cast<float>(count);
  out[0] = static_cast<float>(acc[0] * k);
  out[1] = static_cast<float>(acc[1] * k);
  out[2] = static_cast<float>(acc[2] * k);
}

// --- Split-sum IBL evaluation (frame shader reference) -----------------------
// Reconstructs specular IBL from the prefiltered sample + LUT:
//   spec = prefiltered * (f0 * scale + bias)
//   diff = albedo * (1 - metal) * irradiance / pi
//   ibl  = diff + spec
// All linear HDR. Mirrors the PBR wiring the sibling test renders.
inline auto IblEvaluate(float albedo_r, float albedo_g, float albedo_b,
                        float metal, float ndv, float rough, float irr_r,
                        float irr_g, float irr_b, float pref_r, float pref_g,
                        float pref_b, float lut_scale, float lut_bias,
                        float out[3]) noexcept -> void {
  constexpr float kPi = 3.141592653589793F;
  const float m = (metal < 0.0F ? 0.0F : (metal > 1.0F ? 1.0F : metal));
  // F0 = mix(0.04 dielectric, albedo, metal).
  const float f0r = 0.04F + (albedo_r - 0.04F) * m;
  const float f0g = 0.04F + (albedo_g - 0.04F) * m;
  const float f0b = 0.04F + (albedo_b - 0.04F) * m;
  const float k = (1.0F - m) / kPi;
  const float dr = albedo_r * k * irr_r;
  const float dg = albedo_g * k * irr_g;
  const float db = albedo_b * k * irr_b;
  const float sr = pref_r * (f0r * lut_scale + lut_bias);
  const float sg = pref_g * (f0g * lut_scale + lut_bias);
  const float sb = pref_b * (f0b * lut_scale + lut_bias);
  (void)ndv;
  (void)rough;
  out[0] = dr + sr;
  out[1] = dg + sg;
  out[2] = db + sb;
}

// --- Skybox view math ----------------------------------------------------------
// Fullscreen-triangle skybox: clip = (ndc.xy, 1, 1) pushed to the far plane
// (depth = w, i.e. NDC z = 1 with depth-compare LESS... production uses
// GREATER/EQUAL or depth-write-off + draw-first; the test draws skybox
// first with depthWrite OFF and depthTest ALWAYS so sphere pixels always
// overwrite). View ray in world space: dir = normalize(inv_view_rot * v),
// where v = (ndc.x * tan(fovx/2) * aspect... ) — the test passes NDC and
// the inverse view-projection WITHOUT translation; this helper applies
// the 3x3 (rotation-only) part of a row-major 4x4 to an NDC direction.
inline auto SkyboxViewDir(const float inv_view_3x3[9], float ndc_x,
                          float ndc_y, float out[3]) noexcept -> void {
  // View-space ray for a perspective camera looking down -Z with
  // focal length folded into the inverse matrix: out = M * (ndc_x, ndc_y, -1).
  const float vx = ndc_x;
  const float vy = ndc_y;
  const float vz = -1.0F;
  float dx = inv_view_3x3[0] * vx + inv_view_3x3[1] * vy +
             inv_view_3x3[2] * vz;
  float dy = inv_view_3x3[3] * vx + inv_view_3x3[4] * vy +
             inv_view_3x3[5] * vz;
  float dz = inv_view_3x3[6] * vx + inv_view_3x3[7] * vy +
             inv_view_3x3[8] * vz;
  const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (len > 1e-7F) {
    dx /= len;
    dy /= len;
    dz /= len;
  }
  out[0] = dx;
  out[1] = dy;
  out[2] = dz;
}

// RGBA/BGRA swizzle note (gotcha 23): swapchain goldens on lavapipe come
// back B8G8R8A8 while our maps are RGBA32F. The test swizzles R<->B when
// writing PNGs / comparing readbacks — never swizzle the HDR maps.

}  // namespace engine::renderer

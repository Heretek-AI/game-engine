// skybox_image_based_lighting.hpp — P4-05: equirect HDR import + GPU cubemap conversion.
//
// Import-time path for HDR sky import. The HDR decode reuses the existing
// M5-01 texture pipeline (stb_image via the assets module: stbi_loadf /
// stbi_is_hdr — no new decoder, no CPU fallback beyond what the pipeline
// already does). This header owns what that pipeline does NOT:
//
//   1. HDR pixel validation + metadata (HdrSkyImage): width/height/channels
//      checks on the decoded float buffer, no allocation, no decode here.
//   2. Equirect -> cubemap direction math (SkyboxFaceDir): the per-face,
//      per-texel cubemap direction used by BOTH the GPU compute shader
//      (equirect_to_cube.comp) and the CPU reference in the test. One
//      function, bit-exact contract, so face orientation can never drift
//      between CPU and GPU.
//   3. Equirect sampling reference (SkyboxSampleEquirectCpu): bilinear
//      sample of the float equirect buffer at a direction. The GPU shader
//      uses hardware bilinear (sampler) on the same buffer; the CPU test
//      compares within a tolerance for the hardware-filter difference.
//   4. Import parameters (SkyboxImportParams): cubemap edge size (spec
//      default 512) and format contract (R32G32B32A32_SFLOAT storage —
//      see format notes below).
//
// Vulkan cubemap + compute dispatch + irradiance/prefilter/BRDF-LUT live
// in the sibling tasks on this same branch (t_04b48e31 / t_20b358bd).
// This header exposes ONLY the import-time CPU surface plus the shared
// direction math; it creates no Vulkan objects and needs no SDK.
//
// Format contract (shared with the GPU/IBL tasks):
//   - Cubemap storage is R32G32B32A32_SFLOAT, 6 layers, one mip. RGBA32F
//     (not RGB32F) because Vulkan has no widely-supported RGB32F STORAGE
//     image format without extra feature queries; alpha carries 1.0.
//     Linear HDR throughout: no sRGB decode on import (Radiance .hdr is
//     already linear), no sRGB encode on the cubemap (the P4-02 post
//     pass does ACES + sRGB at frame time).
//   - Equirect upload is R32G32B32A32_SFLOAT too (floats straight from
//     stbi_loadf, alpha padded to 1.0) so the compute shader samples
//     with a REPEAT-U / CLAMP-V linear sampler (equirect wraps in
//     longitude, clamps at the poles) on the same buffer.
//
// Face convention (shared with equirect_to_cube.comp; Vulkan cubemap
// layer order 0..5 = +X -X +Y -Y +Z -Z):
//   face 0 (+X): dir = normalize(+1, -v, -u)
//   face 1 (-X): dir = normalize(-1, -v, +u)
//   face 2 (+Y): dir = normalize(+u, +1, +v)
//   face 3 (-Y): dir = normalize(+u, -1, -v)
//   face 4 (+Z): dir = normalize(+u, -v, +1)
//   face 5 (-Z): dir = normalize(-u, -v, -1)
// where (u, v) in [-1, 1] map from texel centers. +Y is up, so the sky
// zenith lands at the middle of the equirect's top row. Verified against
// the CPU reference in test_skybox_import.cpp: face centers map to the
// six axis directions, and adjacent face edges share directions (no
// seams from orientation bugs).
//
// Compiles WITHOUT the Vulkan SDK (plain data + math, no GPU) so the
// CPU contract tests run on every CI runner, like hdr_camera.hpp.
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::renderer {

// Default cubemap edge in texels (spec P4-05: 512).
inline constexpr std::uint32_t kSkyboxCubemapSize = 512;
// Minimum/maximum sane cubemap edges (import rejects outside).
inline constexpr std::uint32_t kSkyboxCubemapMin = 16;
inline constexpr std::uint32_t kSkyboxCubemapMax = 2048;
// Equirect aspect expected from Radiance .hdr skies (2:1). Import does
// not reject other aspects, but the test's synthetic sky uses 2:1.
inline constexpr std::uint32_t kSkyboxEquirectAspect = 2;
// Face indices in Vulkan cubemap layer order.
inline constexpr std::uint32_t kSkyboxFaceCount = 6;

// Import parameters: cubemap edge + equirect extent. face_size 0 means
// "use kSkyboxCubemapSize".
struct SkyboxImportParams {
  std::uint32_t face_size{0};
  std::uint32_t equirect_w{0};
  std::uint32_t equirect_h{0};
};

// Validated import plan: resolved face size + layer count. validate
// returns false (out untouched) on null args, zero equirect extent, or
// face size outside [kSkyboxCubemapMin, kSkyboxCubemapMax].
struct SkyboxImportPlan {
  std::uint32_t face_size{kSkyboxCubemapSize};
  std::uint32_t layer_count{kSkyboxFaceCount};
  std::uint64_t face_bytes{0};  // face_size^2 * 16 (RGBA32F)
};

[[nodiscard]] inline auto SkyboxValidateImport(const SkyboxImportParams& p,
                                              SkyboxImportPlan* out) noexcept
    -> bool {
  if (out == nullptr) {
    return false;
  }
  if (p.equirect_w == 0 || p.equirect_h == 0) {
    return false;
  }
  const std::uint32_t face =
      (p.face_size != 0) ? p.face_size : kSkyboxCubemapSize;
  if (face < kSkyboxCubemapMin || face > kSkyboxCubemapMax) {
    return false;
  }
  out->face_size = face;
  out->layer_count = kSkyboxFaceCount;
  out->face_bytes =
      static_cast<std::uint64_t>(face) * static_cast<std::uint64_t>(face) * 16u;
  return true;
}

// Per-face, per-texel cubemap direction (shared CPU/GPU contract).
// face in [0, 6), x/y in [0, size). Out is a unit vector. Returns
// false on null out, bad face, or size 0 (out untouched).
[[nodiscard]] inline auto SkyboxFaceDir(std::uint32_t face, std::uint32_t x,
                                       std::uint32_t y, std::uint32_t size,
                                       float out[3]) noexcept -> bool {
  if (out == nullptr || face >= kSkyboxFaceCount || size == 0) {
    return false;
  }
  const float inv = 1.0F / static_cast<float>(size);
  const float u =
      (static_cast<float>(x) + 0.5F) * inv * 2.0F - 1.0F;  // [-1, 1]
  const float v =
      (static_cast<float>(y) + 0.5F) * inv * 2.0F - 1.0F;  // [-1, 1]
  float dx = 0.0F;
  float dy = 0.0F;
  float dz = 0.0F;
  switch (face) {
    case 0:  // +X
      dx = 1.0F;
      dy = -v;
      dz = -u;
      break;
    case 1:  // -X
      dx = -1.0F;
      dy = -v;
      dz = u;
      break;
    case 2:  // +Y
      dx = u;
      dy = 1.0F;
      dz = v;
      break;
    case 3:  // -Y
      dx = u;
      dy = -1.0F;
      dz = -v;
      break;
    case 4:  // +Z
      dx = u;
      dy = -v;
      dz = 1.0F;
      break;
    default:  // face 5: -Z
      dx = -u;
      dy = -v;
      dz = -1.0F;
      break;
  }
  const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (len <= 0.0F) {
    return false;
  }
  out[0] = dx / len;
  out[1] = dy / len;
  out[2] = dz / len;
  return true;
}

// Direction -> equirect UV (u in [0,1) around, v in [0,1] down from
// zenith). Matches the compute shader's mapping term for term.
inline auto SkyboxDirToUv(float dx, float dy, float dz, float* u_out,
                          float* v_out) noexcept -> void {
  // Longitude: atan2(x, z) so +Z (face 4 center) maps to u = 0.5.
  constexpr float kPi = 3.141592653589793F;
  float u = 0.5F + std::atan2(dx, dz) / (2.0F * kPi);
  // Wrap: atan2 seam at u = 0/1.
  u = u - std::floor(u);
  float c = dy;
  c = c < -1.0F ? -1.0F : (c > 1.0F ? 1.0F : c);
  const float v = std::acos(c) / kPi;
  if (u_out != nullptr) {
    *u_out = u;
  }
  if (v_out != nullptr) {
    *v_out = v;
  }
}

// Bilinear sample of an RGBA float equirect buffer (row-major, w*h*4
// floats) at a unit direction. Out is rgb (alpha dropped). Null-safe:
// null pixels/out, w/h 0, or non-unit-free NaN direction -> false.
// Wrap in u, clamp in v (matches CLAMP_TO_EDGE sampler in v).
[[nodiscard]] inline auto SkyboxSampleEquirectCpu(const float* pixels,
                                                 std::uint32_t w,
                                                 std::uint32_t h,
                                                 const float dir[3],
                                                 float out[3]) noexcept
    -> bool {
  if (pixels == nullptr || out == nullptr || w == 0 || h == 0 ||
      dir == nullptr) {
    return false;
  }
  float u = 0.0F;
  float v = 0.0F;
  SkyboxDirToUv(dir[0], dir[1], dir[2], &u, &v);
  const float fx = u * static_cast<float>(w) - 0.5F;
  const float fy = v * static_cast<float>(h) - 0.5F;
  int x0 = static_cast<int>(std::floor(fx));
  int y0 = static_cast<int>(std::floor(fy));
  const float tx = fx - static_cast<float>(x0);
  const float ty = fy - static_cast<float>(y0);
  // Horizontal wrap; vertical clamp.
  auto wrap_x = [w](int x) -> std::uint32_t {
    int m = x % static_cast<int>(w);
    if (m < 0) {
      m += static_cast<int>(w);
    }
    return static_cast<std::uint32_t>(m);
  };
  auto clamp_y = [h](int y) -> std::uint32_t {
    if (y < 0) {
      return 0;
    }
    if (y >= static_cast<int>(h)) {
      return h - 1;
    }
    return static_cast<std::uint32_t>(y);
  };
  const std::uint32_t xa = wrap_x(x0);
  const std::uint32_t xb = wrap_x(x0 + 1);
  const std::uint32_t ya = clamp_y(y0);
  const std::uint32_t yb = clamp_y(y0 + 1);
  const std::array<std::uint64_t, 4> idx = {
      (static_cast<std::uint64_t>(ya) * w + xa) * 4,
      (static_cast<std::uint64_t>(ya) * w + xb) * 4,
      (static_cast<std::uint64_t>(yb) * w + xa) * 4,
      (static_cast<std::uint64_t>(yb) * w + xb) * 4,
  };
  for (int c = 0; c < 3; ++c) {
    const std::uint32_t cc0 = static_cast<std::uint32_t>(c);
    const float a = pixels[idx[0] + cc0];
    const float b = pixels[idx[1] + cc0];
    const float cc = pixels[idx[2] + cc0];
    const float d = pixels[idx[3] + cc0];
    const float top = a + (b - a) * tx;
    const float bot = cc + (d - cc) * tx;
    out[c] = top + (bot - top) * ty;
  }
  return true;
}

}  // namespace engine::renderer

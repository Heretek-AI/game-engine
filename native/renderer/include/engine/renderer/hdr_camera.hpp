// hdr_camera.hpp — P4-02: HDR camera exposure (CPU-side, no GPU needed).
//
// The shade pass writes linear-HDR radiance into R16G16B16A16_SFLOAT; the
// post pass multiplies by exposure, then ACES + sRGB encodes to the
// swapchain LDR image. Exposure here is a plain linear multiplier driven
// by the camera: EV stops convert as 2^ev (ev=0 -> 1.0, ev=-3 -> 0.125).
//
// The component twin (HdrCameraComponent, blittable) is the ECS authoring
// path (mirrors MaterialComponent in pbr_material.hpp: same register path,
// see gotcha 20); HdrExposure is the value type frames actually use.
#pragma once

#include <cmath>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"

// Frame exposure: linear multiplier applied in the post pass BEFORE the
// ACES fit (post_composite.frag: hdr * exposure -> aces -> sRGB).
// ev_stops converts 2^ev; ev=0 is unity.
namespace engine::renderer {

// exposure = 2^ev_stops. ev=-3 -> 0.125: an emissive radiance of 10
// becomes 1.25 pre-tonemap, still inside the ACES curve (no clip to
// white) — the P4-02 acceptance probe.
[[nodiscard]] inline auto HdrExposureFromEv(float ev_stops) noexcept
    -> float {
  return std::exp2(ev_stops);
}

// ECS component twin (blittable): exposure in EV stops, matching the
// PostPush.exposure multiplier after HdrExposureFromEv conversion.
struct HdrCameraComponent {
  float ev_stops{0.0f};
  float pad0{0.0f};
  float pad1{0.0f};
  float pad2{0.0f};
};
static_assert(sizeof(HdrCameraComponent) == 16, "hdr camera is 16B");

// CPU reference for the post pass (mirrors post_composite.frag
// term-for-term: hdr * exposure -> Narkowicz ACES -> sRGB encode).
// rgb/exposure are linear-HDR; out is display-ready LDR. Null-safe:
// null rgb or out leaves out untouched... (callers pass valid arrays;
// null returns without writing).
[[nodiscard]] inline auto HdrTonemapCpu(const float rgb[3], float exposure,
                                       float out[3]) noexcept -> bool {
  if (rgb == nullptr || out == nullptr) {
    return false;
  }
  for (int k = 0; k < 3; ++k) {
    const float x = rgb[k] * exposure;
    const float a = x * (2.51f * x + 0.03f);
    const float b = x * (2.43f * x + 0.59f) + 0.14f;
    float m = (b > 1e-6f) ? a / b : 0.0f;
    m = m < 0.0f ? 0.0f : (m > 1.0f ? 1.0f : m);
    const float c = m < 0.0f ? 0.0f : m;
    const float lo = c * 12.92f;
    const double hi =
        1.055 * std::pow(c > 0.0f ? static_cast<double>(c) : 0.0, 1.0 / 2.4) -
        0.055;
    const auto hi_f = static_cast<float>(hi);
    out[k] = (c <= 0.0031308f) ? lo : hi_f;
  }
  return true;
}

}  // namespace engine::renderer

// gi_cascade.cpp — M3-04 AC-2: cascade plan sizing + CPU reference
// multibounce integrator (sphere-traced analytic SDF).
#include "engine/renderer/gi_cascade.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::renderer {
namespace {

inline auto clamp01(float v) noexcept -> float {
  if (v < 0.0f) {
    return 0.0f;
  }
  if (v > 1.0f) {
    return 1.0f;
  }
  return v;
}

// 6 fixed hemisphere taps around the normal (deterministic, no RNG):
// normal, 4x tilted at 60 degrees, 1x grazing back-tap.
inline auto tap_dir(const float n[3], int i, float o[3]) noexcept -> void {
  // Build a tangent frame.
  float t[3] = {1.0f, 0.0f, 0.0f};
  if (n[0] > 0.9f || n[0] < -0.9f) {
    t[0] = 0.0f;
    t[1] = 1.0f;
  }
  // t = normalize(t - n * dot(t,n)); b = n x t.
  const float d = t[0] * n[0] + t[1] * n[1] + t[2] * n[2];
  t[0] -= n[0] * d;
  t[1] -= n[1] * d;
  t[2] -= n[2] * d;
  const float tl = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
  if (tl > 1e-6f) {
    t[0] /= tl;
    t[1] /= tl;
    t[2] /= tl;
  }
  float b[3] = {n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2],
                n[0] * t[1] - n[1] * t[0]};
  constexpr float kTilt = 0.8660254f;  // cos(30 deg): 60-deg cone taps
  constexpr float kUp = 0.5f;          // sin(30 deg)
  switch (i) {
    case 0:
      o[0] = n[0];
      o[1] = n[1];
      o[2] = n[2];
      break;
    case 1:
      o[0] = t[0] * kTilt + n[0] * kUp;
      o[1] = t[1] * kTilt + n[1] * kUp;
      o[2] = t[2] * kTilt + n[2] * kUp;
      break;
    case 2:
      o[0] = -t[0] * kTilt + n[0] * kUp;
      o[1] = -t[1] * kTilt + n[1] * kUp;
      o[2] = -t[2] * kTilt + n[2] * kUp;
      break;
    case 3:
      o[0] = b[0] * kTilt + n[0] * kUp;
      o[1] = b[1] * kTilt + n[1] * kUp;
      o[2] = b[2] * kTilt + n[2] * kUp;
      break;
    case 4:
      o[0] = -b[0] * kTilt + n[0] * kUp;
      o[1] = -b[1] * kTilt + n[1] * kUp;
      o[2] = -b[2] * kTilt + n[2] * kUp;
      break;
    default:
      o[0] = -n[0] * kUp + t[0] * kTilt;
      o[1] = -n[1] * kUp + t[1] * kTilt;
      o[2] = -n[2] * kUp + t[2] * kTilt;
      break;
  }
  const float l = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
  if (l > 1e-9f) {
    o[0] /= l;
    o[1] /= l;
    o[2] /= l;
  }
}

}  // namespace

auto GiCascadePlan::init(const float aabb_min[3],
                         const float aabb_max[3]) noexcept -> Status {
  if (aabb_min == nullptr || aabb_max == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "gi plan: null aabb");
  }
  for (int k = 0; k < 3; ++k) {
    if (!(aabb_min[k] < aabb_max[k])) {
      return MakeFail(StatusCode::kInvalidArgument, "gi plan: degenerate aabb");
    }
    aabb_min_[k] = aabb_min[k];
    aabb_max_[k] = aabb_max[k];
  }
  ready_ = true;
  return MakeOk();
}

auto GiCascadePlan::level_probe_bytes(std::uint32_t level) const noexcept
    -> std::uint64_t {
  if (!ready_ || level >= kGiCascadeLevels) {
    return 0;
  }
  // Fixed probe count per level (uniform grids, 4x spacing growth is
  // expressed in coverage, not count — every level holds the same
  // kGiProbeRes^3 probes over a 4x-wider volume).
  constexpr std::uint64_t kProbes =
      static_cast<std::uint64_t>(kGiProbeRes) * kGiProbeRes * kGiProbeRes;
  constexpr std::uint64_t kBytesPerProbe = 16;  // RGBA16F irradiance + packing
  return kProbes * kBytesPerProbe;
}

auto GiCascadePlan::total_probe_bytes() const noexcept -> std::uint64_t {
  if (!ready_) {
    return 0;
  }
  return level_probe_bytes(0) * kGiCascadeLevels;
}

auto GiCascadePlan::level_spacing(std::uint32_t level) const noexcept
    -> float {
  if (!ready_ || level >= kGiCascadeLevels) {
    return 0.0f;
  }
  float s = kGiBaseSpacing;
  for (std::uint32_t l = 0; l < level; ++l) {
    s *= 4.0f;
  }
  return s;
}

auto GiCascadePlan::scene_sdf(const float p[3],
                              const GiSphereOccluder* occluders,
                              std::uint32_t occluder_count) noexcept -> float {
  if (occluders == nullptr || occluder_count == 0) {
    return 1e30f;  // open sky
  }
  float best = 1e30f;
  for (std::uint32_t i = 0; i < occluder_count; ++i) {
    const float dx = p[0] - occluders[i].center[0];
    const float dy = p[1] - occluders[i].center[1];
    const float dz = p[2] - occluders[i].center[2];
    const float d =
        std::sqrt(dx * dx + dy * dy + dz * dz) - occluders[i].radius;
    if (d < best) {
      best = d;
    }
  }
  return best;
}

auto GiCascadePlan::integrate(const GiSampleRequest& req,
                              const GiSphereOccluder* occluders,
                              std::uint32_t occluder_count,
                              const float sky_color[3], float bounce_gain,
                              float ao_strength) noexcept -> GiSampleResult {
  GiSampleResult r{};
  r.irradiance[0] = 0.0f;
  r.irradiance[1] = 0.0f;
  r.irradiance[2] = 0.0f;
  r.occlusion = 0.0f;
  r.steps = 0;

  float sky[3] = {0.2f, 0.3f, 0.5f};
  if (sky_color != nullptr) {
    sky[0] = sky_color[0];
    sky[1] = sky_color[1];
    sky[2] = sky_color[2];
  }
  if (!(bounce_gain >= 0.0f)) {
    bounce_gain = 0.0f;
  }
  if (!(ao_strength >= 0.0f)) {
    ao_strength = 0.0f;
  }
  // Start slightly off the surface to avoid self-intersection.
  float base[3] = {req.world_pos[0] + req.normal[0] * 1e-3f,
                   req.world_pos[1] + req.normal[1] * 1e-3f,
                   req.world_pos[2] + req.normal[2] * 1e-3f};
  // Direct sky term: precomputed visibility * sky * albedo (Lambert / pi).
  constexpr float kInvPi = 0.3183099f;
  const float vis = clamp01(req.sky_visibility);
  float acc[3] = {req.albedo[0] * sky[0] * vis * kInvPi,
                  req.albedo[1] * sky[1] * vis * kInvPi,
                  req.albedo[2] * sky[2] * vis * kInvPi};
  // One diffuse bounce: 6 hemisphere taps, sphere-traced; hits contribute
  // their albedo-scaled sky (multibounce energy), misses contribute sky.
  constexpr int kTaps = 6;
  float occ = 0.0f;
  std::uint32_t steps_total = 0;
  for (int i = 0; i < kTaps; ++i) {
    float dir[3];
    tap_dir(req.normal, i, dir);
    float p[3] = {base[0], base[1], base[2]};
    bool hit = false;
    std::uint32_t steps = 0;
    for (std::uint32_t s = 0; s < kGiMaxTraceSteps; ++s) {
      const float d = scene_sdf(p, occluders, occluder_count);
      ++steps;
      if (d < 1e-3f) {
        hit = true;
        break;
      }
      if (d > 1e29f) {
        break;  // open sky: no occluders
      }
      if (d > 64.0f * kGiBaseSpacing) {
        break;  // far field: treat as sky
      }
      p[0] += dir[0] * d;
      p[1] += dir[1] * d;
      p[2] += dir[2] * d;
    }
    steps_total += steps;
    if (hit) {
      occ += 1.0f / kTaps;
      // Bounce: occluder albedo unknown in the analytic reference — reuse
      // the sample albedo at reduced energy (energy-conserving: gain <= 1
      // keeps the sum convergent; tests pin gain = 0.5 behavior).
      acc[0] += req.albedo[0] * sky[0] * (bounce_gain / kTaps) * kInvPi;
      acc[1] += req.albedo[1] * sky[1] * (bounce_gain / kTaps) * kInvPi;
      acc[2] += req.albedo[2] * sky[2] * (bounce_gain / kTaps) * kInvPi;
    } else {
      acc[0] += req.albedo[0] * sky[0] * (1.0f / kTaps) * kInvPi;
      acc[1] += req.albedo[1] * sky[1] * (1.0f / kTaps) * kInvPi;
      acc[2] += req.albedo[2] * sky[2] * (1.0f / kTaps) * kInvPi;
    }
  }
  const float ao = clamp01(occ * ao_strength);
  r.irradiance[0] = acc[0] * (1.0f - ao);
  r.irradiance[1] = acc[1] * (1.0f - ao);
  r.irradiance[2] = acc[2] * (1.0f - ao);
  r.occlusion = occ;
  r.steps = steps_total;
  return r;
}

}  // namespace engine::renderer

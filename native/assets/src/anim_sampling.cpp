// anim_sampling.cpp — P5-02 CPU clip sampling; see anim_sampling.hpp.
#include "engine/assets/anim_sampling.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::assets {
namespace {

auto IsFinite(float v) noexcept -> bool {
  return std::isfinite(v) != 0;
}

// Component count per key for a resolved (non-cubicspline) payload.
auto CompsFor(AnimTargetPath path) noexcept -> std::size_t {
  return (path == AnimTargetPath::kRotation) ? 4U : 3U;
}

auto Copy3(const float* src, float* dst) noexcept -> void {
  dst[0] = src[0];
  dst[1] = src[1];
  dst[2] = src[2];
}

auto Copy4(const float* src, float* dst) noexcept -> void {
  dst[0] = src[0];
  dst[1] = src[1];
  dst[2] = src[2];
  dst[3] = src[3];
}

auto Lerp3(const float* a, const float* b, float u, float* dst) noexcept
    -> void {
  for (int k = 0; k < 3; ++k) {
    dst[k] = a[k] + (b[k] - a[k]) * u;
  }
}

// Normalize an (x,y,z,w) quat in place; zero-length -> identity.
auto NormalizeQuat(float* q) noexcept -> void {
  const float n2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
  if (!(n2 > 0.0F) || !IsFinite(n2)) {
    q[0] = 0.0F;
    q[1] = 0.0F;
    q[2] = 0.0F;
    q[3] = 1.0F;
    return;
  }
  const float inv = 1.0F / std::sqrt(n2);
  q[0] *= inv;
  q[1] *= inv;
  q[2] *= inv;
  q[3] *= inv;
}

// Slerp of unit quats a -> b (x,y,z,w); shortest path; near-identical falls
// back to normalized lerp. Result normalized by construction.
auto SlerpQuat(const float* a, const float* b, float u, float* dst) noexcept
    -> void {
  float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  float bs[4] = {b[0], b[1], b[2], b[3]};
  if (dot < 0.0F) {
    dot = -dot;
    bs[0] = -bs[0];
    bs[1] = -bs[1];
    bs[2] = -bs[2];
    bs[3] = -bs[3];
  }
  constexpr float kOneMinusEps = 1.0F - 1e-6F;
  if (dot > kOneMinusEps) {
    for (int k = 0; k < 4; ++k) {
      dst[k] = a[k] + (bs[k] - a[k]) * u;
    }
    NormalizeQuat(dst);
    return;
  }
  const float theta = std::acos(dot > 1.0F ? 1.0F : dot);
  const float sin_theta = std::sin(theta);
  if (!(sin_theta > 1e-6F) || !IsFinite(sin_theta)) {
    Copy4(a, dst);
    return;
  }
  const float wa = std::sin((1.0F - u) * theta) / sin_theta;
  const float wb = std::sin(u * theta) / sin_theta;
  for (int k = 0; k < 4; ++k) {
    dst[k] = a[k] * wa + bs[k] * wb;
  }
  NormalizeQuat(dst);
}

// Hermite basis for u in [0,1].
auto Hermite(float p0, float p1, float m0, float m1, float u) noexcept
    -> float {
  const float u2 = u * u;
  const float u3 = u2 * u;
  const float h00 = 2.0F * u3 - 3.0F * u2 + 1.0F;
  const float h10 = u3 - 2.0F * u2 + u;
  const float h01 = -2.0F * u3 + 3.0F * u2;
  const float h11 = u3 - u2;
  return h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1;
}

// Locate the segment [i, i+1] with times[i] <= t < times[i+1].
// Precondition: key_count >= 2, t strictly inside (times[0], times[last]).
// Returns i in [0, key_count - 2]. Linear scan: key counts per channel are
// small and the scan touches one cache line; no state, no alloc.
auto FindSegment(const float* times, std::size_t key_count,
                 float t) noexcept -> std::size_t {
  std::size_t i = 0;
  while (i + 1 < key_count - 1 && times[i + 1] <= t) {
    ++i;
  }
  return i;
}

}  // namespace

auto AnimPoseReset(AnimPose pose) noexcept -> void {
  for (std::size_t j = 0; j < pose.joint_count; ++j) {
    if (pose.translation != nullptr) {
      pose.translation[j * 3 + 0] = 0.0F;
      pose.translation[j * 3 + 1] = 0.0F;
      pose.translation[j * 3 + 2] = 0.0F;
    }
    if (pose.rotation != nullptr) {
      pose.rotation[j * 4 + 0] = 0.0F;
      pose.rotation[j * 4 + 1] = 0.0F;
      pose.rotation[j * 4 + 2] = 0.0F;
      pose.rotation[j * 4 + 3] = 1.0F;
    }
    if (pose.scale != nullptr) {
      pose.scale[j * 3 + 0] = 1.0F;
      pose.scale[j * 3 + 1] = 1.0F;
      pose.scale[j * 3 + 2] = 1.0F;
    }
    if (pose.set != nullptr) {
      pose.set[j] = 0;
    }
  }
}

auto SampleAnimChannel(const AnimChannel& channel, float t,
                       float* out) noexcept -> bool {
  if (out == nullptr || !IsFinite(t)) {
    return false;
  }
  const std::size_t keys = channel.times.size();
  if (keys == 0) {
    return false;
  }
  const std::size_t comps = CompsFor(channel.target_path);
  const float* times = channel.times.data();
  const float* vals = channel.values.data();

  if (channel.interpolation == AnimInterpolation::kCubicSpline) {
    if (channel.values.size() != keys * comps * 3U) {
      return false;
    }
    // Clamp: before first -> value of key 0 (middle triplet); at/after
    // last -> value of last key.
    if (!(t > times[0])) {
      const float* v = vals + comps;
      if (comps == 4) {
        Copy4(v, out);
        NormalizeQuat(out);
      } else {
        Copy3(v, out);
      }
      return true;
    }
    if (!(t < times[keys - 1])) {
      const float* v = vals + (keys - 1) * comps * 3U + comps;
      if (comps == 4) {
        Copy4(v, out);
        NormalizeQuat(out);
      } else {
        Copy3(v, out);
      }
      return true;
    }
    const std::size_t i = FindSegment(times, keys, t);
    const float dt = times[i + 1] - times[i];
    if (!(dt > 0.0F) || !IsFinite(dt)) {
      const float* v = vals + i * comps * 3U + comps;
      if (comps == 4) {
        Copy4(v, out);
        NormalizeQuat(out);
      } else {
        Copy3(v, out);
      }
      return true;
    }
    const float u = (t - times[i]) / dt;
    const float* base_i = vals + i * comps * 3U;
    const float* base_j = vals + (i + 1) * comps * 3U;
    const float* p0 = base_i + comps;       // value of key i
    const float* m0 = base_i + 2 * comps;   // out-tangent of key i
    const float* m1 = base_j + 0 * comps;   // in-tangent of key i+1
    const float* p1 = base_j + comps;       // value of key i+1
    for (std::size_t k = 0; k < comps; ++k) {
      out[k] = Hermite(p0[k], p1[k], m0[k] * dt, m1[k] * dt, u);
    }
    if (comps == 4) {
      NormalizeQuat(out);
    }
    return true;
  }

  if (channel.values.size() != keys * comps) {
    return false;
  }
  if (!(t > times[0])) {
    if (comps == 4) {
      Copy4(vals, out);
      NormalizeQuat(out);
    } else {
      Copy3(vals, out);
    }
    return true;
  }
  if (!(t < times[keys - 1])) {
    const float* v = vals + (keys - 1) * comps;
    if (comps == 4) {
      Copy4(v, out);
      NormalizeQuat(out);
    } else {
      Copy3(v, out);
    }
    return true;
  }
  if (channel.interpolation == AnimInterpolation::kStep) {
    const std::size_t i = FindSegment(times, keys, t);
    const float* v = vals + i * comps;
    if (comps == 4) {
      Copy4(v, out);
      NormalizeQuat(out);
    } else {
      Copy3(v, out);
    }
    return true;
  }
  // Linear.
  const std::size_t i = FindSegment(times, keys, t);
  const float dt = times[i + 1] - times[i];
  float u = 0.0F;
  if (dt > 0.0F && IsFinite(dt)) {
    u = (t - times[i]) / dt;
    if (!(u >= 0.0F && u <= 1.0F) || !IsFinite(u)) {
      u = 0.0F;
    }
  }
  const float* a = vals + i * comps;
  const float* b = vals + (i + 1) * comps;
  if (comps == 4) {
    SlerpQuat(a, b, u, out);
  } else {
    Lerp3(a, b, u, out);
  }
  return true;
}

auto SampleAnimClip(const AnimClip& clip, float t, AnimPose pose) noexcept
    -> std::size_t {
  if (!IsFinite(t) || pose.joint_count == 0 || clip.channels.empty()) {
    return 0;
  }
  if (pose.translation == nullptr && pose.rotation == nullptr &&
      pose.scale == nullptr) {
    return 0;
  }
  AnimPoseReset(pose);
  std::size_t joints = 0;
  for (const AnimChannel& ch : clip.channels) {
    if (ch.target_joint >= pose.joint_count) {
      continue;
    }
    float* dst = nullptr;
    switch (ch.target_path) {
      case AnimTargetPath::kTranslation:
        if (pose.translation == nullptr) {
          continue;
        }
        dst = pose.translation + ch.target_joint * 3U;
        break;
      case AnimTargetPath::kRotation:
        if (pose.rotation == nullptr) {
          continue;
        }
        dst = pose.rotation + ch.target_joint * 4U;
        break;
      case AnimTargetPath::kScale:
        if (pose.scale == nullptr) {
          continue;
        }
        dst = pose.scale + ch.target_joint * 3U;
        break;
      default:
        continue;
    }
    if (!SampleAnimChannel(ch, t, dst)) {
      continue;
    }
    if (pose.set != nullptr && pose.set[ch.target_joint] == 0) {
      pose.set[ch.target_joint] = 1;
      ++joints;
    } else if (pose.set == nullptr) {
      ++joints;
    }
  }
  return joints;
}

}  // namespace engine::assets

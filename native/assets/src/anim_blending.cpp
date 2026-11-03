// anim_blending.cpp — P5-02 two-clip blending; see anim_blending.hpp.
#include "engine/assets/anim_blending.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::assets {
namespace {

auto IsFin(float v) noexcept -> bool { return std::isfinite(v) != 0; }

auto Clamp01(float v) noexcept -> float {
  if (v < 0.0F) {
    return 0.0F;
  }
  if (v > 1.0F) {
    return 1.0F;
  }
  return v;
}

auto MinSize(std::size_t a, std::size_t b, std::size_t c) noexcept
    -> std::size_t {
  std::size_t m = (a < b) ? a : b;
  return (m < c) ? m : c;
}

auto Lerp(float a, float b, float w) noexcept -> float {
  return a + (b - a) * w;
}

// Slerp of unit quats x -> y (x,y,z,w), shortest path, normalized result.
// Zero-length inputs fall back to identity; near-identical falls back to
// normalized lerp. Mirrors the sampler's slerp so blended poses match
// clip-sampled poses at w = 0/1 up to float rounding.
auto SlerpBlend(const float* x, const float* y, float w, float* out) noexcept
    -> void {
  float dot = x[0] * y[0] + x[1] * y[1] + x[2] * y[2] + x[3] * y[3];
  float yy[4] = {y[0], y[1], y[2], y[3]};
  if (dot < 0.0F) {
    dot = -dot;
    yy[0] = -yy[0];
    yy[1] = -yy[1];
    yy[2] = -yy[2];
    yy[3] = -yy[3];
  }
  constexpr float kOneMinusEps = 1.0F - 1e-6F;
  if (dot > kOneMinusEps) {
    for (int k = 0; k < 4; ++k) {
      out[k] = x[k] + (yy[k] - x[k]) * w;
    }
  } else {
    const float theta = std::acos(dot > 1.0F ? 1.0F : dot);
    const float s = std::sin(theta);
    if (!(s > 1e-6F) || !IsFin(s)) {
      out[0] = x[0];
      out[1] = x[1];
      out[2] = x[2];
      out[3] = x[3];
    } else {
      const float wa = std::sin((1.0F - w) * theta) / s;
      const float wb = std::sin(w * theta) / s;
      for (int k = 0; k < 4; ++k) {
        out[k] = x[k] * wa + yy[k] * wb;
      }
    }
  }
  const float n2 =
      out[0] * out[0] + out[1] * out[1] + out[2] * out[2] + out[3] * out[3];
  if (!(n2 > 0.0F) || !IsFin(n2)) {
    out[0] = 0.0F;
    out[1] = 0.0F;
    out[2] = 0.0F;
    out[3] = 1.0F;
    return;
  }
  const float inv = 1.0F / std::sqrt(n2);
  out[0] *= inv;
  out[1] *= inv;
  out[2] *= inv;
  out[3] *= inv;
}

// Column-major mat4 multiply: out = a * b.
auto Mul44(const float* a, const float* b, float* out) noexcept -> void {
  float tmp[16];
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      tmp[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] +
                       a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
    }
  }
  for (int i = 0; i < 16; ++i) {
    out[i] = tmp[i];
  }
}

// Joint-local TRS -> column-major matrix (T * R * S).
// Non-finite scale components fall back to 1; non-finite T/R fall back to
// identity parts, so one bad channel never NaNs the palette.
auto TrsToMat(const float* t, const float* r, const float* s, float* m) noexcept
    -> void {
  float q[4] = {r[0], r[1], r[2], r[3]};
  const float n2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
  if (!(n2 > 0.0F) || !IsFin(n2)) {
    q[0] = 0.0F;
    q[1] = 0.0F;
    q[2] = 0.0F;
    q[3] = 1.0F;
  } else {
    const float inv = 1.0F / std::sqrt(n2);
    q[0] *= inv;
    q[1] *= inv;
    q[2] *= inv;
    q[3] *= inv;
  }
  const float x = q[0];
  const float y = q[1];
  const float z = q[2];
  const float wq = q[3];
  const float sx = IsFin(s[0]) ? s[0] : 1.0F;
  const float sy = IsFin(s[1]) ? s[1] : 1.0F;
  const float sz = IsFin(s[2]) ? s[2] : 1.0F;
  // Rotation columns, scaled.
  m[0] = (1.0F - 2.0F * (y * y + z * z)) * sx;
  m[1] = (2.0F * (x * y + z * wq)) * sx;
  m[2] = (2.0F * (x * z - y * wq)) * sx;
  m[3] = 0.0F;
  m[4] = (2.0F * (x * y - z * wq)) * sy;
  m[5] = (1.0F - 2.0F * (x * x + z * z)) * sy;
  m[6] = (2.0F * (y * z + x * wq)) * sy;
  m[7] = 0.0F;
  m[8] = (2.0F * (x * z + y * wq)) * sz;
  m[9] = (2.0F * (y * z - x * wq)) * sz;
  m[10] = (1.0F - 2.0F * (x * x + y * y)) * sz;
  m[11] = 0.0F;
  m[12] = IsFin(t[0]) ? t[0] : 0.0F;
  m[13] = IsFin(t[1]) ? t[1] : 0.0F;
  m[14] = IsFin(t[2]) ? t[2] : 0.0F;
  m[15] = 1.0F;
}

}  // namespace

auto BlendAnimPoses(const AnimPose& a, const AnimPose& b, float weight,
                    AnimPose out) noexcept -> void {
  if (!IsFin(weight)) {
    return;
  }
  if (out.translation == nullptr || out.rotation == nullptr ||
      out.scale == nullptr) {
    return;
  }
  if (a.translation == nullptr || a.rotation == nullptr ||
      a.scale == nullptr || b.translation == nullptr ||
      b.rotation == nullptr || b.scale == nullptr) {
    return;
  }
  const float w = Clamp01(weight);
  const std::size_t n =
      MinSize(a.joint_count, b.joint_count, out.joint_count);
  if (n == 0) {
    return;
  }
  const std::uint8_t* sa = a.set;
  const std::uint8_t* sb = b.set;
  for (std::size_t j = 0; j < n; ++j) {
    const float* ta = a.translation + j * 3;
    const float* tb = b.translation + j * 3;
    const float* ra = a.rotation + j * 4;
    const float* rb = b.rotation + j * 4;
    const float* sa3 = a.scale + j * 3;
    const float* sb3 = b.scale + j * 3;
    float* to = out.translation + j * 3;
    float* ro = out.rotation + j * 4;
    float* so = out.scale + j * 3;
    for (int k = 0; k < 3; ++k) {
      to[k] = IsFin(ta[k]) && IsFin(tb[k]) ? Lerp(ta[k], tb[k], w)
                                           : (IsFin(ta[k]) ? ta[k] : 0.0F);
      so[k] = IsFin(sa3[k]) && IsFin(sb3[k]) ? Lerp(sa3[k], sb3[k], w)
                                             : (IsFin(sa3[k]) ? sa3[k] : 1.0F);
    }
    SlerpBlend(ra, rb, w, ro);
    if (out.set != nullptr) {
      const std::uint8_t fa = (sa != nullptr) ? sa[j] : 0;
      const std::uint8_t fb = (sb != nullptr) ? sb[j] : 0;
      out.set[j] = static_cast<std::uint8_t>((fa != 0 || fb != 0) ? 1 : 0);
    }
  }
}

auto PoseToJointMatrices(const AnimPose& pose, const std::int32_t* parents,
                         const float* inverse_bind, float* out,
                         std::size_t joint_count) noexcept -> std::size_t {
  if (pose.translation == nullptr || pose.rotation == nullptr ||
      pose.scale == nullptr || parents == nullptr || out == nullptr ||
      joint_count == 0 || pose.joint_count < joint_count) {
    return 0;
  }
  // Local matrices first (out doubles as the local store), then compose
  // parent-first: the importer emits parents before children, and any
  // forward/out-of-range parent index is treated as a root.
  for (std::size_t j = 0; j < joint_count; ++j) {
    TrsToMat(pose.translation + j * 3, pose.rotation + j * 4,
             pose.scale + j * 3, out + j * 16);
  }
  for (std::size_t j = 0; j < joint_count; ++j) {
    const std::int32_t p = parents[j];
    if (p >= 0 && static_cast<std::size_t>(p) < j) {
      float world[16];
      Mul44(out + static_cast<std::size_t>(p) * 16, out + j * 16, world);
      for (int i = 0; i < 16; ++i) {
        out[j * 16 + static_cast<std::size_t>(i)] = world[i];
      }
    }
    if (inverse_bind != nullptr) {
      float skinned[16];
      Mul44(out + j * 16, inverse_bind + j * 16, skinned);
      for (int i = 0; i < 16; ++i) {
        out[j * 16 + static_cast<std::size_t>(i)] = skinned[i];
      }
    }
  }
  return joint_count;
}

}  // namespace engine::assets

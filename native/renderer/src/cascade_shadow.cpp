// cascade_shadow.cpp — P4-03 cascaded shadow map CPU reference.
//
// Split math mirrors ShadowMapPlan::init (lambda=0.5 log/uniform blend)
// so the two plans agree on ranges. Ortho fit: frustum-slice corners in
// world space -> light-space AABB -> snapped center.
//
// Texel snapping (the stability mechanism): the light-space ortho center
// is quantized to multiples of texel_world, so sub-texel camera motion
// keeps the SAME texels covering the SAME world points — no shimmer.
// A 1cm nudge moves the unsnapped center by 1cm but the snapped center
// only when it crosses a texel boundary; cascade-0 texels here are ~2mm
// (4m frustum half-width / 1024), so the acceptance pair (1cm apart)
// exercises a real multi-texel shift while staying within the <0.5%
// shadow-region budget because only texels near depth discontinuities
// can flip.
#include "engine/renderer/cascade_shadow.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "engine/renderer/gi_shadow.hpp"

namespace engine::renderer {
namespace {

// Must stay in lockstep with gi_shadow.hpp (single source of truth for
// the atlas layout); pinned at namespace scope so a drift fails the
// build instead of silently changing golden bytes.
static_assert(kCsmCascadeCount == kShadowCascades, "cascade count drift");
static_assert(kCsmTileRes == kShadowTileRes, "tile res drift");
static_assert(kShadowPcfTaps == 9, "PCF kernel is 3x3");

struct Vec3 {
  float x{0.0f};
  float y{0.0f};
  float z{0.0f};
};

inline auto vadd(Vec3 a, Vec3 b) noexcept -> Vec3 {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline auto vsub(Vec3 a, Vec3 b) noexcept -> Vec3 {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline auto vmul(Vec3 a, float s) noexcept -> Vec3 {
  return {a.x * s, a.y * s, a.z * s};
}
inline auto vdot(Vec3 a, Vec3 b) noexcept -> float {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline auto vcross(Vec3 a, Vec3 b) noexcept -> Vec3 {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x};
}
inline auto vlen(Vec3 a) noexcept -> float {
  return std::sqrt(vdot(a, a));
}
inline auto vnorm(Vec3 a) noexcept -> Vec3 {
  const float l = vlen(a);
  if (!(l > 1e-9f)) {
    return {0.0f, 0.0f, 0.0f};
  }
  return vmul(a, 1.0f / l);
}

inline auto to_vec(const float a[3]) noexcept -> Vec3 {
  return {a[0], a[1], a[2]};
}

}  // namespace

auto CascadeShadowMap::init(const CsmCameraBasis& camera,
                             const CsmDirectionalLight& light,
                             float texel_world_base) noexcept -> Status {
  if (!(camera.far_plane > camera.near_plane) || !(camera.near_plane > 0.0f) ||
      !(texel_world_base > 0.0f) || !(camera.fov_y_rad > 0.0f) ||
      !(camera.aspect > 0.0f)) {
    return MakeFail(StatusCode::kInvalidArgument,
                    "csm: bad camera/texel range");
  }
  Vec3 dir = to_vec(light.direction);
  if (!(vlen(dir) > 1e-6f)) {
    return MakeFail(StatusCode::kInvalidArgument, "csm: zero light dir");
  }
  dir = vnorm(dir);

  const Vec3 eye = to_vec(camera.position);
  Vec3 fwd = vnorm(to_vec(camera.forward));
  if (!(vlen(fwd) > 1e-6f)) {
    return MakeFail(StatusCode::kInvalidArgument, "csm: zero forward");
  }
  Vec3 up = to_vec(camera.up);
  Vec3 right = vcross(fwd, up);
  if (!(vlen(right) > 1e-6f)) {
    return MakeFail(StatusCode::kInvalidArgument, "csm: forward||up");
  }
  right = vnorm(right);
  up = vnorm(vcross(right, fwd));

  // Light basis: light looks along -dir (from the sun toward the scene).
  // Pick a stable side vector (same trick as lookAt singularity guard).
  Vec3 lz = vmul(dir, -1.0f);  // light forward (from sun to scene)
  Vec3 lz_n = vnorm(lz);
  Vec3 world_up{0.0f, 1.0f, 0.0f};
  Vec3 lx = vcross(world_up, lz_n);
  if (!(vlen(lx) > 1e-4f)) {
    lx = {1.0f, 0.0f, 0.0f};
  }
  lx = vnorm(lx);
  const Vec3 kLy = vnorm(vcross(lz_n, lx));

  // Cascade splits: same practical-SDSM blend as ShadowMapPlan (lambda
  // 0.5) so ranges match the atlas owner.
  constexpr float kLambda = 0.5f;
  const float view_near = camera.near_plane;
  const float view_far = camera.far_plane;
  float prev = view_near;
  const float tan_half = std::tan(camera.fov_y_rad * 0.5f);
  for (std::uint32_t i = 0; i < kCsmCascadeCount; ++i) {
    const float f = static_cast<float>(i + 1) /
                    static_cast<float>(kCsmCascadeCount);
    const float log_split = view_near * std::pow(view_far / view_near, f);
    const float uni_split = view_near + (view_far - view_near) * f;
    const float split = kLambda * log_split + (1.0f - kLambda) * uni_split;

    // Frustum slice corners (8) in world space: near/far planes of the
    // slice, half-height = tan(fov/2)*dist, half-width = aspect*hh.
    float max_r = 0.0f;
    for (std::uint32_t c = 0; c < 8; ++c) {
      const float d = ((c & 4u) != 0u) ? split : prev;
      const float sx = (((c & 1u) != 0u) ? 1.0f : -1.0f);
      const float sy = (((c & 2u) != 0u) ? 1.0f : -1.0f);
      const float hh = tan_half * d;
      const float hw = camera.aspect * hh;
      Vec3 p = vadd(eye, vmul(fwd, d));
      p = vadd(p, vmul(right, sx * hw));
      p = vadd(p, vmul(up, sy * hh));
      // Light-space coords of the corner.
      const float lx_c = vdot(vsub(p, eye), lx);
      const float ly_c = vdot(vsub(p, eye), kLy);
      const float ax = (lx_c >= 0.0f) ? lx_c : -lx_c;
      const float ay = (ly_c >= 0.0f) ? ly_c : -ly_c;
      if (ax > max_r) {
        max_r = ax;
      }
      if (ay > max_r) {
        max_r = ay;
      }
      (void)lz_n;
    }

    // Texel density halves per cascade outward (same growth as
    // ShadowMapPlan: cascade i texels are 2^i * base).
    float tw = texel_world_base;
    for (std::uint32_t l = 0; l < i; ++l) {
      tw *= 2.0f;
    }
    // Ortho half-extent covers the slice radius, snapped UP to a whole
    // texel count so the box is an integer number of texels wide.
    float half_ext = max_r;
    if (half_ext < tw) {
      half_ext = tw;
    }
    const float half_texels = std::ceil(half_ext / tw);
    half_ext = half_texels * tw;

    // Snap the ortho CENTER to texel increments: project the slice
    // centroid (eye + fwd * mid) into light space, quantize, rebuild.
    const float mid = (prev + split) * 0.5f;
    Vec3 center = vadd(eye, vmul(fwd, mid));
    Vec3 rel = vsub(center, eye);
    const float cx = vdot(rel, lx);
    const float cy = vdot(rel, kLy);
    const float cz = vdot(rel, lz_n);
    const float sx = std::round(cx / tw) * tw;
    const float sy = std::round(cy / tw) * tw;
    center = vadd(eye, vadd(vadd(vmul(lx, sx), vmul(kLy, sy)),
                            vmul(lz_n, cz)));

    // Row-major light view-proj: ortho (half_ext, near/far along light
    // axis sized to the slice) * lookAt(center). The GLSL mirror uses
    // the same matrix uploaded as a 4x4 uniform, so the CPU snap point
    // (sx, sy) and the GPU sample the same texels.
    //
    // View rows: lx, kLy, -lz_n (camera looks along -Z; light forward
    // lz_n points from sun to scene, so the view row is -lz).
    // Keep translation in the matrix (center-relative).
    const float depth_range = (split - prev) + 4.0f * half_ext + 2.0f;
    const float inv_ex = (half_ext > 1e-9f) ? 1.0f / half_ext : 1.0f;
    const float inv_d =
        (depth_range > 1e-9f) ? 2.0f / depth_range : 1.0f;
    // Row 0: lx * inv_ex, translation -dot(lx, center) * inv_ex.
    // Row 1: kLy * inv_ex.
    // Row 2: -lz_n * inv_d mapped so slice maps to [-1, 1].
    float* m = cascades_[i].view_proj;
    const float tcx = vdot(to_vec(light.direction), to_vec(light.direction));
    (void)tcx;
    m[0] = lx.x * inv_ex;
    m[1] = lx.y * inv_ex;
    m[2] = lx.z * inv_ex;
    m[3] = -vdot(lx, center) * inv_ex;
    m[4] = kLy.x * inv_ex;
    m[5] = kLy.y * inv_ex;
    m[6] = kLy.z * inv_ex;
    m[7] = -vdot(kLy, center) * inv_ex;
    m[8] = -lz_n.x * inv_d;
    m[9] = -lz_n.y * inv_d;
    m[10] = -lz_n.z * inv_d;
    // Light-space z of center along lz: zc; map zc-depth/2..zc+depth/2
    // to -1..1: m[11] = -(zc*inv_d) - offset so near maps to -1.
    const float zc = vdot(vsub(center, eye), lz_n);
    m[11] = -(zc * inv_d) + (depth_range * 0.5f * inv_d) - 1.0f;
    m[12] = 0.0f;
    m[13] = 0.0f;
    m[14] = 0.0f;
    m[15] = 1.0f;

    cascades_[i].near_dist = prev;
    cascades_[i].far_dist = split;
    cascades_[i].texel_world = tw;
    cascades_[i].tile_x = (i % 2u) * kCsmTileRes;
    cascades_[i].tile_y = (i / 2u) * kCsmTileRes;
    prev = split;
  }
  light_ = light;
  ready_ = true;
  return MakeOk();
}

auto CascadeShadowMap::cascade(std::uint32_t i) const noexcept
    -> const CsmCascadeFit& {
  static const CsmCascadeFit kEmpty{};
  if (!ready_ || i >= kCsmCascadeCount) {
    return kEmpty;
  }
  return cascades_[i];
}

auto CascadeShadowMap::cascade_index(float view_depth) const noexcept
    -> std::uint32_t {
  if (!ready_) {
    return 0;
  }
  for (std::uint32_t i = 0; i < kCsmCascadeCount; ++i) {
    if (view_depth <= cascades_[i].far_dist) {
      return i;
    }
  }
  return kCsmCascadeCount - 1;
}

auto CascadeShadowMap::ortho_half_extent(std::uint32_t i) const noexcept
    -> float {
  if (!ready_ || i >= kCsmCascadeCount) {
    return 0.0f;
  }
  return cascades_[i].texel_world * static_cast<float>(kCsmTileRes) * 0.5f;
}

auto CascadeShadowMap::pcf_shadow(const float* depth_samples,
                                   float receiver_depth,
                                   float bias) noexcept -> float {
  if (depth_samples == nullptr) {
    return 1.0f;
  }
  std::uint32_t lit = 0;
  for (std::uint32_t k = 0; k < 9; ++k) {
    // Lit when the receiver is in front of (nearer than) the stored
    // occluder depth + bias. Depth convention: [0,1] light-space,
    // 1.0 = far plane (cleared).
    if (receiver_depth <= depth_samples[k] + bias) {
      ++lit;
    }
  }
  return static_cast<float>(lit) / 9.0f;
}

auto CascadeShadowMap::shade_factor(float view_depth,
                                     const float* depth_samples,
                                     float receiver_depth,
                                     float bias) noexcept -> float {
  if (!light_.shadows_enabled) {
    return 1.0f;
  }
  (void)view_depth;
  return pcf_shadow(depth_samples, receiver_depth, bias);
}

auto CascadeShadowMap::mesh_shadow_factor(float wx, float wy,
                                           float wz) const noexcept -> float {
  // Term-for-term mirror of mesh.frag mesh_shadow.
  if (!light_.shadows_enabled) {
    return 1.0f;
  }
  float az = (wz >= 0.0f) ? wz : -wz;
  if (az > 0.05f) {
    return 1.0f;
  }
  // normalize(vec2(0.35, -0.25)): len = sqrt(0.1225+0.0625) = sqrt(0.185).
  constexpr float kLen = 0.43011626f;  // sqrt(0.185f)
  const float sx = 0.35f / kLen;
  const float sy = -0.25f / kLen;
  const float rx = wx - 0.0f;
  const float ry = wy - 0.6f;
  const float along = rx * (-sx) + ry * (-sy);
  const float across_raw = rx * (-sy) + ry * (sx);
  const float across = (across_raw >= 0.0f) ? across_raw : -across_raw;
  if (!(along >= 0.0f) || !(along <= 4.0f) || !(across <= 0.75f)) {
    return 1.0f;
  }
  float edge = (0.75f - across) / 0.25f;
  if (edge < 0.0f) {
    edge = 0.0f;
  }
  if (edge > 1.0f) {
    edge = 1.0f;
  }
  const float tail = 1.0f - along / 4.0f;
  return 0.35f + 0.65f * (1.0f - edge * tail);
}

}  // namespace engine::renderer

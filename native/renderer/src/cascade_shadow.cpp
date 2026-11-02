// cascade_shadow.cpp — P4-03 cascaded shadow map CPU reference.
//
// Split math mirrors ShadowMapPlan::init (lambda=0.5 log/uniform blend)
// so the two plans agree on ranges. Ortho fit: frustum-slice corners in
// world space -> light-space AABB -> snapped center.
//
// Texel snapping (the stability mechanism): the box half-extent covers
// the whole slice, texel_world = 2*half_extent/1024 is the real texel
// size, and the ortho center is quantized to multiples of it in absolute
// light space. Sub-texel camera motion therefore keeps the SAME texels on
// the SAME world points (no shimmer); a 1cm nudge moves the snapped
// center by whole texels only, so just texels at depth edges can flip.
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

  // Light basis: light.direction is the direction light travels (from the
  // sun into the scene); lz_n = -dir points back toward the sun.
  // Pick a stable side vector (same trick as lookAt singularity guard).
  Vec3 lz = vmul(dir, -1.0f);  // toward the sun (dir is travel direction)
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
    // Bounding sphere of the slice around its centroid: rotation
    // invariant, so turning the camera never changes the box size (no
    // texel-size shimmer) and the box always holds all 8 corners.
    const float mid = (prev + split) * 0.5f;
    const Vec3 slice_center = vadd(eye, vmul(fwd, mid));
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
      const float r = vlen(vsub(p, slice_center));
      if (r > max_r) {
        max_r = r;
      }
    }

    // Coverage first: the ortho box must hold the whole slice, so its
    // half-extent is the slice radius, floored at the nominal density
    // (cascade i nominal texel = 2^i * base, same growth as
    // ShadowMapPlan). The REAL texel is then 2*half_ext/tile_res, and
    // that is the size used for snapping and reported as texel_world.
    float nominal_tw = texel_world_base;
    for (std::uint32_t l = 0; l < i; ++l) {
      nominal_tw *= 2.0f;
    }
    constexpr float kHalfRes = static_cast<float>(kCsmTileRes) * 0.5f;
    // One texel of slack: snapping moves the center by <= half a texel
    // per axis, so half_ext - tw >= radius keeps the slice covered.
    float half_ext = max_r * kHalfRes / (kHalfRes - 1.0f);
    if (half_ext < nominal_tw * kHalfRes) {
      half_ext = nominal_tw * kHalfRes;
    }
    const float tw = half_ext / kHalfRes;  // == 2*half_ext/tile_res

    // Snap the ortho CENTER to whole texels in ABSOLUTE light space (not
    // relative to the eye), so a sub-texel camera move leaves every
    // texel on the same world point: project the slice centroid onto
    // the light axes, quantize x/y, rebuild.
    Vec3 center = slice_center;
    const float cx = vdot(center, lx);
    const float cy = vdot(center, kLy);
    const float cz = vdot(center, lz_n);
    const float sx = std::round(cx / tw) * tw;
    const float sy = std::round(cy / tw) * tw;
    center = vadd(vadd(vmul(lx, sx), vmul(kLy, sy)), vmul(lz_n, cz));

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
    // lz_n points from the scene toward the sun, so s = dot(lz_n, p)
    // grows toward the sun. Row 2 is -lz_n*inv_d; with sc = dot(lz_n,
    // center) (absolute), cz = (sc - s)*inv_d maps the slab
    // s in [sc - D/2, sc + D/2] to [+1, -1]: nearest the sun -> depth 0,
    // farthest -> depth 1 (the atlas clear / "far" value).
    m[11] = vdot(lz_n, center) * inv_d;
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

}  // namespace engine::renderer

// gi_baker.cpp — P4-07 part 2: SDF-grid Jacobi probe baker.
//
// Pipeline: AddBricks voxelizes each brick's shell (|sdf| <= band) into
// the shared SDF grid (min-composite) + albedo grid. Bake() runs the
// Jacobi solve: pass 1 gathers direct light only (sphere-traced
// visibility + shadow ray, gradient normals), later passes add the
// trilinear-sampled previous field (one more bounce order each).
#include "engine/renderer/gi_baker.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "engine/renderer/sdf_brick_builder.hpp"

namespace engine::renderer {

namespace {

// Brick texel value v (int8): dequantize to voxel units — the builder
// stores sd/ts clamped to +-kSdfQuantRangeVox over +-127. Shell test
// against kSdfBandVox in real units, never raw texel counts.
inline auto TexelSdfVox(float v) noexcept -> float {
  return v * (kSdfQuantRangeVox / 127.0f);
}

inline auto TexelSolid(float v) noexcept -> bool {
  return TexelSdfVox(v) <= kSdfBandVox;
}

inline auto Idx3(std::uint32_t x, std::uint32_t y, std::uint32_t z,
                 std::uint32_t res) noexcept -> std::size_t {
  return (static_cast<std::size_t>(z) * res + y) * res + x;
}

inline auto FiboDir(std::uint32_t i, std::uint32_t n, float out[3]) noexcept
    -> void {
  constexpr float kGolden = 2.399963229728653f;
  const float z = 1.0f - 2.0f * (static_cast<float>(i) + 0.5f) /
                               static_cast<float>(n);
  const float rr = std::sqrt(std::max(0.0f, 1.0f - z * z));
  const float ph = kGolden * static_cast<float>(i);
  out[0] = rr * std::cos(ph);
  out[1] = rr * std::sin(ph);
  out[2] = z;
}

}  // namespace

auto GiBaker::configure(const GiBakeParams& params) noexcept -> Status {
  if (params.box_size[0] <= 0.0f || params.box_size[1] <= 0.0f ||
      params.box_size[2] <= 0.0f || params.occ_res == 0 ||
      params.probe_res == 0 || params.dirs == 0 || params.passes <= 0) {
    return MakeFail(StatusCode::kInvalidArgument, "baker: bad params");
  }
  params_ = params;
  const std::size_t n = static_cast<std::size_t>(params.occ_res) *
                          params.occ_res * params.occ_res;
  // Bounded far field (box diagonal): 1e9 would poison the trilinear
  // samples near the shell into step sizes that jump over it.
  far_ = std::sqrt(params.box_size[0] * params.box_size[0] +
                   params.box_size[1] * params.box_size[1] +
                   params.box_size[2] * params.box_size[2]);
  sdf_.assign(n, far_);
  albedo_.assign(n * 3, 0.8f);
  solid_voxels_ = 0;
  configured_ = true;
  return MakeOk();
}

auto GiBaker::add_bricks(const SdfBrick* bricks, std::uint32_t brick_count,
                         const float* world, const float* albedo) noexcept
    -> Status {
  if (!configured_) {
    return MakeFail(StatusCode::kInvalidArgument, "baker: not configured");
  }
  if ((bricks == nullptr && brick_count > 0) || world == nullptr ||
      albedo == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "baker: null brick args");
  }
  const float* mn = params_.box_min;
  const float* sz = params_.box_size;
  const float res = static_cast<float>(params_.occ_res);
  const float vx = sz[0] / res, vy = sz[1] / res, vz = sz[2] / res;
  for (std::uint32_t b = 0; b < brick_count; ++b) {
    const SdfBrick& br = bricks[b];
    // Brick corner + axes in world space (column-major matrix).
    float corner[3] = {
        world[0] * br.origin[0] + world[4] * br.origin[1] +
            world[8] * br.origin[2] + world[12],
        world[1] * br.origin[0] + world[5] * br.origin[1] +
            world[9] * br.origin[2] + world[13],
        world[2] * br.origin[0] + world[6] * br.origin[1] +
            world[10] * br.origin[2] + world[14]};
    float ax[3] = {world[0] * br.texel_size, world[1] * br.texel_size,
                   world[2] * br.texel_size};
    float ay[3] = {world[4] * br.texel_size, world[5] * br.texel_size,
                   world[6] * br.texel_size};
    float az[3] = {world[8] * br.texel_size, world[9] * br.texel_size,
                   world[10] * br.texel_size};
    for (std::uint32_t z = 0; z < kSdfBrickRes; ++z) {
      for (std::uint32_t y = 0; y < kSdfBrickRes; ++y) {
        for (std::uint32_t x = 0; x < kSdfBrickRes; ++x) {
          const float v = static_cast<float>(
              br.texels[(z * kSdfBrickRes + y) * kSdfBrickRes + x]);
          if (!TexelSolid(v)) {
            continue;
          }
          // Texel center in world space.
          const float fx = static_cast<float>(x) + 0.5f;
          const float fy = static_cast<float>(y) + 0.5f;
          const float fz = static_cast<float>(z) + 0.5f;
          float p[3] = {
              corner[0] + ax[0] * fx + ay[0] * fy + az[0] * fz,
              corner[1] + ax[1] * fx + ay[1] * fy + az[1] * fz,
              corner[2] + ax[2] * fx + ay[2] * fy + az[2] * fz};
          const float gx = (p[0] - mn[0]) / vx - 0.5f;
          const float gy = (p[1] - mn[1]) / vy - 0.5f;
          const float gz = (p[2] - mn[2]) / vz - 0.5f;
          const int ix = static_cast<int>(gx + 0.5f);
          const int iy = static_cast<int>(gy + 0.5f);
          const int iz = static_cast<int>(gz + 0.5f);
          if (ix < 0 || iy < 0 || iz < 0 ||
              ix >= static_cast<int>(params_.occ_res) ||
              iy >= static_cast<int>(params_.occ_res) ||
              iz >= static_cast<int>(params_.occ_res)) {
            continue;
          }
          const std::size_t o = Idx3(static_cast<std::uint32_t>(ix),
                                     static_cast<std::uint32_t>(iy),
                                     static_cast<std::uint32_t>(iz),
                                     params_.occ_res);
          // Min-composite SDF in world units (dequantized shell value).
          const float sdf_v = TexelSdfVox(v) * br.texel_size;
          if (sdf_v < sdf_[o]) {
            if (sdf_[o] >= far_) {
              ++solid_voxels_;
            }
            sdf_[o] = sdf_v;
            albedo_[o * 3] = albedo[0];
            albedo_[o * 3 + 1] = albedo[1];
            albedo_[o * 3 + 2] = albedo[2];
          }
        }
      }
    }
  }
  return MakeOk();
}

auto GiBaker::sdf_at(float x, float y, float z) const noexcept -> float {
  const float* mn = params_.box_min;
  const float* sz = params_.box_size;
  const float res = static_cast<float>(params_.occ_res);
  const float gx = (x - mn[0]) / sz[0] * res - 0.5f;
  const float gy = (y - mn[1]) / sz[1] * res - 0.5f;
  const float gz = (z - mn[2]) / sz[2] * res - 0.5f;
  int x0 = static_cast<int>(std::floor(gx));
  int y0 = static_cast<int>(std::floor(gy));
  int z0 = static_cast<int>(std::floor(gz));
  const float fx = gx - static_cast<float>(x0);
  const float fy = gy - static_cast<float>(y0);
  const float fz = gz - static_cast<float>(z0);
  float acc = 0.0f;
  for (int dz = 0; dz < 2; ++dz) {
    for (int dy = 0; dy < 2; ++dy) {
      for (int dx = 0; dx < 2; ++dx) {
        int ix = x0 + dx, iy = y0 + dy, iz = z0 + dz;
        float s = far_;
        if (ix >= 0 && iy >= 0 && iz >= 0 &&
            ix < static_cast<int>(params_.occ_res) &&
            iy < static_cast<int>(params_.occ_res) &&
            iz < static_cast<int>(params_.occ_res)) {
          s = sdf_[Idx3(static_cast<std::uint32_t>(ix),
                        static_cast<std::uint32_t>(iy),
                        static_cast<std::uint32_t>(iz), params_.occ_res)];
        }
        const float w = (dx == 0 ? 1.0f - fx : fx) *
                        (dy == 0 ? 1.0f - fy : fy) *
                        (dz == 0 ? 1.0f - fz : fz);
        acc += s * w;
      }
    }
  }
  return acc;
}

// Sphere-trace the SDF grid; returns hit t (<= max_t) or -1.
auto TraceGrid(const float* sdf, const float* box_min, const float* box_size,
               std::uint32_t res, const float p[3], const float d[3],
               float max_t, float min_t) noexcept -> float {
  const float far =
      std::sqrt(box_size[0] * box_size[0] + box_size[1] * box_size[1] +
                box_size[2] * box_size[2]);
  // Trilinear SDF sample (local copy to stay header-free).
  const float res_f = static_cast<float>(res);
  const std::size_t res_n = static_cast<std::size_t>(res);
  const int res_i = static_cast<int>(res);
  auto at = [&](float x, float y, float z) {
    const float gx = (x - box_min[0]) / box_size[0] * res_f - 0.5f;
    const float gy = (y - box_min[1]) / box_size[1] * res_f - 0.5f;
    const float gz = (z - box_min[2]) / box_size[2] * res_f - 0.5f;
    const int x0 = static_cast<int>(std::floor(gx));
    const int y0 = static_cast<int>(std::floor(gy));
    const int z0 = static_cast<int>(std::floor(gz));
    const float fx = gx - static_cast<float>(x0);
    const float fy = gy - static_cast<float>(y0);
    const float fz = gz - static_cast<float>(z0);
    float acc = 0.0f;
    for (int dz = 0; dz < 2; ++dz)
      for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx) {
          const int ix = x0 + dx, iy = y0 + dy, iz = z0 + dz;
          float s = far;
          if (ix >= 0 && iy >= 0 && iz >= 0 && ix < res_i && iy < res_i &&
              iz < res_i) {
            s = sdf[(static_cast<std::size_t>(iz) * res_n +
                     static_cast<std::size_t>(iy)) *
                        res_n +
                    static_cast<std::size_t>(ix)];
          }
          acc += s * (dx != 0 ? fx : 1.0f - fx) *
                 (dy != 0 ? fy : 1.0f - fy) * (dz != 0 ? fz : 1.0f - fz);
        }
    return acc;
  };
  float t = 0.01f;
  // Clamped sphere trace: the grid SDF is a min-composite of shells, not
  // a true distance field, so unbounded steps overshoot thin shells.
  // Clamp to 3/4 voxel: guaranteed progress, never skips a shell voxel.
  const float vmax =
      std::max(box_size[0], std::max(box_size[1], box_size[2])) /
      static_cast<float>(res);
  const float max_step = vmax * 0.75f;
  // Hit epsilon: sampled shell values bottom out near half a texel, not
  // zero — trigger within half a voxel, and on any negative sample
  // (inside solid).
  const float hit_eps = vmax * 0.5f;
  for (int s = 0; s < 64; ++s) {
    const float p0 = p[0] + d[0] * t;
    const float p1 = p[1] + d[1] * t;
    const float p2 = p[2] + d[2] * t;
    if (p0 < box_min[0] || p0 > box_min[0] + box_size[0] ||
        p1 < box_min[1] || p1 > box_min[1] + box_size[1] ||
        p2 < box_min[2] || p2 > box_min[2] + box_size[2]) {
      return -1.0f;  // escaped: sky
    }
    const float dist = at(p0, p1, p2);
    // min_t skips the shell the ray starts inside (self-shadow bias).
    if (dist < hit_eps && t > min_t) {
      return t;
    }
    t += std::min(std::max(dist * 0.9f, 0.02f), max_step);
    if (t > max_t) {
      return -1.0f;
    }
  }
  return -1.0f;
}

auto GiBaker::gather(const float* prev, const GiBakeLight& light,
                     float* e) const noexcept -> void {
  const float* mn = params_.box_min;
  const float* sz = params_.box_size;
  const float res_f = static_cast<float>(params_.occ_res);
  const float vx = sz[0] / res_f;
  const std::uint32_t pr = params_.probe_res;
  const float pr_f = static_cast<float>(pr);
  const float pw[3] = {sz[0] / pr_f, sz[1] / pr_f, sz[2] / pr_f};
  const float dirs_f = static_cast<float>(params_.dirs);
  const int res_i = static_cast<int>(params_.occ_res);
  const int pr_i = static_cast<int>(pr);
  const std::size_t pr_n = static_cast<std::size_t>(pr);
  // Light direction toward scene (already normalized by contract).
  for (std::uint32_t z = 0; z < pr; ++z) {
    for (std::uint32_t y = 0; y < pr; ++y) {
      for (std::uint32_t x = 0; x < pr; ++x) {
        float p[3] = {mn[0] + (static_cast<float>(x) + 0.5f) * pw[0],
                      mn[1] + (static_cast<float>(y) + 0.5f) * pw[1],
                      mn[2] + (static_cast<float>(z) + 0.5f) * pw[2]};
        float acc[3] = {0, 0, 0};
        for (std::uint32_t k = 0; k < params_.dirs; ++k) {
          float d[3];
          FiboDir(k, params_.dirs, d);
          // Gather rays start in open air (probe centers): min_t = 0.
          const float t = TraceGrid(sdf_.data(), mn, sz, params_.occ_res,
                                    p, d, 1e9f, 0.0f);
          if (t < 0.0f) {
            continue;  // sky: shade sky term covers it
          }
          float q[3] = {p[0] + d[0] * t, p[1] + d[1] * t, p[2] + d[2] * t};
          // Gradient normal (central differences, world units).
          const float e2 = vx * 0.5f;
          float n[3] = {
              sdf_at(q[0] + e2, q[1], q[2]) - sdf_at(q[0] - e2, q[1], q[2]),
              sdf_at(q[0], q[1] + e2, q[2]) - sdf_at(q[0], q[1] - e2, q[2]),
              sdf_at(q[0], q[1], q[2] + e2) - sdf_at(q[0], q[1], q[2] - e2)};
          const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
          if (nl < 1e-9f) {
            continue;
          }
          n[0] /= nl;
          n[1] /= nl;
          n[2] /= nl;
          // Two-sided: open quads have arbitrary SDF sign (parity ray),
          // so face the normal toward the gather ray. The surface as
          // seen from the probe is the lit one.
          if (n[0] * d[0] + n[1] * d[1] + n[2] * d[2] > 0.0f) {
            n[0] = -n[0];
            n[1] = -n[1];
            n[2] = -n[2];
          }
          const float ndl =
              std::max(0.0f, -(n[0] * light.direction[0] +
                               n[1] * light.direction[1] +
                               n[2] * light.direction[2]));
          float direct = 0.0f;
          if (ndl > 0.0f) {
            // Shadow ray toward the light (directional: far target).
            // min_t = one voxel: the ray starts inside the hit shell.
            float to_l[3] = {-light.direction[0], -light.direction[1],
                             -light.direction[2]};
            const float st = TraceGrid(sdf_.data(), mn, sz, params_.occ_res,
                                       q, to_l, 1e9f, vx);
            if (st < 0.0f) {
              direct = ndl * light.intensity;
            }
          }
          // Hit albedo (nearest voxel).
          const int ix =
              static_cast<int>((q[0] - mn[0]) / sz[0] * res_f);
          const int iy =
              static_cast<int>((q[1] - mn[1]) / sz[1] * res_f);
          const int iz =
              static_cast<int>((q[2] - mn[2]) / sz[2] * res_f);
          float alb[3] = {0.8f, 0.8f, 0.8f};
          if (ix >= 0 && iy >= 0 && iz >= 0 && ix < res_i && iy < res_i &&
              iz < res_i) {
            const std::size_t o = Idx3(static_cast<std::uint32_t>(ix),
                                       static_cast<std::uint32_t>(iy),
                                       static_cast<std::uint32_t>(iz),
                                       params_.occ_res);
            alb[0] = albedo_[o * 3];
            alb[1] = albedo_[o * 3 + 1];
            alb[2] = albedo_[o * 3 + 2];
          }
          float bounced[3] = {0.0f, 0.0f, 0.0f};
          if (prev != nullptr) {
            // Trilinear sample of previous field at q.
            const float gx = (q[0] - mn[0]) / sz[0] * pr_f - 0.5f;
            const float gy = (q[1] - mn[1]) / sz[1] * pr_f - 0.5f;
            const float gz = (q[2] - mn[2]) / sz[2] * pr_f - 0.5f;
            const int x0 = static_cast<int>(std::floor(gx));
            const int y0 = static_cast<int>(std::floor(gy));
            const int z0 = static_cast<int>(std::floor(gz));
            const float fx = gx - static_cast<float>(x0);
            const float fy = gy - static_cast<float>(y0);
            const float fz = gz - static_cast<float>(z0);
            for (int dz = 0; dz < 2; ++dz) {
              for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                  const int cix = std::min(std::max(x0 + dx, 0), pr_i - 1);
                  const int ciy = std::min(std::max(y0 + dy, 0), pr_i - 1);
                  const int ciz = std::min(std::max(z0 + dz, 0), pr_i - 1);
                  const float wgt = (dx != 0 ? fx : 1.0f - fx) *
                                    (dy != 0 ? fy : 1.0f - fy) *
                                    (dz != 0 ? fz : 1.0f - fz);
                  const std::size_t o =
                      (static_cast<std::size_t>(ciz) * pr_n +
                       static_cast<std::size_t>(ciy)) *
                          pr_n +
                      static_cast<std::size_t>(cix);
                  bounced[0] += prev[o * 3] * wgt;
                  bounced[1] += prev[o * 3 + 1] * wgt;
                  bounced[2] += prev[o * 3 + 2] * wgt;
                }
              }
            }
          }
          const float w = 1.0f / dirs_f;
          acc[0] += alb[0] * (direct * light.color[0] + bounced[0]) * w;
          acc[1] += alb[1] * (direct * light.color[1] + bounced[1]) * w;
          acc[2] += alb[2] * (direct * light.color[2] + bounced[2]) * w;
        }
        const std::size_t o = ((std::size_t)z * pr + y) * pr + x;
        e[o * 3] = acc[0] * params_.gain;
        e[o * 3 + 1] = acc[1] * params_.gain;
        e[o * 3 + 2] = acc[2] * params_.gain;
      }
    }
  }
}

auto GiBaker::bake(const GiBakeLight& light, float* probes_out) noexcept
    -> Status {
  if (!configured_) {
    return MakeFail(StatusCode::kInvalidArgument, "baker: not configured");
  }
  if (probes_out == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "baker: null out");
  }
  const std::size_t n = static_cast<std::size_t>(params_.probe_res) *
                        params_.probe_res * params_.probe_res * 3;
  std::vector<float> a(n, 0.0f), b(n, 0.0f);
  const float* prev = nullptr;
  for (int pass = 0; pass < params_.passes; ++pass) {
    std::vector<float>& cur = (pass % 2 == 0) ? a : b;
    gather(prev, light, cur.data());
    prev = cur.data();
  }
  const float* last =
      ((params_.passes - 1) % 2 == 0) ? a.data() : b.data();
  std::memcpy(probes_out, last, n * sizeof(float));
  return MakeOk();
}

}  // namespace engine::renderer

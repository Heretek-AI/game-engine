// sdf_brick_builder.cpp — M3-04 AC-1: sparse SDF brick construction.
//
// Brute-force but exact: per candidate brick texel, unsigned distance =
// min over all triangles of point-triangle distance; sign = parity ray
// cast along +X. Bricks whose minimum |distance| is within the surface
// band are materialized with int8 quantization; everything else stays
// sparse. Deterministic (fixed triangle order, no threads inside build —
// the caller fans whole meshes across worker threads).
#include "engine/renderer/sdf_brick_builder.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace engine::renderer {
namespace {

inline auto dot3(const float a[3], const float b[3]) noexcept -> float {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

inline auto sub3(const float a[3], const float b[3], float o[3]) noexcept
    -> void {
  o[0] = a[0] - b[0];
  o[1] = a[1] - b[1];
  o[2] = a[2] - b[2];
}

}  // namespace

auto MeshSdfBuilder::point_triangle_dist(const float p[3], const float a[3],
                                         const float b[3],
                                         const float c[3]) noexcept -> float {
  // Ericson 5.1: closest point via barycentric region tests.
  float ab[3];
  float ac[3];
  float ap[3];
  sub3(b, a, ab);
  sub3(c, a, ac);
  sub3(p, a, ap);
  const float d1 = dot3(ab, ap);
  const float d2 = dot3(ac, ap);
  if (d1 <= 0.0f && d2 <= 0.0f) {  // vertex A region
    float d[3];
    sub3(p, a, d);
    return std::sqrt(dot3(d, d));
  }
  float bp[3];
  sub3(p, b, bp);
  const float d3 = dot3(ab, bp);
  const float d4 = dot3(ac, bp);
  if (d3 >= 0.0f && d4 <= d3) {  // vertex B region
    float d[3];
    sub3(p, b, d);
    return std::sqrt(dot3(d, d));
  }
  const float vc = d1 * d4 - d3 * d2;
  if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {  // edge AB region
    const float v = d1 / (d1 - d3);
    float q[3] = {a[0] + v * ab[0], a[1] + v * ab[1], a[2] + v * ab[2]};
    float d[3];
    sub3(p, q, d);
    return std::sqrt(dot3(d, d));
  }
  float cp[3];
  sub3(p, c, cp);
  const float d5 = dot3(ab, cp);
  const float d6 = dot3(ac, cp);
  if (d6 >= 0.0f && d5 <= d6) {  // vertex C region
    float d[3];
    sub3(p, c, d);
    return std::sqrt(dot3(d, d));
  }
  const float vb = d5 * d2 - d1 * d6;
  if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {  // edge AC region
    const float w = d2 / (d2 - d6);
    float q[3] = {a[0] + w * ac[0], a[1] + w * ac[1], a[2] + w * ac[2]};
    float d[3];
    sub3(p, q, d);
    return std::sqrt(dot3(d, d));
  }
  const float va = d3 * d6 - d5 * d4;
  if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {  // edge BC
    float bc[3];
    sub3(c, b, bc);
    const float denom = (d4 - d3) + (d5 - d6);
    const float w = (denom != 0.0f) ? (d4 - d3) / denom : 0.0f;
    float q[3] = {b[0] + w * bc[0], b[1] + w * bc[1], b[2] + w * bc[2]};
    float d[3];
    sub3(p, q, d);
    return std::sqrt(dot3(d, d));
  }
  // Face interior.
  const float denom = 1.0f / (va + vb + vc);
  const float v = vb * denom;
  const float w = vc * denom;
  float q[3] = {a[0] + ab[0] * v + ac[0] * w, a[1] + ab[1] * v + ac[1] * w,
                a[2] + ab[2] * v + ac[2] * w};
  float d[3];
  sub3(p, q, d);
  return std::sqrt(dot3(d, d));
}

auto MeshSdfBuilder::point_inside(const float p[3], const float* positions,
                                  const std::uint32_t* indices,
                                  std::uint32_t tri_count) const noexcept
    -> bool {
  // Parity ray along +X from p: count intersections with t > epsilon.
  // Moller–Trumbore with the ray dir (1,0,0); watertight meshes give a
  // clean odd/even answer; degenerate hits (parallel) are skipped.
  // Diagonal rule: a ray that pierces the shared edge of two coplanar
  // triangles would otherwise count both (even -> outside). Dedupe by
  // hit distance — votes within 1e-4 of an already-counted hit are the
  // same geometric piercing, not a second surface.
  std::uint32_t crossings = 0;
  float hit_ts[64]{};
  std::uint32_t hit_n = 0;
  for (std::uint32_t t = 0; t < tri_count; ++t) {
    const float* a = positions + indices[t * 3] * 3;
    const float* b = positions + indices[t * 3 + 1] * 3;
    const float* c = positions + indices[t * 3 + 2] * 3;
    float e1[3];
    float e2[3];
    sub3(b, a, e1);
    sub3(c, a, e2);
    // dir=(1,0,0): pvec = dir x e2 = (0, -e2.z, e2.y).
    const float pvec[3] = {0.0f, -e2[2], e2[1]};
    const float det = dot3(e1, pvec);
    if (det > -1e-9f && det < 1e-9f) {
      continue;  // parallel — skip vote
    }
    const float inv = 1.0f / det;
    float tvec[3];
    sub3(p, a, tvec);
    const float u = dot3(tvec, pvec) * inv;
    if (u < 0.0f || u > 1.0f) {
      continue;
    }
    // qvec = tvec x e1; v = dir.qvec * inv = qvec.x * inv (dir=(1,0,0)).
    const float qvec[3] = {tvec[1] * e1[2] - tvec[2] * e1[1],
                           tvec[2] * e1[0] - tvec[0] * e1[2],
                           tvec[0] * e1[1] - tvec[1] * e1[0]};
    const float v = qvec[0] * inv;
    if (v < 0.0f || u + v > 1.0f) {
      continue;
    }
    // t_hit = e2.qvec * inv.
    const float t_hit = dot3(e2, qvec) * inv;
    if (t_hit > 1e-6f) {
      bool dup = false;
      for (std::uint32_t h = 0; h < hit_n; ++h) {
        float dt = t_hit - hit_ts[h];
        if (dt < 0.0f) {
          dt = -dt;
        }
        if (dt < 1e-4f) {
          dup = true;
          break;
        }
      }
      if (!dup) {
        if (hit_n < 64) {
          hit_ts[hit_n++] = t_hit;
        }
        ++crossings;
      }
    }
  }
  return (crossings & 1u) != 0;
}

auto MeshSdfBuilder::build(const float* positions,
                           std::uint32_t vertex_count,
                           const std::uint32_t* indices,
                           std::uint32_t index_count,
                           const SdfBuildParams& params, MeshSdf* out) noexcept
    -> Status {
  if (positions == nullptr || indices == nullptr || out == nullptr ||
      out->bricks == nullptr || vertex_count == 0 || index_count == 0 ||
      (index_count % 3) != 0 || params.texel_size <= 0.0f ||
      params.brick_capacity == 0) {
    return MakeFail(StatusCode::kInvalidArgument, "sdf build: bad input");
  }
  const std::uint32_t tri_count = index_count / 3;
  if (tri_count > kSdfMaxTriangles ||
      params.brick_capacity > kSdfMaxBricks) {
    return MakeFail(StatusCode::kInvalidArgument, "sdf build: over cap");
  }
  // Validate indices before touching geometry.
  for (std::uint32_t i = 0; i < index_count; ++i) {
    if (indices[i] >= vertex_count) {
      return MakeFail(StatusCode::kInvalidArgument, "sdf build: index OOB");
    }
  }
  // Mesh AABB.
  float mn[3] = {positions[0], positions[1], positions[2]};
  float mx[3] = {positions[0], positions[1], positions[2]};
  for (std::uint32_t v = 1; v < vertex_count; ++v) {
    for (int k = 0; k < 3; ++k) {
      const float c = positions[v * 3 + static_cast<std::uint32_t>(k)];
      if (c < mn[k]) {
        mn[k] = c;
      }
      if (c > mx[k]) {
        mx[k] = c;
      }
    }
  }
  const float ts = params.texel_size;
  const float brick_world = ts * static_cast<float>(kSdfBrickRes);
  // Brick grid origin: floor(min / brick_world) * brick_world.
  float grid_origin[3];
  std::int64_t grid_n[3];
  for (int k = 0; k < 3; ++k) {
    const float g = std::floor(mn[k] / brick_world);
    grid_origin[k] = g * brick_world;
    const float span = mx[k] - grid_origin[k];
    grid_n[k] = static_cast<std::int64_t>(std::ceil(span / brick_world)) + 1;
    if (grid_n[k] <= 0) {
      grid_n[k] = 1;
    }
    if (grid_n[k] > 256) {  // address-space guard: 256^3 bricks max scan
      return MakeFail(StatusCode::kInvalidArgument, "sdf build: mesh too big");
    }
  }
  out->brick_count = 0;
  out->dropped_bricks = 0;
  for (int k = 0; k < 3; ++k) {
    out->aabb_min[k] = mn[k];
    out->aabb_max[k] = mx[k];
  }
  out->texel_size = ts;
  out->capacity = params.brick_capacity;

  const float band_world = kSdfBandVox * ts;
  // Scan candidate bricks; keep those within the surface band.
  for (std::int64_t bz = 0; bz < grid_n[2]; ++bz) {
    for (std::int64_t by = 0; by < grid_n[1]; ++by) {
      for (std::int64_t bx = 0; bx < grid_n[0]; ++bx) {
        float origin[3] = {grid_origin[0] + bx * brick_world,
                           grid_origin[1] + by * brick_world,
                           grid_origin[2] + bz * brick_world};
        // Brick AABB vs mesh AABB reject (with band padding): bricks fully
        // outside the padded box cannot hold band texels.
        bool overlaps = true;
        for (int k = 0; k < 3; ++k) {
          if (origin[k] > mx[k] + band_world ||
              origin[k] + brick_world < mn[k] - band_world) {
            overlaps = false;
            break;
          }
        }
        if (!overlaps) {
          continue;
        }
        // Materialize texels into a local brick; measure min |d| to decide
        // band membership. Local staging keeps partial bricks out of the
        // caller's storage (failed/empty bricks leave no trace).
        SdfBrick staged{};
        staged.origin[0] = origin[0];
        staged.origin[1] = origin[1];
        staged.origin[2] = origin[2];
        staged.texel_size = ts;
        float min_abs = 1e30f;
        for (std::uint32_t tz = 0; tz < kSdfBrickRes; ++tz) {
          for (std::uint32_t ty = 0; ty < kSdfBrickRes; ++ty) {
            for (std::uint32_t tx = 0; tx < kSdfBrickRes; ++tx) {
              float p[3] = {origin[0] + (tx + 0.5f) * ts,
                            origin[1] + (ty + 0.5f) * ts,
                            origin[2] + (tz + 0.5f) * ts};
              float best = 1e30f;
              for (std::uint32_t t = 0; t < tri_count; ++t) {
                const float* a = positions + indices[t * 3] * 3;
                const float* b = positions + indices[t * 3 + 1] * 3;
                const float* c = positions + indices[t * 3 + 2] * 3;
                const float d = point_triangle_dist(p, a, b, c);
                if (d < best) {
                  best = d;
                }
              }
              const bool inside =
                  point_inside(p, positions, indices, tri_count);
              const float sd = inside ? -best : best;
              const float ad = best;  // |sd| == best (unsigned distance is positive)
              if (ad < min_abs) {
                min_abs = ad;
              }
              // Quantize: sd/ts voxels clamped to [-range,+range].
              float vox = sd / ts;
              if (vox < -kSdfQuantRangeVox) {
                vox = -kSdfQuantRangeVox;
              }
              if (vox > kSdfQuantRangeVox) {
                vox = kSdfQuantRangeVox;
              }
              const int q = static_cast<int>(std::round(
                  vox * (127.0f / kSdfQuantRangeVox)));
              const std::size_t idx =
                  (tz * kSdfBrickRes + ty) * kSdfBrickRes + tx;
              staged.texels[idx] =
                  static_cast<std::int8_t>(q < -127 ? -127 : (q > 127 ? 127
                                                                       : q));
            }
          }
        }
        if (min_abs > band_world) {
          continue;  // sparse skip: no surface within the band
        }
        if (out->brick_count >= params.brick_capacity) {
          ++out->dropped_bricks;
          continue;
        }
        out->bricks[out->brick_count] = staged;
        ++out->brick_count;
      }
    }
  }
  return MakeOk();
}

auto MeshSdfBuilder::sample_voxel(const MeshSdf& sdf, float x, float y,
                                  float z) noexcept -> float {
  if (sdf.bricks == nullptr || sdf.brick_count == 0 || sdf.texel_size <= 0) {
    return kSdfBandVox;
  }
  const float ts = sdf.texel_size;
  // Locate the resident brick (linear scan is fine for tests/oracles;
  // the GPU path uses a brick address map). A point exactly on a shared
  // brick face belongs to both neighbors; prefer the brick whose CLOSED
  // texel interval contains the point unclamped (no edge pull), so the
  // sign comes from the brick that truly straddles the surface instead
  // of the neighbor's clamped edge texel. Two passes: exact-fit first,
  // clamped-edge fallback second.
  const auto sample_in = [&](const SdfBrick& br, float qx, float qy,
                             float qz) -> float {
    // Trilinear over int8 texels (dequantized to voxels).
    const float fx = qx - std::floor(qx);
    const float fy = qy - std::floor(qy);
    const float fz = qz - std::floor(qz);
    int ix = static_cast<int>(std::floor(qx));
    int iy = static_cast<int>(std::floor(qy));
    int iz = static_cast<int>(std::floor(qz));
    if (ix < 0) {
      ix = 0;
    }
    if (iy < 0) {
      iy = 0;
    }
    if (iz < 0) {
      iz = 0;
    }
    if (ix > 6) {
      ix = 6;
    }
    if (iy > 6) {
      iy = 6;
    }
    if (iz > 6) {
      iz = 6;
    }
    auto tex = [&](int xx, int yy, int zz) -> float {
      const std::size_t idx =
          (static_cast<std::size_t>(zz) * kSdfBrickRes +
           static_cast<std::size_t>(yy)) *
              kSdfBrickRes +
          static_cast<std::size_t>(xx);
      return static_cast<float>(br.texels[idx]) *
             (kSdfQuantRangeVox / 127.0f);
    };
    const float c000 = tex(ix, iy, iz);
    const float c100 = tex(ix + 1, iy, iz);
    const float c010 = tex(ix, iy + 1, iz);
    const float c110 = tex(ix + 1, iy + 1, iz);
    const float c001 = tex(ix, iy, iz + 1);
    const float c101 = tex(ix + 1, iy, iz + 1);
    const float c011 = tex(ix, iy + 1, iz + 1);
    const float c111 = tex(ix + 1, iy + 1, iz + 1);
    const float c00 = c000 + fx * (c100 - c000);
    const float c10 = c010 + fx * (c110 - c010);
    const float c01 = c001 + fx * (c101 - c001);
    const float c11 = c011 + fx * (c111 - c011);
    const float c0 = c00 + fy * (c10 - c00);
    const float c1 = c01 + fy * (c11 - c01);
    return c0 + fz * (c1 - c0);
  };
  // Pass 1: exact-fit brick (point inside the closed texel interval —
  // no clamping, no edge pull).
  for (std::uint32_t b = 0; b < sdf.brick_count; ++b) {
    const SdfBrick& br = sdf.bricks[b];
    const float lx = (x - br.origin[0]) / ts - 0.5f;
    const float ly = (y - br.origin[1]) / ts - 0.5f;
    const float lz = (z - br.origin[2]) / ts - 0.5f;
    if (lx >= 0.0f && ly >= 0.0f && lz >= 0.0f && lx <= 7.0f && ly <= 7.0f &&
        lz <= 7.0f) {
      return sample_in(br, lx, ly, lz);
    }
  }
  // Pass 2: within half a texel of a brick face — clamp to the edge.
  for (std::uint32_t b = 0; b < sdf.brick_count; ++b) {
    const SdfBrick& br = sdf.bricks[b];
    float lx = (x - br.origin[0]) / ts - 0.5f;
    float ly = (y - br.origin[1]) / ts - 0.5f;
    float lz = (z - br.origin[2]) / ts - 0.5f;
    if (lx < -0.5f || ly < -0.5f || lz < -0.5f || lx > 7.5f || ly > 7.5f ||
        lz > 7.5f) {
      continue;
    }
    if (lx < 0.0f) {
      lx = 0.0f;
    }
    if (ly < 0.0f) {
      ly = 0.0f;
    }
    if (lz < 0.0f) {
      lz = 0.0f;
    }
    if (lx > 7.0f) {
      lx = 7.0f;
    }
    if (ly > 7.0f) {
      ly = 7.0f;
    }
    if (lz > 7.0f) {
      lz = 7.0f;
    }
    return sample_in(br, lx, ly, lz);
  }
  return kSdfBandVox;  // non-resident: conservative far-field
}

auto MeshSdfBuilder::dense_brick_bytes(const float aabb_min[3],
                                       const float aabb_max[3],
                                       float texel_size) noexcept
    -> std::uint64_t {
  if (texel_size <= 0.0f) {
    return 0;
  }
  std::uint64_t bricks = 1;
  for (int k = 0; k < 3; ++k) {
    const float span = aabb_max[k] - aabb_min[k];
    if (span < 0.0f) {
      return 0;
    }
    const std::uint64_t n = static_cast<std::uint64_t>(
        std::ceil(span / (texel_size * kSdfBrickRes))) +
        1;
    bricks *= (n == 0 ? 1 : n);
  }
  return bricks * sizeof(SdfBrick);
}

}  // namespace engine::renderer

// transform.cpp — component math; see header.
#include "engine/ecs/transform.hpp"

#if defined(__AVX2__) || defined(ENGINE_HAS_AVX2)
#include <immintrin.h>
#define ENGINE_SIMD_AVX2 1
#elif defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#define ENGINE_SIMD_NEON 1
#endif

namespace engine::ecs {

static_assert(sizeof(LocalTransform) == 64,
              "LocalTransform must be exactly one cache line");
static_assert(alignof(LocalTransform) == 64, "LocalTransform align drifted");
static_assert(sizeof(WorldTransform) == 64,
              "WorldTransform must be exactly one cache line");
static_assert(alignof(WorldTransform) == 64, "WorldTransform align drifted");

auto ComposeLocal(const LocalTransform& t, float out[16]) noexcept -> void {
  // Quaternion -> rotation matrix (row-major), then scale folded into the
  // 3x3 block (M = T * R * S: rows scaled, translation unscaled).
  const float x = t.qx;
  const float y = t.qy;
  const float z = t.qz;
  const float w = t.qw;
  const float xx = x * x;
  const float yy = y * y;
  const float zz = z * z;
  const float xy = x * y;
  const float xz = x * z;
  const float yz = y * z;
  const float wx = w * x;
  const float wy = w * y;
  const float wz = w * z;

  out[0] = (1.0F - 2.0F * (yy + zz)) * t.sx;
  out[1] = (2.0F * (xy + wz)) * t.sx;
  out[2] = (2.0F * (xz - wy)) * t.sx;
  out[3] = 0.0F;
  out[4] = (2.0F * (xy - wz)) * t.sy;
  out[5] = (1.0F - 2.0F * (xx + zz)) * t.sy;
  out[6] = (2.0F * (yz + wx)) * t.sy;
  out[7] = 0.0F;
  out[8] = (2.0F * (xz + wy)) * t.sz;
  out[9] = (2.0F * (yz - wx)) * t.sz;
  out[10] = (1.0F - 2.0F * (xx + yy)) * t.sz;
  out[11] = 0.0F;
  out[12] = t.px;
  out[13] = t.py;
  out[14] = t.pz;
  out[15] = 1.0F;
}

auto MatMul4x4(const float a[16], const float b[16],
                float out[16]) noexcept -> void {
#if defined(ENGINE_SIMD_AVX2)
  // 8-wide AVX2: each output row = sum over k of broadcast(a[i][k]) *
  // b_row(k). Loads are two 128-bit halves per row (never crossing a row
  // boundary): _mm_loadu_ps faults only when its own 16 bytes are
  // unmapped, so b+4/b+8/b+12 rows and stack-suballocated matrices are
  // safe. ASan (dynamic-alloca instrumentation) treats a 32B vmovups as
  // a 32B access and flags 16B-aligned-only buffers; the split halves
  // match the actual 16B row granularity. On silicon the halves fuse in
  // the load buffer — one cache line per row either way.
  const __m128 b0l = _mm_loadu_ps(b + 0);
  const __m128 b0h = _mm_loadu_ps(b + 4);
  const __m128 b1l = _mm_loadu_ps(b + 8);
  const __m128 b1h = _mm_loadu_ps(b + 12);
  // Row k of b is b[4k..4k+4). b0..b3 below are the four ROWS
  // broadcast to both 128-bit lanes: b0 = {b[0..4], b[0..4]}, etc.
  // Built from single 16-byte loads that never cross a 16-byte boundary.
  const __m256 b0 = _mm256_insertf128_ps(_mm256_castps128_ps256(b0l), b0l, 1);
  const __m256 b1 = _mm256_insertf128_ps(_mm256_castps128_ps256(b0h), b0h, 1);
  const __m256 b2 = _mm256_insertf128_ps(_mm256_castps128_ps256(b1l), b1l, 1);
  const __m256 b3 =
      _mm256_insertf128_ps(_mm256_castps128_ps256(b1h), b1h, 1);
  for (int i = 0; i < 4; ++i) {
    const float* ai = a + i * 4;
    __m256 r = _mm256_mul_ps(_mm256_broadcast_ss(ai + 0), b0);
    r = _mm256_fmadd_ps(_mm256_broadcast_ss(ai + 1), b1, r);
    r = _mm256_fmadd_ps(_mm256_broadcast_ss(ai + 2), b2, r);
    r = _mm256_fmadd_ps(_mm256_broadcast_ss(ai + 3), b3, r);
    // Store the row then its duplicate lane-by-lane: with b-rows
    // lane-duplicated above, lanes [0..4) and [4..8) hold the same row,
    // so two 16-byte stores write identical data and no 32B store ever
    // executes (keeps ASan's dynamic-alloca instrumentation quiet — it
    // sizes a vmovups by its width, not by the object's row layout).
    _mm_storeu_ps(out + i * 4, _mm256_castps256_ps128(r));
  }
#elif defined(ENGINE_SIMD_NEON)
  // Row-scale accumulate (mirrors the AVX2 path, no transpose): row i of
  // the output is the sum over k of a[i][k] * (row k of b). Each lane j
  // then holds sum_k a[i][k]*b[k][j], i.e. standard row-major matmul.
  // (A previous transpose-then-dot form mixed up columns 1 and 2; this
  // form has no column indexing to get wrong.)
  const float32x4_t b0 = vld1q_f32(b + 0);
  const float32x4_t b1 = vld1q_f32(b + 4);
  const float32x4_t b2 = vld1q_f32(b + 8);
  const float32x4_t b3 = vld1q_f32(b + 12);
  for (int i = 0; i < 4; ++i) {
    const float* ai = a + i * 4;
    float32x4_t r = vmulq_n_f32(b0, ai[0]);
    r = vmlaq_n_f32(r, b1, ai[1]);
    r = vmlaq_n_f32(r, b2, ai[2]);
    r = vmlaq_n_f32(r, b3, ai[3]);
    vst1q_f32(out + i * 4, r);
  }
#else
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      float s = 0.0F;
      for (int k = 0; k < 4; ++k) {
        s += a[i * 4 + k] * b[k * 4 + j];
      }
      out[i * 4 + j] = s;
    }
  }
#endif
}

}  // namespace engine::ecs

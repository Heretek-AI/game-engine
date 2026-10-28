// transform.hpp — M1-03 core built-in native components.
//
// LocalTransform: per-entity offset relative to its parent — position,
// rotation (unit quaternion), non-uniform scale. WorldTransform: the
// entity's composed world-space matrix, row-major 4x4.
//
// Layout: 64-byte aligned so table columns ride whole cache lines and
// AVX2 (8 floats / op) consumes rows without split loads. Sizes are
// asserted in the TU.
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"

namespace engine::ecs {

struct alignas(64) LocalTransform {
  float px, py, pz;  // position
  float qx, qy, qz, qw;  // rotation (unit quaternion)
  float sx, sy, sz;  // scale
  float pad[5];  // 12 + 16 + 12 + 20 = 60 -> pad to 64
};

struct alignas(64) WorldTransform {
  float m[16];  // row-major 4x4 world matrix
};

// Compose a row-major local matrix from a LocalTransform.
// out[i*4+j]: row i, column j.
ENGINE_CORE_API auto ComposeLocal(const LocalTransform& t,
                                  float out[16]) noexcept -> void;

// out = a * b (row-major 4x4). SIMD: AVX2 on x86-64, NEON on AArch64,
// scalar fallback elsewhere. All pointers must be 16B+ aligned (they
// are: 64B-aligned components / frame-scratch rows).
ENGINE_CORE_API auto MatMul4x4(const float a[16], const float b[16],
                               float out[16]) noexcept -> void;

inline auto MatIdentity(float out[16]) noexcept -> void {
  for (int i = 0; i < 16; ++i) {
    out[i] = (i % 5 == 0) ? 1.0F : 0.0F;
  }
}

}  // namespace engine::ecs

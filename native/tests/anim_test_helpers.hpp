// anim_test_helpers.hpp — P5-02 shared test prelude (Sonar dedup).
//
// test_anim_sampling.cpp and test_anim_blending.cpp both need the same
// fail-counting check() plus float Near()/NearArr() helpers. One header
// keeps the 21-line mirror from tripping the Sonar duplication gate.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdio>

namespace engine::test::anim {

inline auto check(bool ok, const char* name) -> int {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  std::fflush(stdout);
  return ok ? 0 : 1;
}

inline auto Near(float a, float b, float eps = 1e-5F) -> bool {
  return std::fabs(a - b) <= eps;
}

inline auto NearArr(const float* a, const float* b, std::size_t n,
                    float eps = 1e-5F) -> bool {
  for (std::size_t i = 0; i < n; ++i) {
    if (!Near(a[i], b[i], eps)) {
      return false;
    }
  }
  return true;
}

}  // namespace engine::test::anim

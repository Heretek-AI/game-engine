#pragma once
#include <cstdio>
#include <cstdlib>
#include <string_view>
// Returns 1 (a failure to add to the test's count) when ENGINE_REQUIRE_GPU=1,
// else prints [SKIP] and returns 0.
inline auto gpu_missing(std::string_view what) -> int {
  const char* req = std::getenv("ENGINE_REQUIRE_GPU");
  const bool required = req != nullptr && std::string_view(req) == "1";
  std::printf("[%s] %.*s\n", required ? "FAIL" : "SKIP",
              static_cast<int>(what.size()), what.data());
  return required ? 1 : 0;
}

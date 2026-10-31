#pragma once
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string_view>
// Returns 1 (a failure to add to the test's count) when ENGINE_REQUIRE_GPU=1,
// else prints [SKIP] and returns 0.
inline auto gpu_missing(std::string_view what) -> int {
#if defined(_WIN32)
  // MSVC treats getenv as a C4996 error under /WX: _dupenv_s instead.
  char* dup = nullptr;
  std::size_t dup_len = 0;
  const bool required = _dupenv_s(&dup, &dup_len, "ENGINE_REQUIRE_GPU") == 0 &&
                        dup != nullptr && std::string_view(dup) == "1";
  std::free(dup);
#else
  const char* req = std::getenv("ENGINE_REQUIRE_GPU");
  const bool required = req != nullptr && std::string_view(req) == "1";
#endif
  std::printf("[%s] %.*s\n", required ? "FAIL" : "SKIP",
              static_cast<int>(what.size()), what.data());
  return required ? 1 : 0;
}

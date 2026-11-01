// test_hello_cube_compare.cpp — P1-09 golden compare step.
//
// The engine_hello_cube CTest runs the HelloCube sample offscreen
// (--frames 10 --screenshot <build>/hello_cube.png) and prints
// HELLO-CUBE-OK. This second step loads that PNG and compares it against
// the checked-in golden `hello_cube_10f_640x360.png` with the P1-05
// tolerance (per-channel <= 2, <= 0.1% differing pixels). Prints:
//
//   [PASS] golden: hello_cube_10f_640x360   (via golden_check)
//
// The sample screenshot lands next to this exe at CTest time
// (HELLO_CUBE_PNG carries the absolute path); missing file fails closed.
// GPU rules: offscreen only; no-device folds into the missing-file FAIL
// (the sample itself would already have failed).
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <vector>

#include "golden_compare.hpp"

namespace {

auto failures = 0;

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}

}  // namespace

auto main() -> int {
#if defined(_WIN32)
  // MSVC treats getenv as a C4996 error under /WX: _dupenv_s instead.
  char* dup = nullptr;
  std::size_t dup_len = 0;
  const bool have_png =
      _dupenv_s(&dup, &dup_len, "HELLO_CUBE_PNG") == 0 && dup != nullptr &&
      dup[0] != '\0';
  const std::string png_str = have_png ? dup : "";
  std::free(dup);
  const char* png = have_png ? png_str.c_str() : nullptr;
#else
  const char* png = std::getenv("HELLO_CUBE_PNG");
#endif
  if (png == nullptr || png[0] == '\0') {
    std::printf("[FAIL] golden: hello_cube_10f_640x360 (HELLO_CUBE_PNG unset)\n");
    return 1;
  }
  std::vector<std::uint8_t> rgba;
  int w = 0;
  int h = 0;
  if (!engine::test::GoldenLoadPng(png, rgba, w, h)) {
    std::printf("[FAIL] golden: hello_cube_10f_640x360 (cannot load %s)\n",
                png);
    return 1;
  }
  if (w != 640 || h != 360) {
    std::printf("[FAIL] golden: hello_cube_10f_640x360 (size %dx%d, want 640x360)\n",
                w, h);
    return 1;
  }
  failures += engine::test::golden_check(
      "hello_cube_10f_640x360",
      std::span<const std::uint8_t>(rgba.data(), rgba.size()),
      static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
  check(failures == 0, "hello-cube: golden compare done");
  return failures == 0 ? 0 : 1;
}

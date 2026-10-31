// golden_compare.hpp — P1-05 contract, first landed by P4-01.
//
// Compare an in-memory RGBA8 frame against the checked-in golden PNG
// `native/tests/golden/<name>.png`:
//
//   auto golden_check(std::string_view name,
//                     std::span<const std::uint8_t> rgba,
//                     std::uint32_t w, std::uint32_t h) -> int
//
// Returns 0 on pass, 1 on failure. Pass rule (P1-05): every channel
// differs by at most 2, and at most 0.1% of pixels differ at all.
// Golden PNGs are RGBA (gotcha 23: swizzle BGRA swapchain data to RGBA
// BEFORE calling). On failure writes `<build>/golden_out/<name>.actual.png`
// and `<name>.diff.png` (red where different) and prints both paths.
// When ENGINE_UPDATE_GOLDENS=1, overwrites the golden and prints
// `[PASS] golden: updated <name>`.
//
// stb_image / stb_image_write come from the assets module's stb
// FetchContent (test TU defines the IMPLEMENTATION macros once; the
// header only declares). ENGINE_GOLDEN_DIR (compile definition) points
// at the source-tree golden dir; ENGINE_GOLDEN_OUT_DIR points at
// `<build>/golden_out`.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace engine::test {

// Per-channel tolerance + max fraction of differing pixels (P1-05).
inline constexpr int kGoldenChannelTol = 2;
inline constexpr double kGoldenMaxFrac = 0.001;

// Implementation hooks (defined once in a test TU): PNG decode/encode
// via stb. The header stays stb-free so every TU compiles fast.
auto GoldenLoadPng(const std::string& path, std::vector<std::uint8_t>& out,
                   int& w_out, int& h_out) -> bool;
auto GoldenWritePng(const std::string& path,
                    const std::uint8_t* rgba, std::uint32_t w,
                    std::uint32_t h) -> bool;
auto GoldenMkdirP(const std::string& path) -> bool;

inline auto golden_path(std::string_view name) -> std::string {
  std::string p = ENGINE_GOLDEN_DIR;
  if (!p.empty() && p.back() != '/') {
    p.push_back('/');
  }
  p += std::string(name);
  p += ".png";
  return p;
}

inline auto golden_out_path(std::string_view name, std::string_view suffix)
    -> std::string {
  std::string p = ENGINE_GOLDEN_OUT_DIR;
  if (!p.empty() && p.back() != '/') {
    p.push_back('/');
  }
  p += std::string(name);
  p += std::string(suffix);
  return p;
}

inline auto golden_check(std::string_view name,
                         std::span<const std::uint8_t> rgba, std::uint32_t w,
                         std::uint32_t h) -> int {
  if (rgba.size() != static_cast<std::size_t>(w) * h * 4) {
    std::printf("[FAIL] golden: %.*s size mismatch\n",
                static_cast<int>(name.size()), name.data());
    return 1;
  }
#if defined(_WIN32)
  // MSVC treats getenv as a C4996 error under /WX: _dupenv_s instead.
  char* dup = nullptr;
  std::size_t dup_len = 0;
  const bool update =
      _dupenv_s(&dup, &dup_len, "ENGINE_UPDATE_GOLDENS") == 0 &&
      dup != nullptr && std::string_view(dup) == "1";
  std::free(dup);
#else
  const char* upd = std::getenv("ENGINE_UPDATE_GOLDENS");
  const bool update = upd != nullptr && std::string_view(upd) == "1";
#endif
  const std::string path = golden_path(name);
  if (update) {
    if (!GoldenMkdirP(path) ||
        !GoldenWritePng(path, rgba.data(), w, h)) {
      std::printf("[FAIL] golden: update write failed %s\n", path.c_str());
      return 1;
    }
    std::printf("[PASS] golden: updated %.*s\n", static_cast<int>(name.size()),
                name.data());
    return 0;
  }
  std::vector<std::uint8_t> want;
  int gw = 0;
  int gh = 0;
  if (!GoldenLoadPng(path, want, gw, gh)) {
    std::printf("[FAIL] golden: %.*s missing (%s); re-run with "
                "ENGINE_UPDATE_GOLDENS=1\n",
                static_cast<int>(name.size()), name.data(), path.c_str());
    return 1;
  }
  if (gw != static_cast<int>(w) || gh != static_cast<int>(h) ||
      want.size() != rgba.size()) {
    std::printf("[FAIL] golden: %.*s dims %dx%d want %ux%u\n",
                static_cast<int>(name.size()), name.data(), gw, gh, w, h);
    return 1;
  }
  std::size_t diff_px = 0;
  const std::size_t px = static_cast<std::size_t>(w) * h;
  for (std::size_t p = 0; p < px; ++p) {
    bool diff = false;
    for (int c = 0; c < 4; ++c) {
      const int d = static_cast<int>(rgba[p * 4 + static_cast<std::size_t>(c)]) -
                    static_cast<int>(want[p * 4 + static_cast<std::size_t>(c)]);
      if (d > kGoldenChannelTol || d < -kGoldenChannelTol) {
        diff = true;
        break;
      }
    }
    if (diff) {
      ++diff_px;
    }
  }
  const double frac =
      px == 0 ? 1.0 : static_cast<double>(diff_px) / static_cast<double>(px);
  if (frac <= kGoldenMaxFrac) {
    std::printf("[PASS] golden: %.*s\n", static_cast<int>(name.size()),
                name.data());
    return 0;
  }
  // Failure artifacts: actual + red-marked diff.
  std::vector<std::uint8_t> diff(rgba.size());
  for (std::size_t p = 0; p < px; ++p) {
    bool d = false;
    for (int c = 0; c < 4; ++c) {
      const int dd =
          static_cast<int>(rgba[p * 4 + static_cast<std::size_t>(c)]) -
          static_cast<int>(want[p * 4 + static_cast<std::size_t>(c)]);
      if (dd > kGoldenChannelTol || dd < -kGoldenChannelTol) {
        d = true;
        break;
      }
    }
    if (d) {
      diff[p * 4 + 0] = 255;
      diff[p * 4 + 1] = 0;
      diff[p * 4 + 2] = 0;
      diff[p * 4 + 3] = 255;
    } else {
      diff[p * 4 + 0] = rgba[p * 4 + 0];
      diff[p * 4 + 1] = rgba[p * 4 + 1];
      diff[p * 4 + 2] = rgba[p * 4 + 2];
      diff[p * 4 + 3] = rgba[p * 4 + 3];
    }
  }
  const std::string actual = golden_out_path(name, ".actual.png");
  const std::string diffp = golden_out_path(name, ".diff.png");
  GoldenMkdirP(actual);
  GoldenMkdirP(diffp);
  GoldenWritePng(actual, rgba.data(), w, h);
  GoldenWritePng(diffp, diff.data(), w, h);
  std::printf("[FAIL] golden: %.*s frac=%.4f px=%zu actual=%s diff=%s\n",
              static_cast<int>(name.size()), name.data(), frac, diff_px,
              actual.c_str(), diffp.c_str());
  return 1;
}

}  // namespace engine::test

// golden_stb.cpp — stb IMPLEMENTATION TU for golden_compare.hpp hooks.
//
// One TU in the P4-01 test exe defines GoldenLoadPng/GoldenWritePng/
// GoldenMkdirP via stb_image / stb_image_write from the assets module's
// stb FetchContent (assets links them PUBLIC, so the include dir +
// ENGINE_ASSETS_HAS_STB propagate to this test through engine_core).
// Offline degrade: without ENGINE_ASSETS_HAS_STB the hooks report
// failure (golden comparisons fail closed, never silently pass).
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#if defined(ENGINE_ASSETS_HAS_STB)
// stb_image_write encode implementation lives in this TU (the assets
// module's engine_stb_image TU only implements stb_image DECODE, so no
// duplicate-symbol clash; decode here reuses the assets TU's stbi_load
// through the normal link). STB_IMAGE_WRITE_STATIC keeps the encoder
// TU-local (no global stbi_write_* symbols leak from the test exe).
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#endif
#if defined(ENGINE_ASSETS_HAS_STB)
#include "stb_image.h"
#endif

namespace engine::test {

auto GoldenMkdirP(const std::string& path) -> bool {
  const std::size_t slash = path.find_last_of('/');
  if (slash == std::string::npos || slash == 0) {
    return true;
  }
  const std::string dir = path.substr(0, slash);
  std::string cur;
  if (!dir.empty() && dir[0] == '/') {
    cur = "/";
  }
  std::size_t i = cur.empty() ? 0 : 1;
  for (; i <= dir.size(); ++i) {
    if (i == dir.size() || dir[i] == '/') {
      if (!cur.empty() && cur != "/") {
#if defined(_WIN32)
        CreateDirectoryA(cur.c_str(), nullptr);
#else
        mkdir(cur.c_str(), 0755);  // NOLINT: test tooling path.
#endif
      }
      if (i < dir.size()) {
        cur.push_back('/');
      }
    } else {
      cur.push_back(dir[i]);
    }
  }
#if defined(_WIN32)
  DWORD attr = GetFileAttributesA(dir.c_str());
  return attr != INVALID_FILE_ATTRIBUTES &&
         (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
  struct stat st {};
  return ::stat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

auto GoldenLoadPng(const std::string& path, std::vector<std::uint8_t>& out,
                   int& w_out, int& h_out) -> bool {
#if !defined(ENGINE_ASSETS_HAS_STB)
  (void)path;
  (void)out;
  w_out = 0;
  h_out = 0;
  return false;
#else
  int w = 0;
  int h = 0;
  int comp = 0;
  stbi_uc* px = stbi_load(path.c_str(), &w, &h, &comp, 4);
  if (px == nullptr || w <= 0 || h <= 0) {
    return false;
  }
  const std::size_t n = static_cast<std::size_t>(w) *
                        static_cast<std::size_t>(h) * 4;
  out.assign(px, px + n);
  stbi_image_free(px);
  w_out = w;
  h_out = h;
  return true;
#endif
}

auto GoldenWritePng(const std::string& path, const std::uint8_t* rgba,
                    std::uint32_t w, std::uint32_t h) -> bool {
#if !defined(ENGINE_ASSETS_HAS_STB)
  (void)path;
  (void)rgba;
  (void)w;
  (void)h;
  return false;
#else
  if (rgba == nullptr || w == 0 || h == 0) {
    return false;
  }
  const int iw = static_cast<int>(w);
  const int ih = static_cast<int>(h);
  return stbi_write_png(path.c_str(), iw, ih, 4, rgba, iw * 4) != 0;
#endif
}

}  // namespace engine::test

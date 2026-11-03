// scene_file_util.hpp — P2-06: shared UTF-8 file helpers for scene I/O.
//
// Both scene TUs need the same semantics: UTF-8 paths on Windows (MSVC
// fopen reads the ANSI code page, so non-ASCII paths fail) and atomic
// writes (fopen "wb" truncates the target before data lands; a short write
// must not destroy the user's previous scene). Header-inline so no ABI
// surface changes; Sonar-clean (no C arrays / strlen / snprintf).
#pragma once

#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace engine::scene::file_util {

#if defined(_WIN32)
// UTF-8 narrow -> wide (bridge precedent: NarrowToWide in dotnet_host.cpp).
inline auto NarrowToWide(const char* src, std::vector<wchar_t>& out) noexcept
    -> bool {
  if (src == nullptr || src[0] == '\0') {
    return false;
  }
  const int need =
      ::MultiByteToWideChar(CP_UTF8, 0, src, -1, nullptr, 0);
  if (need <= 0) {
    return false;
  }
  out.resize(static_cast<std::size_t>(need));
  return ::MultiByteToWideChar(CP_UTF8, 0, src, -1, out.data(), need) == need;
}

inline auto OpenWide(const char* path, const wchar_t* mode) noexcept
    -> std::FILE* {
  std::vector<wchar_t> wpath;
  if (!NarrowToWide(path, wpath)) {
    return nullptr;
  }
#if defined(_MSC_VER)
  std::FILE* f = nullptr;
  if (_wfopen_s(&f, wpath.data(), mode) != 0) {
    return nullptr;
  }
  return f;
#else
  return ::_wfopen(wpath.data(), mode);
#endif
}
#endif

inline auto OpenForRead(const char* path) noexcept -> std::FILE* {
  if (path == nullptr || path[0] == '\0') {
    return nullptr;
  }
#if defined(_WIN32)
  return OpenWide(path, L"rb");
#elif defined(_MSC_VER)
  std::FILE* f = nullptr;
  if (fopen_s(&f, path, "rb") != 0) {
    return nullptr;
  }
  return f;
#else
  return std::fopen(path, "rb");
#endif
}

inline auto OpenForWrite(const char* path) noexcept -> std::FILE* {
  if (path == nullptr || path[0] == '\0') {
    return nullptr;
  }
#if defined(_WIN32)
  return OpenWide(path, L"wb");
#elif defined(_MSC_VER)
  std::FILE* f = nullptr;
  if (fopen_s(&f, path, "wb") != 0) {
    return nullptr;
  }
  return f;
#else
  return std::fopen(path, "wb");
#endif
}

// AtomicWrite: bytes land in "<path>.tmp" (same dir, same filesystem), then
// the tmp is renamed over the target. fwrite short writes and fclose flush
// failures both fail the call and remove the tmp — the previous scene file
// is never truncated by a failed save.
inline auto AtomicWrite(const char* path, const char* data,
                        std::size_t size) noexcept -> bool {
  if (path == nullptr || path[0] == '\0' ||
      (data == nullptr && size != 0U)) {
    return false;
  }
  const std::string tmp = std::string(path) + ".tmp";
  std::FILE* f = OpenForWrite(tmp.c_str());
  if (f == nullptr) {
    return false;
  }
  const std::size_t written =
      (size == 0U) ? 0U : std::fwrite(data, 1, size, f);
  const bool closed_ok = std::fclose(f) == 0;
  if (written != size || !closed_ok) {
    std::remove(tmp.c_str());
    return false;
  }
#if defined(_WIN32)
  std::vector<wchar_t> wtmp;
  std::vector<wchar_t> wdst;
  if (!NarrowToWide(tmp.c_str(), wtmp) ||
      !NarrowToWide(path, wdst)) {
    std::remove(tmp.c_str());
    return false;
  }
  // std::rename fails when the target exists on Windows; MoveFileExW with
  // REPLACE_EXISTING is the atomic overwrite there.
  if (::MoveFileExW(wtmp.data(), wdst.data(),
                    MOVEFILE_REPLACE_EXISTING |
                        MOVEFILE_WRITE_THROUGH) == 0) {
    std::remove(tmp.c_str());
    return false;
  }
#else
  if (std::rename(tmp.c_str(), path) != 0) {
    std::remove(tmp.c_str());
    return false;
  }
#endif
  return true;
}

// ReadAll: whole file into `out` (binary). False on any open/read error.
inline auto ReadAll(const char* path, std::string& out) noexcept -> bool {
  std::FILE* f = OpenForRead(path);
  if (f == nullptr) {
    return false;
  }
  std::vector<char> chunk(4096U);
  while (true) {
    const std::size_t got = std::fread(chunk.data(), 1, chunk.size(), f);
    out.append(chunk.data(), got);
    if (got < chunk.size()) {
      break;
    }
  }
  // Read-path fclose flushes nothing; a close error cannot lose data that
  // was already returned by fread.
  std::fclose(f);
  return true;
}

}  // namespace engine::scene::file_util

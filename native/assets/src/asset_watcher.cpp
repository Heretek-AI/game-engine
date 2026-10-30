// asset_watcher.cpp — poll-based directory diff; see header.
#include "engine/assets/asset_watcher.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <utility>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace engine::assets {
namespace {

auto LowerExt(std::string_view path) -> std::string {
  std::size_t dot = path.rfind('.');
  std::string ext = dot == std::string_view::npos
                        ? std::string()
                        : std::string(path.substr(dot + 1));
  for (char& c : ext) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return ext;
}

#if defined(_WIN32)
auto FileEntry(const std::string& path, AssetWatcher::Entry& e) -> bool {
  WIN32_FILE_ATTRIBUTE_DATA ad{};
  if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &ad)) {
    return false;
  }
  if (ad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
    return false;
  }
  std::uint64_t wt = (static_cast<std::uint64_t>(ad.ftLastWriteTime.dwHighDateTime)
                      << 32) |
                     ad.ftLastWriteTime.dwLowDateTime;
  e.mtime_ns = wt * 100ULL - 11644473600000000000ULL;
  e.size = (static_cast<std::uint64_t>(ad.nFileSizeHigh) << 32) |
           ad.nFileSizeLow;
  return true;
}
#else
// Defined below (needs AssetWatcher::Entry visible at namespace scope).
#endif

}  // namespace

auto AssetWatcher::IsTracked(std::string_view path) noexcept -> bool {
  std::string ext = LowerExt(path);
  return ext == "glb" || ext == "gltf" || ext == "png" || ext == "jpg" ||
         ext == "jpeg";
}

auto AssetWatcher::start(const std::string& root) noexcept -> bool {
  stop();
  root_ = root;
#if defined(_WIN32)
  DWORD attr = GetFileAttributesA(root.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES) {
    if (!CreateDirectoryA(root.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
      return false;
    }
  } else if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
    return false;
  }
#else
  // NOLINTNEXTLINE: best-effort mkdir, host tooling path.
  ::mkdir(root.c_str(), 0755);
  DIR* d = ::opendir(root.c_str());
  if (d == nullptr) {
    return false;
  }
  ::closedir(d);
#endif
  running_ = true;
  // Baseline: snapshot without firing (existing files are not "added").
  poll_now([](const WatcherEvent&) {});
  return true;
}

auto AssetWatcher::stop() noexcept -> void {
  running_ = false;
  root_.clear();
  snapshot_.clear();
}

auto AssetWatcher::poll_now(const Callback& cb) noexcept -> std::size_t {
  if (!running_) {
    return 0;
  }
  std::size_t fired = 0;
  try {
    std::unordered_map<std::string, Entry> fresh;
#if defined(_WIN32)
    std::string pattern = root_ + "/*";
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        if (fd.cFileName[0] == '.') {
          continue;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
          continue;
        }
        std::string full = root_ + "/" + fd.cFileName;
        if (!IsTracked(full)) {
          continue;
        }
        Entry e{};
        if (FileEntry(full, e)) {
          fresh.emplace(full, e);
        }
      } while (FindNextFileA(h, &fd));
      FindClose(h);
    }
#else
    DIR* d = ::opendir(root_.c_str());
    if (d != nullptr) {
      while (dirent* de = ::readdir(d)) {
        if (de->d_name[0] == '.') {
          continue;
        }
        std::string full = root_ + "/" + de->d_name;
        if (!IsTracked(full)) {
          continue;
        }
        struct stat st {};
        // NOLINTNEXTLINE: stat per file, host tooling path.
        if (::stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
          continue;
        }
        Entry e{};
#if defined(__APPLE__)
        e.mtime_ns = static_cast<std::uint64_t>(st.st_mtimespec.tv_sec) *
                         1000000000ULL +
                     static_cast<std::uint64_t>(st.st_mtimespec.tv_nsec);
#else
        e.mtime_ns = static_cast<std::uint64_t>(st.st_mtim.tv_sec) *
                         1000000000ULL +
                     static_cast<std::uint64_t>(st.st_mtim.tv_nsec);
#endif
        e.size = static_cast<std::uint64_t>(st.st_size);
        fresh.emplace(full, e);
      }
      ::closedir(d);
    }
#endif
    std::vector<WatcherEvent> events;
    for (const auto& [path, e] : fresh) {
      auto it = snapshot_.find(path);
      if (it == snapshot_.end()) {
        events.push_back({WatcherEventKind::kAdded, path});
      } else if (it->second.mtime_ns != e.mtime_ns ||
                 it->second.size != e.size) {
        events.push_back({WatcherEventKind::kModified, path});
      }
    }
    for (const auto& [path, e] : snapshot_) {
      (void)e;
      if (fresh.find(path) == fresh.end()) {
        events.push_back({WatcherEventKind::kRemoved, path});
      }
    }
    std::sort(events.begin(), events.end(), [](const WatcherEvent& a,
                                               const WatcherEvent& b) {
      if (static_cast<int>(a.kind) != static_cast<int>(b.kind)) {
        // removals(2) first? No — deterministic: Added(0), Modified(1),
        // Removed(2) sorted by kind then path.
        return static_cast<int>(a.kind) < static_cast<int>(b.kind);
      }
      return a.path < b.path;
    });
    snapshot_.swap(fresh);
    for (const WatcherEvent& ev : events) {
      cb(ev);
      ++fired;
    }
  } catch (...) {
    return fired;
  }
  return fired;
}

}  // namespace engine::assets

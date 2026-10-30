// asset_watcher.hpp — M5-01: poll-based Assets/ directory watcher.
//
// Reports added/modified/removed model+texture files under a root dir.
// Poll-based (mtime + size snapshot, default 250ms cadence) by design:
//   * portable (inotify/kqueue/FSEvents all differ; CI runs headless),
//   * deterministic (tests drive poll_now() manually — no sleeping),
//   * cheap (readdir + stat of a few hundred files is microseconds).
//
// Threading: NOT thread-safe. Drive from one thread (editor main loop or
// the import pump). Callbacks fire inline during poll_now() — keep them
// short (enqueue paths, never import inline).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/core/engine_core_export.hpp"

// MSVC C4251 (std:: members in an exported class) is benign here: all
// std:: members are private and both sides build with the same toolchain.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

namespace engine::assets {

enum class WatcherEventKind : std::uint8_t {
  kAdded = 0,
  kModified = 1,
  kRemoved = 2,
};

struct ENGINE_CORE_API WatcherEvent {
  WatcherEventKind kind = WatcherEventKind::kAdded;
  std::string path;  // absolute, as scanned
};

class ENGINE_CORE_API AssetWatcher {
 public:
  AssetWatcher() = default;
  AssetWatcher(const AssetWatcher&) = delete;
  auto operator=(const AssetWatcher&) -> AssetWatcher& = delete;
  ~AssetWatcher() = default;

  using Callback = std::function<void(const WatcherEvent&)>;

  // Start watching `root` (created when missing). Returns false when the
  // root cannot be created or read. Rescans from scratch.
  [[nodiscard]] auto start(const std::string& root) noexcept -> bool;
  auto stop() noexcept -> void;
  [[nodiscard]] auto running() const noexcept -> bool { return running_; }
  [[nodiscard]] auto root() const noexcept -> const std::string& {
    return root_;
  }

  // Single poll: diff the directory against the snapshot, fire `cb` once
  // per change (sorted: removals, additions, modifications — deterministic
  // order for tests). Returns events fired. Never throws.
  auto poll_now(const Callback& cb) noexcept -> std::size_t;

  // Tracked asset count (for tests/diagnostics).
  [[nodiscard]] auto tracked_count() const noexcept -> std::size_t {
    return snapshot_.size();
  }

 private:
  struct Entry {
    std::uint64_t mtime_ns = 0;
    std::uint64_t size = 0;
  };
#if defined(_WIN32)
  // MSVC rejects private-nested access from file-local helpers under /WX,
  // so the Windows stat helper is a friend declared up front.
  friend auto watcher_detail::FileEntry(const std::string& path, Entry& e)
      -> bool;
#endif

  // Tracked extensions: glb gltf png jpg jpeg (lowercased compare).
  static auto IsTracked(std::string_view path) noexcept -> bool;

  std::string root_;
  bool running_ = false;
  std::unordered_map<std::string, Entry> snapshot_;
};

#if defined(_WIN32)
namespace watcher_detail {
auto FileEntry(const std::string& path, struct AssetWatcher::Entry& e)
    -> bool;
}  // namespace watcher_detail
#endif

}  // namespace engine::assets

#ifdef _MSC_VER
#pragma warning(pop)
#endif

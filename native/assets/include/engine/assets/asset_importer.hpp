// asset_importer.hpp — M5-01: non-blocking import pipeline.
//
// Two-lane design (the whole point of M5-01):
//
//   FAST LANE (calling thread, <16ms): ImportFast() mmaps the file,
//     parses ONLY the container header (ParseModelPreview /
//     ParseImagePreview), ensures the .meta GUID, and returns a FastAsset
//     the renderer can show THIS frame (uncompressed staging upload).
//     No allocation on success beyond the returned record; no meshopt,
//     no BC7, no waiting.
//
//   BACKGROUND LANE (worker thread): ImportWorker::enqueue() takes the
//     source path + GUID + kind and runs the expensive jobs off-thread:
//       model   -> cgltf full parse -> meshopt vertex-cache optimize,
//                  overdraw optimize, LOD chain (simplify) -> artifact
//       texture -> stb_image decode -> mip chain -> bc7enc (or ASTC) ->
//                  artifact
//     then writes Library/ArtifactCache/<guid-hex>/<guid-hex>.<ext>.
//     Completion is reported via a lock-free-ish finished queue the editor
//     drains on its main loop (poll_finished) — the renderer hot-swaps
//     the preview upload for the compressed artifact, no modal anywhere.
//
// The worker runs on ONE std::thread (import is I/O + batch math, not a
// frame job — it must never steal enkiTS frame workers). Cancellation is
// cooperative at job boundaries; shutdown() joins.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "engine/assets/asset_fast_preview.hpp"
#include "engine/assets/asset_guid.hpp"
#include "engine/core/engine_core_export.hpp"

// MSVC C4251 (std:: members in an exported class) is benign here: all
// std:: members are private and both sides build with the same toolchain.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

namespace engine::assets {

// Result of the fast lane: everything the viewport needs for frame N.
struct ENGINE_CORE_API FastAsset {
  AssetMeta meta;  // GUID (+ kind + settings), sidecar ensured on disk
  bool is_model = false;
  ModelPreview model{};
  ImagePreview image{};
  // Nanoseconds spent in map+parse (excluding .meta I/O), for the <16ms
  // gate assertion in tests/benchmarks.
  std::uint64_t fast_ns = 0;
};

// Background job outcome, delivered via poll_finished().
struct ENGINE_CORE_API ImportResult {
  AssetGuid guid{};
  std::string source_path;
  bool ok = false;
  std::string artifact_path;  // empty on failure
  std::string error;          // empty on success
  std::uint64_t elapsed_ns = 0;
  // Model stats (0 for textures).
  std::uint32_t input_triangles = 0;
  std::uint32_t output_triangles = 0;  // LOD0 after optimize
  std::uint32_t lod_levels = 0;
  // Texture stats (0 for models).
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

// Fast lane: map + header-parse + .meta ensure. Returns false when the
// file is missing/malformed (out untouched). Never blocks on compression.
[[nodiscard]] auto ImportFast(const std::string& asset_path,
                              FastAsset& out) noexcept -> bool;

// Artifact layout: <library_root>/<guid-hex[0:2]>/<guid-hex>.<ext>.
// (Two-char shard prefix keeps one directory from holding 100k files.)
[[nodiscard]] auto ArtifactPathFor(const std::string& library_root,
                                   const AssetGuid& guid,
                                   std::string_view ext) -> std::string;

class ENGINE_CORE_API ImportWorker {
 public:
  ImportWorker() = default;
  ImportWorker(const ImportWorker&) = delete;
  auto operator=(const ImportWorker&) -> ImportWorker& = delete;
  ~ImportWorker() { shutdown(); }

  // Start the background thread. library_root =
  // <project>/Library/ArtifactCache. Idempotent; false on double-start.
  auto start(const std::string& library_root) -> bool;
  auto shutdown() -> void;
  [[nodiscard]] auto running() const noexcept -> bool {
    return running_.load(std::memory_order_acquire);
  }

  // Queue one source file. Cheap: dedupes in-flight GUIDs, returns false
  // when not running. The worker ensures the .meta itself (so callers can
  // enqueue raw drops without a prior ImportFast).
  auto enqueue(const std::string& asset_path) -> bool;

  // Drain finished jobs (non-blocking, calling thread). Returns count.
  auto poll_finished(const std::function<void(const ImportResult&)>& cb)
      -> std::size_t;

  [[nodiscard]] auto pending_count() const noexcept -> std::size_t;
  [[nodiscard]] auto library_root() const noexcept -> const std::string& {
    return library_root_;
  }

 private:
  auto ThreadMain() -> void;
  auto ProcessOne(const std::string& asset_path, ImportResult& out) -> void;

  std::string library_root_;
  std::thread thread_;
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_{false};
  mutable std::mutex mutex_;
  std::queue<std::string> pending_;
  std::vector<ImportResult> finished_;
  std::vector<std::string> in_flight_;
};

}  // namespace engine::assets

#ifdef _MSC_VER
#pragma warning(pop)
#endif

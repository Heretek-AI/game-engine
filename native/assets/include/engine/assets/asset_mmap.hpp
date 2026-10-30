// asset_mmap.hpp — M5-01: read-only memory-mapped source assets.
//
// The <16ms gate: when a .glb/.png lands in Assets/, the editor must show
// it in the viewport within one frame. A memory map gives us that — no
// fread, no copy, no decompress on the fast path; the OS pages the file
// in on first touch and the header parsers below read straight from the
// mapped bytes.
//
// Lifetime: MappedFile is move-only RAII; unmap on destroy (munmap /
// UnmapViewOfFile). Zero-length files fail to map (fail closed).
// Threading: mapping is immutable + shareable across threads; close the
// handle promptly (POSIX keeps pages after close; Windows needs the file
// mapping handle alive — MappedFile holds it).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "engine/assets/engine_assets_export.hpp"

namespace engine::assets {

class MappedFile {
 public:
  MappedFile() noexcept = default;
  MappedFile(const MappedFile&) = delete;
  auto operator=(const MappedFile&) -> MappedFile& = delete;
  MappedFile(MappedFile&& o) noexcept;
  auto operator=(MappedFile&& o) noexcept -> MappedFile&;
  ~MappedFile() noexcept;

  // Map the whole file read-only. Returns false on any error (missing,
  // empty, no permission). Never throws.
  [[nodiscard]] auto map(const std::string& path) noexcept -> bool;
  auto unmap() noexcept -> void;

  [[nodiscard]] auto valid() const noexcept -> bool { return data_ != nullptr; }
  [[nodiscard]] auto data() const noexcept -> const std::uint8_t* {
    return data_;
  }
  [[nodiscard]] auto size() const noexcept -> std::size_t { return size_; }

  // File modification time (nanoseconds since epoch, best-effort; 0 when
  // unknown). The watcher uses it for change detection.
  [[nodiscard]] auto mtime_ns() const noexcept -> std::uint64_t {
    return mtime_ns_;
  }

 private:
#if defined(_WIN32)
  void* file_handle_{nullptr};
  void* map_handle_{nullptr};
#else
  int fd_{-1};
#endif
  const std::uint8_t* data_{nullptr};
  std::size_t size_{0};
  std::uint64_t mtime_ns_{0};
};

}  // namespace engine::assets

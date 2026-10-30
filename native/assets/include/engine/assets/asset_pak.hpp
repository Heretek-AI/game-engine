// asset_pak.hpp — M5-04: memory-mappable `.pak` archive (GUID-keyed).
//
// Format (ENGPAK, little-endian, all integers LE):
//   header  magic[8]="ENGPAK1\0" version u16=1 flags u16 entry_count u32
//           toc_offset u64 (byte offset of TOC from file start)
//   body    entry blobs back-to-back (each optionally LZ4-block-compressed)
//   TOC     entry_count records, SORTED by guid bytes (binary search):
//           guid[16] kind u8 codec u8 reserved u16
//           name_hash u64 (FNV-1a of source filename, diagnostic only)
//           offset u64 packed_size u64 unpacked_size u64 crc32 u32
//           reserved2 u32
//
// Codecs: 0 = stored (raw bytes), 1 = LZ4 block (wire-compatible with the
// LZ4 block format: token/literals/offset/match sequences; any third-party
// LZ4 decoder can decompress a codec-1 blob given its unpacked size).
//
// Memory-map: the reader mmaps the file read-only and serves entry data as
// spans into the mapping (zero-copy for stored entries; one malloc for
// LZ4-decompressed entries — export/boot path only, never the frame path).
// CRC32 (zlib poly) is verified on read.
//
// Threading: PakReader is immutable after open (shareable); PakBuilder is
// single-threaded build-time only. Noexcept where noted; open/pack report
// errors via bool + std::string& err (host tooling, exceptions allowed but
// unused).
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/assets/asset_guid.hpp"
#include "engine/assets/engine_assets_export.hpp"

// MSVC C4251 (std:: members in an exported class) is benign here: all
// std:: members are private and both sides build with the same toolchain.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

namespace engine::assets {

// TOC entry (in-memory; serialized explicitly as a 64-byte record — never
// memcpy'd as a struct: AssetGuid's array alignment pads sizeof to 72).
struct PakTocEntry {
  AssetGuid guid{};
  std::uint8_t kind = 0;   // AssetKind as u8
  std::uint8_t codec = 0;  // 0=stored, 1=LZ4 block
  std::uint16_t reserved = 0;
  std::uint64_t name_hash = 0;
  std::uint64_t offset = 0;
  std::uint64_t packed_size = 0;
  std::uint64_t unpacked_size = 0;
  std::uint32_t crc32 = 0;  // of UNPACKED bytes
  std::uint32_t reserved2 = 0;
  std::uint8_t tail_pad[4] = {};  // explicit: on-disk record is 64 bytes
};
inline constexpr std::size_t kPakTocRecordSize = 64;

inline constexpr std::uint8_t kPakCodecStored = 0;
inline constexpr std::uint8_t kPakCodecLz4 = 1;

inline constexpr std::size_t kPakMagicLen = 8;
// "ENGPAK1\0" (7 chars + NUL).
inline constexpr char kPakMagic[kPakMagicLen + 1] = "ENGPAK1";
inline constexpr std::uint16_t kPakVersion = 1;

// FNV-1a 64 (diagnostic name hash + GUID-sort tiebreak stability).
[[nodiscard]] auto ENGINE_ASSETS_API
PakHashName(std::string_view name) noexcept -> std::uint64_t;

// CRC32 (zlib polynomial) over raw bytes.
[[nodiscard]] auto ENGINE_ASSETS_API
PakCrc32(const std::uint8_t* data, std::size_t size) noexcept -> std::uint32_t;

// --- LZ4 block codec (self-contained, wire-compatible) ----------------------
// Compress src into LZ4 block format. Returns false only on invalid args.
// When incompressible, emits a stored-style all-literals stream (still a
// valid LZ4 block: single literal run) — output may exceed input by ~0.4%.
// Decompress requires the exact unpacked size (from TOC); returns false on
// corrupt input or size mismatch (overrun/underrun guarded, no OOB read).
[[nodiscard]] auto ENGINE_ASSETS_API PakLz4Compress(const std::uint8_t* src,
                                                   std::size_t src_size,
                                                   std::vector<std::uint8_t>& out)
    -> bool;
[[nodiscard]] auto ENGINE_ASSETS_API PakLz4Decompress(const std::uint8_t* src,
                                                     std::size_t src_size,
                                                     std::uint8_t* dst,
                                                     std::size_t dst_size) -> bool;
// Upper bound for the compressed size (src + src/255 + 16).
[[nodiscard]] auto ENGINE_ASSETS_API
PakLz4Bound(std::size_t src_size) noexcept -> std::size_t;

// --- Builder (single-threaded, build time) ----------------------------------
class ENGINE_ASSETS_API PakBuilder {
 public:
  PakBuilder() = default;
  PakBuilder(const PakBuilder&) = delete;
  auto operator=(const PakBuilder&) -> PakBuilder& = delete;
  ~PakBuilder() = default;

  // Queue one asset's raw bytes under its GUID. compress=true tries LZ4 and
  // keeps stored when compression does not pay (<8% saving). Returns false
  // on duplicate GUID or empty payload.
  auto add(AssetGuid guid, AssetKind kind, std::string_view source_name,
           const std::uint8_t* data, std::size_t size, bool compress) -> bool;

  // Convenience: read a whole file and add it (false on I/O error).
  auto add_file(AssetGuid guid, AssetKind kind, const std::string& path,
                bool compress) -> bool;

  [[nodiscard]] auto entry_count() const noexcept -> std::size_t {
    return entries_.size();
  }
  [[nodiscard]] auto unpacked_bytes() const noexcept -> std::size_t {
    return unpacked_total_;
  }

  // Write the archive (TOC sorted by GUID). False on I/O error.
  auto write(const std::string& pak_path, std::string& err) -> bool;

 private:
  struct Pending {
    PakTocEntry meta{};
    std::vector<std::uint8_t> blob;  // packed bytes
  };
  std::vector<Pending> entries_{};
  std::size_t unpacked_total_ = 0;
};

// --- Reader (mmap-backed, shareable, cold-boot fast) -------------------------
class ENGINE_ASSETS_API PakReader {
 public:
  PakReader() = default;
  PakReader(const PakReader&) = delete;
  auto operator=(const PakReader&) -> PakReader& = delete;
  PakReader(PakReader&&) noexcept;
  auto operator=(PakReader&&) noexcept -> PakReader&;
  ~PakReader() noexcept;

  // Map + verify header/TOC bounds (CRC checked per entry on read, not open
  // — cold boot stays page-in cheap). Returns false with err set.
  auto open(const std::string& pak_path, std::string& err) -> bool;
  auto close() noexcept -> void;
  [[nodiscard]] auto is_open() const noexcept -> bool { return data_ != nullptr; }

  [[nodiscard]] auto entry_count() const noexcept -> std::size_t {
    return entry_count_;
  }

  // Binary search by GUID. Returns false when absent.
  [[nodiscard]] auto find(const AssetGuid& guid, PakTocEntry& out) const noexcept
      -> bool;

  // Read + CRC-verify one entry. Stored entries return a zero-copy span into
  // the mapping (no allocation); LZ4 entries decompress into `scratch`
  // (caller-provided; resized as needed) and the span views `scratch`.
  // Returns false on missing entry, corrupt blob, or CRC mismatch.
  [[nodiscard]] auto read(const AssetGuid& guid,
                          std::vector<std::uint8_t>& scratch,
                          std::span<const std::uint8_t>& out) const -> bool;

  // Enumerate TOC (read-only views, GUID-sorted).
  [[nodiscard]] auto entries() const noexcept
      -> std::span<const PakTocEntry> {
    return {toc_, entry_count_};
  }

 private:
  const std::uint8_t* data_ = nullptr;
  std::size_t file_size_ = 0;
  const PakTocEntry* toc_ = nullptr;
  std::size_t entry_count_ = 0;
#if defined(_WIN32)
  void* file_handle_ = nullptr;
  void* map_handle_ = nullptr;
#else
  int fd_ = -1;
#endif
};

#ifdef _MSC_VER
#pragma warning(pop)  // C4251 scoped to this header.
#endif

}  // namespace engine::assets

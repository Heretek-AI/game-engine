// asset_pak.cpp — M5-04: GUID-keyed `.pak` archive with LZ4 block codec.
//
// The LZ4 block codec below is an original implementation of the public
// LZ4 block format (token / literal-run / offset / match-length, 64KB
// sliding window, 12-bit hash chains). Wire-compatible: a codec-1 blob
// decompresses with any third-party LZ4 decoder given the unpacked size,
// and PakLz4Decompress accepts any conforming LZ4 block stream. No
// third-party code is vendored; no new CMake dependency.
//
// NOTE: the assets module builds WITHOUT exceptions/RTTI under hot flags?
// No — assets use engine_apply_base_flags (host tooling), so std::vector,
// std::string, fstream are all available. Frame-path rules (no malloc)
// do not apply: the pak path runs at export/boot time only.

#include "engine/assets/asset_pak.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <filesystem>
#include <fstream>

namespace engine::assets {
namespace {

constexpr std::size_t kLz4MinMatch = 4;           // min match length
constexpr std::size_t kLz4HashBits = 12;          // 4K hash table
constexpr std::size_t kLz4HashSize = 1u << 12;
constexpr std::size_t kLz4SkipTrigger = 6;        // skip acceleration trigger
constexpr std::int32_t kLz4TableEmpty = -1;       // hash-table vacant slot

// 4-byte little-endian load (alignment-safe).
inline auto Load32(const std::uint8_t* p) noexcept -> std::uint32_t {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}

inline auto HashPos(const std::uint8_t* p) noexcept -> std::uint32_t {
  // LZ4's Fibonacci hash of the 4-byte sequence.
  return (Load32(p) * 2654435761u) >> (32 - kLz4HashBits);
}

inline auto U16At(const std::uint8_t* p) noexcept -> std::uint32_t {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8);
}

inline auto CrcTable() noexcept -> const std::uint32_t* {
  // Magic static: thread-safe one-time init (C++11); PakReader::read is
  // shareable across threads, so the old bool-guard raced (CodeRabbit).
  static const std::array<std::uint32_t, 256> table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      t[i] = c;
    }
    return t;
  }();
  return table.data();
}

inline void WriteU16(std::uint8_t* p, std::uint16_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v & 0xFF);
  p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
}

inline void WriteU32(std::uint8_t* p, std::uint32_t v) noexcept {
  p[0] = static_cast<std::uint8_t>(v & 0xFF);
  p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
  p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
  p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
}

inline void WriteU64(std::uint8_t* p, std::uint64_t v) noexcept {
  for (int i = 0; i < 8; ++i) {
    p[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
  }
}

inline auto ReadU16(const std::uint8_t* p) noexcept -> std::uint16_t {
  return static_cast<std::uint16_t>(p[0] |
                                    (static_cast<std::uint16_t>(p[1]) << 8));
}

inline auto ReadU32(const std::uint8_t* p) noexcept -> std::uint32_t {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}

inline auto ReadU64(const std::uint8_t* p) noexcept -> std::uint64_t {
  std::uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
  }
  return v;
}

}  // namespace

auto PakHashName(std::string_view name) noexcept -> std::uint64_t {
  // FNV-1a 64.
  std::uint64_t h = 14695981039346656037ull;
  for (char c : name) {
    h ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
    h *= 1099511628211ull;
  }
  return h;
}

auto PakCrc32(const std::uint8_t* data, std::size_t size) noexcept
    -> std::uint32_t {
  if (data == nullptr) {
    return 0;
  }
  const std::uint32_t* t = CrcTable();
  std::uint32_t c = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < size; ++i) {
    c = t[(c ^ data[i]) & 0xFF] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

auto PakLz4Bound(std::size_t src_size) noexcept -> std::size_t {
  return src_size + src_size / 255 + 16;
}

static auto EmitSequence(std::vector<std::uint8_t>& out,
                         const std::uint8_t* lit_start, std::size_t lit_len,
                         std::size_t match_off, std::size_t match_len) noexcept
    -> void {
  // Token: high nibble literal length, low nibble (match_len - 4).
  std::size_t token_pos = out.size();
  out.push_back(0);
  std::uint8_t token = 0;
  if (lit_len >= 15) {
    token |= 0xF0;
  } else {
    token |= static_cast<std::uint8_t>(lit_len << 4);
  }
  std::size_t m = match_len >= kLz4MinMatch ? match_len - kLz4MinMatch : 0;
  if (m >= 15) {
    token |= 0x0F;
  } else {
    token |= static_cast<std::uint8_t>(m);
  }
  out[token_pos] = token;
  if (lit_len >= 15) {
    std::size_t len = lit_len - 15;
    while (len >= 255) {
      out.push_back(255);
      len -= 255;
    }
    out.push_back(static_cast<std::uint8_t>(len));
  }
  out.insert(out.end(), lit_start, lit_start + lit_len);
  if (match_len >= kLz4MinMatch) {
    out.push_back(static_cast<std::uint8_t>(match_off & 0xFF));
    out.push_back(static_cast<std::uint8_t>((match_off >> 8) & 0xFF));
    if (m >= 15) {
      std::size_t len = m - 15;
      while (len >= 255) {
        out.push_back(255);
        len -= 255;
      }
      out.push_back(static_cast<std::uint8_t>(len));
    }
  }
}

// One match attempt at ip: hash lookup, window/prefix guards, backward +
// forward extension, sequence emit, hash-tail fill, anchor/ip advance.
// Returns true when a match was emitted (skip resets); false advances ip
// by the caller (skip accelerates). Keeps PakLz4Compress flat for Sonar.
static auto TryEmitMatch(const std::uint8_t* src, const std::uint8_t* anchor,
                         const std::uint8_t*& ip,
                         const std::uint8_t* match_limit,
                         const std::uint8_t* match_ext_end,
                         std::vector<std::int32_t>& table,
                         std::vector<std::uint8_t>& out, std::size_t& skip)
    -> bool {
  std::uint32_t h = HashPos(ip);
  std::int32_t ref = table[h];
  // Record current position (truncate: asset blobs never approach 2GB).
  std::size_t cur = static_cast<std::size_t>(ip - src);
  table[h] = cur > 0x7FFFFFFF ? kLz4TableEmpty : static_cast<std::int32_t>(cur);
  if (ref == kLz4TableEmpty) {
    return false;
  }
  const std::uint8_t* match = src + static_cast<std::size_t>(ref);
  std::size_t ref_sz = static_cast<std::size_t>(ref);
  if (match + kLz4MinMatch > ip || cur == ref_sz || cur - ref_sz > 0xFFFF ||
      Load32(match) != Load32(ip)) {
    return false;
  }
  const std::uint8_t* m_start = match;
  const std::uint8_t* i_start = ip;
  while (i_start > anchor && m_start > src && m_start[-1] == i_start[-1]) {
    --m_start;
    --i_start;
  }
  const std::uint8_t* m_end = match + kLz4MinMatch;
  const std::uint8_t* i_end = ip + kLz4MinMatch;
  while (i_end < match_ext_end && *m_end == *i_end) {
    ++m_end;
    ++i_end;
  }
  EmitSequence(out, anchor, static_cast<std::size_t>(i_start - anchor),
               static_cast<std::size_t>(i_start - m_start),
               static_cast<std::size_t>(i_end - i_start));
  if (i_end < match_limit) {
    table[HashPos(i_end - 2)] = static_cast<std::int32_t>((i_end - 2) - src);
  }
  // C++17: anchor is caller-owned; report the new anchor through ip and
  // let the caller resync (anchor follows ip). Return the end for advance:
  ip = i_end;
  skip = 0;
  return true;
}

auto PakLz4Compress(const std::uint8_t* src, std::size_t src_size,
                    std::vector<std::uint8_t>& out) -> bool {
  if (src == nullptr && src_size != 0) {
    return false;
  }
  out.clear();
  if (src_size == 0) {
    return true;  // empty block: no sequences
  }
  out.reserve(PakLz4Bound(src_size));

  // Hash table of last positions (int32, -1 = empty). Positions capped to
  // < 2^31 by chunking huge inputs? src_size fits size_t; positions stored
  // as size_t in a parallel array to stay 64-bit clean.
  thread_local static std::vector<std::int32_t> tl_table;
  tl_table.assign(kLz4HashSize, kLz4TableEmpty);

  const std::uint8_t* const src_end = src + src_size;
  // LZ4 end-of-block rules (CodeRabbit): the last 5 bytes must be
  // literals and the last match must start >=12 bytes before the end
  // (MFLIMIT), or reference decoders (LZ4_decompress_safe) reject the
  // block. Tiny inputs go literals-only; otherwise the search horizon
  // stops 12 bytes early and forward extension stops 5 bytes early, so
  // the tail is always >=5 literals. Decoder unchanged (it already
  // accepts this subset — and now every blob we emit is reference-valid).
  constexpr std::size_t kLastLiterals = 5;
  constexpr std::size_t kMfLimit = 12;
  const std::uint8_t* const match_limit =
      (src_size <= kMfLimit) ? src : src_end - kMfLimit;
  const std::uint8_t* const match_ext_end =
      (src_size <= kLastLiterals) ? src : src_end - kLastLiterals;
  const std::uint8_t* anchor = src;
  const std::uint8_t* ip = src;
  std::size_t skip = 0;

  // Main search loop. Guarded by match_limit > src so tiny inputs
  // (src_size <= kMfLimit, match_limit == src) go literals-only:
  // otherwise HashPos->Load32 would read 4 bytes from a 1-3 byte buffer.
  while (ip <= match_limit && match_limit > src) {
    const std::uint8_t* before = ip;
    if (TryEmitMatch(src, anchor, ip, match_limit, match_ext_end, tl_table,
                     out, skip)) {
      anchor = ip;
    } else {
      ip = before + 1 + (skip++ >> kLz4SkipTrigger);
    }
  }
  // Last literals.
  std::size_t tail = static_cast<std::size_t>(src_end - anchor);
  if (tail > 0) {
    std::size_t token_pos = out.size();
    out.push_back(0);
    std::uint8_t token = 0;
    if (tail >= 15) {
      token |= 0xF0;
    } else {
      token |= static_cast<std::uint8_t>(tail << 4);
    }
    out[token_pos] = token;  // low nibble 0: no match follows
    if (tail >= 15) {
      std::size_t len = tail - 15;
      while (len >= 255) {
        out.push_back(255);
        len -= 255;
      }
      out.push_back(static_cast<std::uint8_t>(len));
    }
    out.insert(out.end(), anchor, src_end);
  }
  return true;
}

// Variable-length extension: token nibble was 15, consume 0xFF-chained
// bytes. False on truncation or varint wrap; advances *ip past extensions.
inline auto ReadExtLen(const std::uint8_t*& ip, const std::uint8_t* ip_end,
                       std::size_t& len, std::size_t wrap_floor) noexcept
    -> bool {
  std::uint8_t b = 0;
  do {
    if (ip >= ip_end) {
      return false;
    }
    b = *ip++;
    len += b;
    if (len < wrap_floor) {  // varint wrapped
      return false;
    }
  } while (b == 255);
  return true;
}

// One LZ4 sequence: literal run, optional offset+match. Advances *ip/*op.
// False on any truncation, overrun, or bad offset (fail closed, no OOB).
inline auto DecodeSequence(std::uint8_t token, const std::uint8_t*& ip,
                           const std::uint8_t* ip_end, std::uint8_t*& op,
                           const std::uint8_t* op_end,
                           const std::uint8_t* dst) noexcept -> bool {
  std::size_t lit_len = static_cast<std::size_t>(token >> 4);
  if (lit_len == 15 && !ReadExtLen(ip, ip_end, lit_len, 15)) {
    return false;
  }
  if (lit_len > static_cast<std::size_t>(ip_end - ip) ||
      lit_len > static_cast<std::size_t>(op_end - op)) {
    return false;
  }
  if (lit_len > 0) {
    std::memcpy(op, ip, lit_len);
    ip += lit_len;
    op += lit_len;
  }
  if (ip >= ip_end) {
    // Last literals: output is complete only when exactly full.
    return op == op_end;
  }
  if (static_cast<std::size_t>(ip_end - ip) < 2) {
    return false;
  }
  std::size_t offset = U16At(ip);
  ip += 2;
  if (offset == 0 || offset > static_cast<std::size_t>(op - dst)) {
    return false;
  }
  std::size_t match_len = static_cast<std::size_t>(token & 0x0F) + 4;
  if ((token & 0x0F) == 15 && !ReadExtLen(ip, ip_end, match_len, 19)) {
    return false;
  }
  if (match_len > static_cast<std::size_t>(op_end - op)) {
    return false;
  }
  // Forward byte loop (NOT memmove): offset < match_len extends the run —
  // byte i reads byte i-offset written moments earlier. glibc memmove
  // copies overlapping dest>src regions backwards and freezes the tail.
  std::uint8_t* match_dst = op;
  const std::uint8_t* match_src = op - offset;
  for (std::size_t i = 0; i < match_len; ++i) {
    match_dst[i] = match_src[i];
  }
  op += match_len;
  return true;
}

auto PakLz4Decompress(const std::uint8_t* src, std::size_t src_size,
                      std::uint8_t* dst, std::size_t dst_size) -> bool {
  if ((src == nullptr && src_size != 0) || (dst == nullptr && dst_size != 0)) {
    return false;
  }
  if (dst_size == 0) {
    return src_size == 0;  // empty block decodes to empty
  }
  const std::uint8_t* ip = src;
  const std::uint8_t* const ip_end = src + src_size;
  std::uint8_t* op = dst;
  std::uint8_t* const op_end = dst + dst_size;
  while (ip < ip_end) {
    std::uint8_t token = *ip++;
    if (!DecodeSequence(token, ip, ip_end, op, op_end, dst)) {
      return false;
    }
    if (op == op_end) {
      // Output full: input must be exactly consumed (DecodeSequence already
      // enforces literal-exactness at block end; a mid-stream full-output
      // with trailing input is corrupt).
      return ip == ip_end;
    }
  }
  return op == op_end;
}

// --- PakBuilder -------------------------------------------------------------
auto PakBuilder::add(AssetGuid guid, AssetKind kind, std::string_view name,
                     const std::uint8_t* data, std::size_t size, bool compress)
    -> bool {
  if (guid.is_nil() || (data == nullptr && size != 0) || size == 0) {
    return false;
  }
  for (const Pending& e : entries_) {
    if (e.meta.guid == guid) {
      return false;  // duplicate
    }
  }
  Pending p{};
  p.meta.guid = guid;
  p.meta.kind = static_cast<std::uint8_t>(kind);
  p.meta.codec = kPakCodecStored;
  p.meta.name_hash = PakHashName(name);
  p.meta.unpacked_size = static_cast<std::uint64_t>(size);
  p.meta.crc32 = PakCrc32(data, size);
  if (compress) {
    std::vector<std::uint8_t> packed;
    if (PakLz4Compress(data, size, packed) && packed.size() < size &&
        (size - packed.size()) * 100 / size >= 8) {
      p.meta.codec = kPakCodecLz4;
      p.blob = std::move(packed);
    }
  }
  if (p.meta.codec == kPakCodecStored) {
    p.blob.assign(data, data + size);
  }
  p.meta.packed_size = static_cast<std::uint64_t>(p.blob.size());
  unpacked_total_ += size;
  entries_.push_back(std::move(p));
  return true;
}

auto PakBuilder::add_file(AssetGuid guid, AssetKind kind,
                          const std::string& path, bool compress) -> bool {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    return false;
  }
  std::streamsize n = f.tellg();
  if (n <= 0) {
    return false;
  }
  f.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> buf(static_cast<std::size_t>(n));
  if (!f.read(reinterpret_cast<char*>(buf.data()), n)) {
    return false;
  }
  return add(guid, kind, path, buf.data(), buf.size(), compress);
}

auto PakBuilder::write(const std::string& pak_path, std::string& err) -> bool {
  std::sort(entries_.begin(), entries_.end(),
            [](const Pending& a, const Pending& b) {
              if (a.meta.guid.bytes != b.meta.guid.bytes) {
                return a.meta.guid.bytes < b.meta.guid.bytes;
              }
              return a.meta.name_hash < b.meta.name_hash;
            });
  // Layout: header(24) + blobs + TOC(64 * N).
  constexpr std::size_t kHeaderSize = 8 + 2 + 2 + 4 + 8;  // 24
  std::uint64_t body_off = kHeaderSize;
  for (auto& e : entries_) {
    e.meta.offset = body_off;
    body_off += e.meta.packed_size;
  }
  std::uint64_t toc_off = body_off;

  std::ofstream f(pak_path, std::ios::binary | std::ios::trunc);
  if (!f) {
    err = "pak: cannot open output file: " + pak_path;
    return false;
  }
  std::uint8_t hdr[kHeaderSize]{};
  std::memcpy(hdr, kPakMagic, kPakMagicLen);
  WriteU16(hdr + 8, kPakVersion);
  WriteU16(hdr + 10, 0);  // flags
  WriteU32(hdr + 12, static_cast<std::uint32_t>(entries_.size()));
  WriteU64(hdr + 16, toc_off);
  f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
  for (const auto& e : entries_) {
    if (!e.blob.empty()) {
      f.write(reinterpret_cast<const char*>(e.blob.data()),
              static_cast<std::streamsize>(e.blob.size()));
    }
  }
  for (const auto& e : entries_) {
    std::uint8_t rec[64]{};
    std::memcpy(rec, e.meta.guid.bytes.data(), 16);
    rec[16] = e.meta.kind;
    rec[17] = e.meta.codec;
    WriteU16(rec + 18, 0);
    WriteU64(rec + 20, e.meta.name_hash);
    WriteU64(rec + 28, e.meta.offset);
    WriteU64(rec + 36, e.meta.packed_size);
    WriteU64(rec + 44, e.meta.unpacked_size);
    WriteU32(rec + 52, e.meta.crc32);
    WriteU32(rec + 56, 0);
    // rec[60..64] tail_pad zeros (struct is 64B; matches on-disk record).
    rec[60] = rec[61] = rec[62] = rec[63] = 0;
    f.write(reinterpret_cast<const char*>(rec), sizeof(rec));
  }
  f.flush();
  if (!f) {
    err = "pak: write failed: " + pak_path;
    return false;
  }
  return true;
}

// --- PakReader ---------------------------------------------------------------
PakReader::PakReader(PakReader&& o) noexcept
    : data_(o.data_),
      file_size_(o.file_size_),
      toc_(o.toc_),
      entry_count_(o.entry_count_)
#if defined(_WIN32)
      ,
      file_handle_(o.file_handle_),
      map_handle_(o.map_handle_)
#else
      ,
      fd_(o.fd_)
#endif
{
  o.data_ = nullptr;
  o.file_size_ = 0;
  o.toc_ = nullptr;
  o.entry_count_ = 0;
#if defined(_WIN32)
  o.file_handle_ = nullptr;
  o.map_handle_ = nullptr;
#else
  o.fd_ = -1;
#endif
}

auto PakReader::operator=(PakReader&& o) noexcept -> PakReader& {
  if (this != &o) {
    close();
    data_ = o.data_;
    file_size_ = o.file_size_;
    toc_ = o.toc_;
    entry_count_ = o.entry_count_;
#if defined(_WIN32)
    file_handle_ = o.file_handle_;
    map_handle_ = o.map_handle_;
    o.file_handle_ = nullptr;
    o.map_handle_ = nullptr;
#else
    fd_ = o.fd_;
    o.fd_ = -1;
#endif
    o.data_ = nullptr;
    o.file_size_ = 0;
    o.toc_ = nullptr;
    o.entry_count_ = 0;
  }
  return *this;
}

PakReader::~PakReader() noexcept { close(); }

namespace {

// RAII mapping for PakReader::open: every early-return error path used to
// repeat the platform unmap+close block (Sonar nesting/complexity). The
// guard owns the view + handles until open() commits them to the member
// (dismiss), so failures just `return false`.
// Sonar cpp:S2095 ("resource opened in MapFile never closed") is a false
// positive here by construction: MapFile stores the HANDLE/fd into this
// guard and PakReader::open commits it to the member on success (dismiss);
// on every error path the guard's destructor runs MapGuard::reset, which
// closes all three handles — there is no leak path an in-scope close
// could fix that reset does not already cover.
struct MapGuard {
#if defined(_WIN32)
  const void* view = nullptr;
  void* file_handle = nullptr;
  void* map_handle = nullptr;
#else
  const std::uint8_t* view = nullptr;
  int fd = -1;
#endif
  std::size_t fsize = 0;
  bool dismiss = false;

  MapGuard() noexcept = default;
  MapGuard(const MapGuard&) = delete;
  auto operator=(const MapGuard&) -> MapGuard& = delete;
  ~MapGuard() noexcept { reset(); }

  auto reset() noexcept -> void {
    if (dismiss) {
      return;
    }
#if defined(_WIN32)
    if (view != nullptr) {
      UnmapViewOfFile(const_cast<void*>(view));
    }
    if (map_handle != nullptr) {
      CloseHandle(static_cast<HANDLE>(map_handle));
    }
    if (file_handle != nullptr) {
      CloseHandle(static_cast<HANDLE>(file_handle));
    }
    view = nullptr;
    file_handle = nullptr;
    map_handle = nullptr;
#else
    if (view != nullptr) {
      ::munmap(const_cast<std::uint8_t*>(view), fsize);
    }
    if (fd >= 0) {
      ::close(fd);
    }
    view = nullptr;
    fd = -1;
#endif
    fsize = 0;
  }
};

}  // namespace

auto MapFile(const std::string& pak_path, MapGuard& guard, std::string& err) -> bool {
#if defined(_WIN32)
  HANDLE fh = CreateFileA(pak_path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                          nullptr);
  if (fh == INVALID_HANDLE_VALUE) {
    err = "pak: cannot open file: " + pak_path;
    return false;
  }
  guard.file_handle = fh;
  // File size via GetFileSize (DWORD halves). Sonar cpp:S6232 flags ANY
  // member access on LARGE_INTEGER (it is a union: LowPart/HighPart share
  // storage with QuadPart), including the innocent `sz.QuadPart` read after
  // GetFileSizeEx fills it. Route around the union entirely: query the size
  // with GetFileSize (DWORD halves, no union) and compose arithmetically.
  DWORD size_hi = 0;
  DWORD size_lo = GetFileSize(fh, &size_hi);
  if ((size_lo == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) ||
      (size_hi == 0 && size_lo < 24)) {
    err = "pak: file too small: " + pak_path;
    return false;
  }
  HANDLE mh = CreateFileMappingA(fh, nullptr, PAGE_READONLY, 0, 0, nullptr);
  if (mh == nullptr) {
    err = "pak: mapping failed: " + pak_path;
    return false;
  }
  guard.map_handle = mh;
  const auto* view = static_cast<const std::uint8_t*>(
      MapViewOfFile(mh, FILE_MAP_READ, 0, 0, 0));
  if (view == nullptr) {
    err = "pak: view failed: " + pak_path;
    return false;
  }
  guard.view = view;
  guard.fsize = (static_cast<std::size_t>(size_hi) << 32) | size_lo;
#else
  int fd = ::open(pak_path.c_str(), O_RDONLY);
  if (fd < 0) {
    err = "pak: cannot open file: " + pak_path;
    return false;
  }
  guard.fd = fd;
  struct stat st {};
  if (::fstat(fd, &st) != 0 || st.st_size < 24) {
    err = "pak: file too small: " + pak_path;
    return false;
  }
  guard.fsize = static_cast<std::size_t>(st.st_size);
  void* view = ::mmap(nullptr, guard.fsize, PROT_READ, MAP_PRIVATE, fd, 0);
  if (view == MAP_FAILED) {
    err = "pak: mmap failed: " + pak_path;
    return false;
  }
  guard.view = static_cast<const std::uint8_t*>(view);
#endif
  return true;
}

auto ValidateHeader(const std::uint8_t* bytes, std::size_t fsize,
                    std::uint32_t& count, std::uint64_t& toc_off,
                    const std::string& pak_path, std::string& err) -> bool {
  if (std::memcmp(bytes, kPakMagic, kPakMagicLen) != 0) {
    err = "pak: bad magic: " + pak_path;
    return false;
  }
  if (ReadU16(bytes + 8) != kPakVersion) {
    err = "pak: unsupported version: " + pak_path;
    return false;
  }
  count = ReadU32(bytes + 12);
  toc_off = ReadU64(bytes + 16);
  // Subtraction-based (CodeRabbit): toc_off/count are attacker-controlled
  // and the sum can wrap; bytes+toc_off would read outside the mapping.
  std::uint64_t toc_bytes = static_cast<std::uint64_t>(count) * 64;
  if (toc_off < 24 || toc_off > fsize || toc_bytes > fsize - toc_off) {
    err = "pak: TOC out of bounds: " + pak_path;
    return false;
  }
  return true;
}

auto ParseToc(const std::uint8_t* bytes, std::size_t fsize, std::uint32_t count,
              std::uint64_t toc_off, std::unique_ptr<PakTocEntry[]>& toc,
              const std::string& pak_path, std::string& err) -> bool {
  // Heap array (avoids alignment/aliasing reading packed records straight
  // from the mapping). unique_ptr: error paths need no manual delete
  // (Sonar S5025); released to the member only on full success.
  toc = std::unique_ptr<PakTocEntry[]>(new (std::nothrow) PakTocEntry[count]);
  if (toc == nullptr) {
    err = "pak: out of memory";
    return false;
  }
  const std::uint8_t* rec = bytes + toc_off;
  for (std::uint32_t i = 0; i < count; ++i, rec += 64) {
    PakTocEntry e{};
    std::memcpy(e.guid.bytes.data(), rec, 16);
    e.kind = rec[16];
    e.codec = rec[17];
    e.name_hash = ReadU64(rec + 20);
    e.offset = ReadU64(rec + 28);
    e.packed_size = ReadU64(rec + 36);
    e.unpacked_size = ReadU64(rec + 44);
    e.crc32 = ReadU32(rec + 52);
    if (e.codec > kPakCodecLz4 || e.offset < 24 || e.offset > fsize ||
        e.packed_size > fsize - e.offset || e.offset > toc_off ||
        e.packed_size > toc_off - e.offset) {
      toc.reset();
      err = "pak: corrupt TOC entry: " + pak_path;
      return false;
    }
    toc[i] = e;
  }
  for (std::uint32_t i = 1; i < count; ++i) {
    if (!(toc[i - 1].guid.bytes < toc[i].guid.bytes)) {
      toc.reset();
      err = "pak: TOC not sorted: " + pak_path;
      return false;
    }
  }
  return true;
}

auto PakReader::open(const std::string& pak_path, std::string& err) -> bool {
  close();
  MapGuard guard{};
  if (!MapFile(pak_path, guard, err)) {
    return false;
  }
#if defined(_WIN32)
  const std::uint8_t* bytes = static_cast<const std::uint8_t*>(guard.view);
#else
  const std::uint8_t* bytes = guard.view;
#endif
  std::uint32_t count = 0;
  std::uint64_t toc_off = 0;
  if (!ValidateHeader(bytes, guard.fsize, count, toc_off, pak_path, err)) {
    return false;
  }
  std::unique_ptr<PakTocEntry[]> toc;
  if (count > 0 &&
      !ParseToc(bytes, guard.fsize, count, toc_off, toc, pak_path, err)) {
    return false;
  }
  data_ = bytes;
  file_size_ = guard.fsize;
  toc_ = toc.release();
  entry_count_ = count;
#if defined(_WIN32)
  file_handle_ = guard.file_handle;
  map_handle_ = guard.map_handle;
#else
  fd_ = guard.fd;
#endif
  guard.dismiss = true;
#if defined(_WIN32)
  guard.view = nullptr;
  guard.file_handle = nullptr;
  guard.map_handle = nullptr;
#else
  guard.view = nullptr;
  guard.fd = -1;
#endif
  guard.fsize = 0;
  return true;
}

auto PakReader::close() noexcept -> void {
  if (data_ != nullptr) {
#if defined(_WIN32)
    UnmapViewOfFile(data_);
    if (map_handle_ != nullptr) {
      CloseHandle(static_cast<HANDLE>(map_handle_));
    }
    if (file_handle_ != nullptr) {
      CloseHandle(static_cast<HANDLE>(file_handle_));
    }
    file_handle_ = nullptr;
    map_handle_ = nullptr;
#else
    ::munmap(const_cast<std::uint8_t*>(data_), file_size_);
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = -1;
#endif
    data_ = nullptr;
    file_size_ = 0;
  }
  // NOSONAR(cpp:S5025,cpp:S8268) owner-free of the ParseToc handoff (member
  // owns the array between open/close; same opaque-handle pattern as M1-04
  // worlds and M5-02 editor snapshots).
  delete[] toc_;  // NOLINT NOSONAR(cpp:S5025,cpp:S8268) paired with ParseToc release
  toc_ = nullptr;
  entry_count_ = 0;
}

auto PakReader::find(const AssetGuid& guid, PakTocEntry& out) const noexcept
    -> bool {
  std::size_t lo = 0;
  std::size_t hi = entry_count_;
  while (lo < hi) {
    std::size_t mid = lo + (hi - lo) / 2;
    const auto& e = toc_[mid];
    if (e.guid.bytes == guid.bytes) {
      out = e;
      return true;
    }
    if (e.guid.bytes < guid.bytes) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return false;
}

auto PakReader::read(const AssetGuid& guid,
                     std::vector<std::uint8_t>& scratch,
                     std::span<const std::uint8_t>& out) const -> bool {
  PakTocEntry e{};
  if (data_ == nullptr || !find(guid, e)) {
    return false;
  }
  if (e.packed_size > file_size_ || e.unpacked_size > (1ull << 31)) {
    return false;  // sanity cap: 2GB single entry
  }
  const std::uint8_t* blob = data_ + e.offset;
  std::size_t packed = static_cast<std::size_t>(e.packed_size);
  std::size_t unpacked = static_cast<std::size_t>(e.unpacked_size);
  if (e.codec == kPakCodecStored) {
    if (PakCrc32(blob, packed) != e.crc32 || packed != unpacked) {
      return false;
    }
    out = std::span<const std::uint8_t>(blob, packed);
    return true;
  }
  if (e.codec == kPakCodecLz4) {
    scratch.resize(unpacked);
    if (!PakLz4Decompress(blob, packed, scratch.data(), unpacked)) {
      return false;
    }
    if (PakCrc32(scratch.data(), unpacked) != e.crc32) {
      return false;
    }
    out = std::span<const std::uint8_t>(scratch.data(), unpacked);
    return true;
  }
  return false;
}

}  // namespace engine::assets

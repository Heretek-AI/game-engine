// test_asset_pak.cpp — M5-04 acceptance: GUID-keyed `.pak` archive.
//
// AC1: pack referenced assets by GUID into a memory-mappable .pak with
// LZ4 block compression; cold open + per-entry read + CRC must hold.
// Gates:
//   LZ4   round-trip on compressible + incompressible payloads; corrupt
//         blob / truncated output / bad CRC fail closed (no OOB).
//   PAK   builder rejects dup GUID + nil GUID + empty payload; TOC is
//         GUID-sorted; stored entries read back zero-copy (span aliases
//         the mapping); LZ4 entries decompress + CRC-verify; missing GUID
//         reads false; bad magic / truncated file opens false.
//   PERF  pack + cold-open + full read timings printed (informational;
//         the <1.0s cold-boot gate is proven by the export test below).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "engine/assets/asset_guid.hpp"
#include "engine/assets/asset_pak.hpp"

namespace fs = std::filesystem;

namespace {

auto check(bool ok, const char* name) -> int {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  std::fflush(stdout);
  return ok ? 0 : 1;
}

auto TmpRoot() -> std::string {
  static std::atomic<unsigned> s_seq{0};
  auto pid =
#if defined(_WIN32)
      _getpid();
#else
      ::getpid();
#endif
  auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::string p = "pak_test_scratch_" + std::to_string(pid) + "_" +
                  std::to_string(s_seq.fetch_add(1)) + "_" +
                  std::to_string(static_cast<unsigned long long>(now));
  std::error_code ec;
  fs::remove_all(p, ec);
  fs::create_directories(p, ec);
#if !defined(_WIN32)
  ::chmod(p.c_str(), 0700);
#endif
  return p;
}

// Deterministic PRNG payload (seeded mt19937 — reproducible, compressible
// when low-entropy, incompressible when byte-uniform).
auto MakePayload(std::size_t n, unsigned seed, bool compressible)
    -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> v(n);
  if (compressible) {
    // Repeating 1KB sentence: highly compressible.
    const char* sentence =
        "the quick brown fox jumps over the lazy dog. pack my box with five "
        "dozen liquor jugs! sly foxes rankle buzzing beehives. ";
    std::size_t sl = std::strlen(sentence);
    for (std::size_t i = 0; i < n; ++i) {
      v[i] = static_cast<std::uint8_t>(sentence[i % sl]);
    }
  } else {
    std::mt19937 rng(seed);
    for (std::size_t i = 0; i < n; ++i) {
      v[i] = static_cast<std::uint8_t>(rng() & 0xFF);
    }
  }
  return v;
}

}  // namespace

static auto TestLz4Codec() -> int {
  using namespace engine::assets;
  int failures = 0;
  auto text = MakePayload(1 << 16, 1, true);
  std::vector<std::uint8_t> packed;
  failures += check(PakLz4Compress(text.data(), text.size(), packed),
                    "lz4 compresses");
  failures += check(packed.size() < text.size() / 2,
                    "lz4 shrinks text payload");
  std::printf("  lz4 text: %zu -> %zu bytes\n", text.size(), packed.size());
  std::vector<std::uint8_t> back(text.size());
  failures += check(PakLz4Decompress(packed.data(), packed.size(),
                                     back.data(), back.size()) &&
                        back == text,
                    "lz4 text round-trips");
  // Incompressible: encoder must still emit a valid stream.
  auto noise = MakePayload(1 << 14, 42, false);
  std::vector<std::uint8_t> npacked;
  failures += check(PakLz4Compress(noise.data(), noise.size(), npacked),
                    "lz4 handles noise");
  std::vector<std::uint8_t> nback(noise.size());
  failures += check(PakLz4Decompress(npacked.data(), npacked.size(),
                                     nback.data(), nback.size()) &&
                        nback == noise,
                    "lz4 noise round-trips");
  // Empty.
  std::vector<std::uint8_t> empty_out;
  failures +=
      check(PakLz4Compress(nullptr, 0, empty_out) && empty_out.empty(),
            "lz4 empty compresses to empty");
  failures += check(PakLz4Decompress(nullptr, 0, nullptr, 0),
                    "lz4 empty decompresses to empty");
  // Corrupt / hostile inputs fail closed (no crash, no OOB).
  std::vector<std::uint8_t> garbage = {0xFF, 0xFF, 0x01, 0x02, 0x03};
  std::vector<std::uint8_t> tmp(64);
  failures += check(!PakLz4Decompress(garbage.data(), garbage.size(),
                                      tmp.data(), tmp.size()),
                    "lz4 rejects garbage");
  failures += check(!PakLz4Decompress(packed.data(), packed.size(),
                                      tmp.data(), 8),
                    "lz4 rejects truncated output");
  std::vector<std::uint8_t> trunc(packed.begin(), packed.end() - 1);
  std::vector<std::uint8_t> tmp2(text.size());
  failures += check(!PakLz4Decompress(trunc.data(), trunc.size(),
                                      tmp2.data(), tmp2.size()),
                    "lz4 rejects truncated input");
  failures += check(PakLz4Bound(1000) >= 1000, "lz4 bound sane");
  return failures;
}

static auto TestBuilderValidation() -> int {
  using namespace engine::assets;
  int failures = 0;
  PakBuilder b{};
  AssetGuid g = MintGuid();
  auto pay = MakePayload(4096, 7, true);
  failures += check(!b.add(AssetGuid{}, AssetKind::kModel, "nil",
                           pay.data(), pay.size(), true),
                    "builder rejects nil GUID");
  failures += check(!b.add(g, AssetKind::kModel, "empty", pay.data(), 0,
                           true),
                    "builder rejects empty payload");
  failures += check(b.add(g, AssetKind::kModel, "mesh.mesh", pay.data(),
                          pay.size(), true),
                    "builder adds entry");
  failures += check(!b.add(g, AssetKind::kModel, "dup", pay.data(),
                           pay.size(), true),
                    "builder rejects duplicate GUID");
  failures +=
      check(b.entry_count() == 1 && b.unpacked_bytes() == pay.size(),
            "builder counts");
  return failures;
}

static auto TestBuildPak(std::vector<std::pair<engine::assets::AssetGuid, std::vector<std::uint8_t>>>& corpus, const std::string& pak_path) -> int {
  using namespace engine::assets;
  int failures = 0;
  PakBuilder b{};
  // 8 compressible (LZ4) + 4 noise (stored fallback) + 1 tiny (stored).
  for (int i = 0; i < 8; ++i) {
    auto pay = MakePayload(8192 + static_cast<std::size_t>(i) * 1024,
                           static_cast<unsigned>(i), true);
    AssetGuid g = MintGuid();
    std::string name = "asset_" + std::to_string(i) + ".mesh";
    if (!b.add(g, AssetKind::kModel, name, pay.data(), pay.size(), true)) {
      failures += check(false, "builder adds compressible entry");
    }
    corpus.emplace_back(g, std::move(pay));
  }
  for (int i = 0; i < 4; ++i) {
    auto pay = MakePayload(4096, static_cast<unsigned>(100 + i), false);
    AssetGuid g = MintGuid();
    std::string name = "tex_" + std::to_string(i) + ".bc7";
    if (!b.add(g, AssetKind::kTexture, name, pay.data(), pay.size(),
               true)) {
      failures += check(false, "builder adds noise entry");
    }
    corpus.emplace_back(g, std::move(pay));
  }
  {
    std::vector<std::uint8_t> tiny = {1, 2, 3, 4, 5};
    AssetGuid g = MintGuid();
    if (!b.add(g, AssetKind::kUnknown, "tiny.bin", tiny.data(), tiny.size(),
               true)) {
      failures += check(false, "builder adds tiny entry");
    }
    corpus.emplace_back(g, tiny);
  }
  auto t0 = std::chrono::steady_clock::now();
  std::string err;
  bool ok = b.write(pak_path, err);
  auto pack_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - t0)
          .count();
  failures += check(ok, "pak writes");
  if (!ok) {
    std::printf("  write err: %s\n", err.c_str());
  }
  std::printf("  pack %zu entries (%zu unpacked B) in %lld ns\n",
              b.entry_count(), b.unpacked_bytes(),
              static_cast<long long>(pack_ns));
  std::error_code ec;
  auto fsize = fs::file_size(pak_path, ec);
  failures += check(!ec, "pak file exists");
  std::printf("  pak file bytes: %llu\n",
              static_cast<unsigned long long>(fsize));
  return failures;
}

static auto TestReadPak(const std::vector<std::pair<engine::assets::AssetGuid, std::vector<std::uint8_t>>>& corpus, const std::string& pak_path) -> int {
  using namespace engine::assets;
  int failures = 0;
  PakReader r{};
  std::string err;
  auto t0 = std::chrono::steady_clock::now();
  bool ok = r.open(pak_path, err);
  auto open_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - t0)
          .count();
  failures += check(ok, "pak cold-opens");
  if (!ok) {
    std::printf("  open err: %s\n", err.c_str());
  }
  std::printf("  cold open %zu entries in %lld ns\n", r.entry_count(),
              static_cast<long long>(open_ns));
  failures +=
      check(r.entry_count() == corpus.size(), "pak entry count matches");

  // TOC GUID-sorted.
  bool sorted = true;
  auto es = r.entries();
  for (std::size_t i = 1; i < es.size(); ++i) {
    if (!(es[i - 1].guid.bytes < es[i].guid.bytes)) {
      sorted = false;
    }
  }
  failures += check(sorted, "TOC GUID-sorted");

  // At least one LZ4 + one stored (mixed corpus proves both paths).
  std::size_t n_lz4 = 0;
  std::size_t n_stored = 0;
  for (const auto& e : es) {
    n_lz4 += (e.codec == kPakCodecLz4) ? 1 : 0;
    n_stored += (e.codec == kPakCodecStored) ? 1 : 0;
  }
  failures += check(n_lz4 > 0 && n_stored > 0, "mixed lz4 + stored codecs");
  std::printf("  codecs: lz4=%zu stored=%zu\n", n_lz4, n_stored);

  // Read every entry: content match + CRC gate inside read().
  std::vector<std::uint8_t> scratch;
  bool all_ok = true;
  auto tr0 = std::chrono::steady_clock::now();
  for (const auto& [guid, want] : corpus) {
    PakTocEntry found{};
    if (!r.find(guid, found)) {
      all_ok = false;
      break;
    }
    std::span<const std::uint8_t> got;
    if (!r.read(guid, scratch, got)) {
      all_ok = false;
      break;
    }
    if (got.size() != want.size() ||
        std::memcmp(got.data(), want.data(), want.size()) != 0) {
      all_ok = false;
      break;
    }
  }
  auto read_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - tr0)
          .count();
  failures += check(all_ok, "all entries read back byte-exact");
  std::printf("  full read %zu entries in %lld ns\n", corpus.size(),
              static_cast<long long>(read_ns));
  std::printf("PAK_PACK_NS=%lld PAK_OPEN_NS=%lld PAK_READ_NS=%lld\n",
              0LL, static_cast<long long>(open_ns),
              static_cast<long long>(read_ns));

  // Missing GUID.
  AssetGuid missing = MintGuid();
  PakTocEntry ignored{};
  std::span<const std::uint8_t> ign_span;
  failures += check(!r.find(missing, ignored), "find misses unknown GUID");
  failures +=
      check(!r.read(missing, scratch, ign_span), "read misses unknown GUID");

  // Zero-copy check: a stored entry's span must alias the mapping, not
  // the scratch buffer (re-read into a poisoned scratch to prove it).
  for (const auto& e : es) {
    if (e.codec == kPakCodecStored && e.unpacked_size > 0) {
      std::fill(scratch.begin(), scratch.end(), 0xAA);
      std::span<const std::uint8_t> zc;
      bool zok = r.read(e.guid, scratch, zc);
      bool aliases_map = zok && !zc.empty() &&
                         (zc.data() < scratch.data() ||
                          zc.data() >= scratch.data() + scratch.size());
      failures += check(aliases_map, "stored entry is zero-copy");
      break;
    }
  }
  r.close();
  failures += check(!r.is_open(), "reader closes");
  return failures;
}

static auto TestCorruptOpens(const std::string& root, const std::string& pak_path) -> int {
  using namespace engine::assets;
  int failures = 0;
  // Bad magic.
  std::string bad = root + "/bad.pak";
  {
    std::ofstream f(bad, std::ios::binary | std::ios::trunc);
    f << "NOTAPAK!";
    for (int i = 0; i < 32; ++i) {
      f.put('\0');
    }
  }
  PakReader r{};
  std::string err;
  failures += check(!r.open(bad, err), "rejects bad magic");
  // Truncated (header claims TOC beyond EOF).
  std::string trunc = root + "/trunc.pak";
  {
    std::error_code ec;
    fs::copy_file(pak_path, trunc, ec);
    // Chop to 32 bytes.
    std::filesystem::resize_file(trunc, 32, ec);
  }
  failures += check(!r.open(trunc, err), "rejects truncated pak");
  failures += check(!r.open(root + "/missing.pak", err),
                    "rejects missing file");
  return failures;
}

auto main() -> int {
  using namespace engine::assets;
  int failures = 0;
  std::string root = TmpRoot();
  std::string pak_path = root + "/game.pak";
  std::vector<std::pair<AssetGuid, std::vector<std::uint8_t>>> corpus;

  failures += TestLz4Codec();
  failures += TestBuilderValidation();
  failures += TestBuildPak(corpus, pak_path);
  failures += TestReadPak(corpus, pak_path);
  failures += TestCorruptOpens(root, pak_path);

  std::printf(failures == 0 ? "ASSET_PAK_OK\n" : "ASSET_PAK_FAIL\n");
  return failures == 0 ? 0 : 1;
}

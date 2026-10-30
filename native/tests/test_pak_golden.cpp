// test_pak_golden.cpp — M5-04: managed-packer golden vector.
//
// Verifies the MANAGED engine-pak writer through the NATIVE PakReader:
// argv = <golden.pak> <text.bin> <noise.bin>. Opens the pak (mmap),
// asserts 2 entries, GUID-sorted, text entry is LZ4 + byte-exact,
// noise entry is stored + byte-exact, both CRC-verified inside read().
// Any layout/codec/order drift between the C# writer and the C++ reader
// fails here (the native engine_asset_pak suite only covers C++ <-> C++).
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "engine/assets/asset_guid.hpp"
#include "engine/assets/asset_pak.hpp"

namespace fs = std::filesystem;

namespace {

auto Slurp(const std::string& path, std::vector<std::uint8_t>& out) -> bool {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    return false;
  }
  std::streamsize n = f.tellg();
  if (n <= 0) {
    return false;
  }
  f.seekg(0, std::ios::beg);
  out.resize(static_cast<std::size_t>(n));
  return static_cast<bool>(f.read(reinterpret_cast<char*>(out.data()), n));
}

auto Fail(const char* name) -> int {
  std::printf("[FAIL] %s\n", name);
  return 1;
}

}  // namespace

auto main(int argc, char** argv) -> int {
  using namespace engine::assets;
  if (argc != 4) {
    std::printf("usage: engine_pak_golden <golden.pak> <text.bin> <noise.bin>\n");
    return 2;
  }
  int failures = 0;

  std::vector<std::uint8_t> text;
  std::vector<std::uint8_t> noise;
  if (!Slurp(argv[2], text)) {
    return Fail("golden: read text fixture");
  }
  if (!Slurp(argv[3], noise)) {
    return Fail("golden: read noise fixture");
  }

  AssetGuid text_guid{};
  AssetGuid noise_guid{};
  if (!GuidFromString("01234567-89ab-cdef-0123-456789abcdef", text_guid)) {
    return Fail("golden: parse text guid");
  }
  if (!GuidFromString("11111111-2222-4333-8555-666666666666", noise_guid)) {
    return Fail("golden: parse noise guid");
  }

  PakReader r{};
  std::string err;
  if (!r.open(argv[1], err)) {
    std::printf("[FAIL] golden: open %s\n", err.c_str());
    return 1;
  }
  if (r.entry_count() != 2) {
    std::printf("[FAIL] golden: entry_count=%zu want 2\n", r.entry_count());
    return 1;
  }
  std::printf("[PASS] golden: cold-open 2 entries\n");

  // TOC GUID-sorted.
  auto es = r.entries();
  if (!(es[0].guid.bytes < es[1].guid.bytes)) {
    return Fail("golden: TOC GUID-sorted");
  }
  std::printf("[PASS] golden: TOC GUID-sorted\n");

  // Text entry: LZ4 codec + byte-exact.
  PakTocEntry found{};
  if (!r.find(text_guid, found)) {
    return Fail("golden: find text guid");
  }
  if (found.codec != kPakCodecLz4) {
    std::printf("[FAIL] golden: text codec=%u want lz4(1)\n", found.codec);
    return 1;
  }
  std::vector<std::uint8_t> scratch;
  std::span<const std::uint8_t> got;
  if (!r.read(text_guid, scratch, got)) {
    return Fail("golden: read text entry");
  }
  if (got.size() != text.size() ||
      std::memcmp(got.data(), text.data(), text.size()) != 0) {
    return Fail("golden: text byte-exact");
  }
  std::printf("[PASS] golden: text lz4 byte-exact (%zu -> %llu B)\n", text.size(),
              static_cast<unsigned long long>(found.packed_size));

  // Noise entry: stored codec + byte-exact.
  if (!r.find(noise_guid, found)) {
    return Fail("golden: find noise guid");
  }
  if (found.codec != kPakCodecStored) {
    std::printf("[FAIL] golden: noise codec=%u want stored(0)\n", found.codec);
    return 1;
  }
  if (!r.read(noise_guid, scratch, got)) {
    return Fail("golden: read noise entry");
  }
  if (got.size() != noise.size() ||
      std::memcmp(got.data(), noise.data(), noise.size()) != 0) {
    return Fail("golden: noise byte-exact");
  }
  std::printf("[PASS] golden: noise stored byte-exact (%zu B)\n", noise.size());

  if (!fs::exists(argv[1])) {
    (void)failures;
  }
  std::printf("PAK_GOLDEN_OK\n");
  return 0;
}

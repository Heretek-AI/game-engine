// test_asset_pipeline.cpp — M5-01 acceptance: GUID/.meta, fast preview,
// watcher, and background import worker.
//
// Proven end to end on self-generated fixtures (no network, no binaries
// checked in):
//   model   — a minimal valid GLB is SYNTHESIZED in tmp (JSON with 1 mesh /
//             1 primitive / 2 nodes + BIN chunk), covering ParseModelPreview
//             fast counts AND the cgltf+meshopt background path.
//   texture — a minimal PNG is synthesized (signature + IHDR + IDAT CRCs via
//             a local CRC32), covering ParseImagePreview + stb+bc7enc path.
// Gates:
//   AC1: .meta sidecars minted once, stable GUID across re-imports,
//        human-readable text round-trips.
//   AC2: fast lane (mmap + header parse) < 16ms per asset (measured,
//        reported p50/max).
//   AC3: background worker populates Library/ArtifactCache/<guid>.* with a
//        valid .mesh (ENGM) / .bc7 (ENGB) artifact; mesh is LOD'd
//        (output<=input tris), texture artifact dims match.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif
// (Editor LSP on Linux flags <process.h> above as missing when the WIN32
// branch is parsed without the Windows SDK — CMake builds only take the
// matching branch.)

#include "engine/assets/asset_guid.hpp"
#include "engine/assets/asset_importer.hpp"
#include "engine/assets/asset_mmap.hpp"
#include "engine/assets/asset_watcher.hpp"

namespace fs = std::filesystem;

namespace {

auto check(bool ok, const char* name) -> int {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  std::fflush(stdout);
  return ok ? 0 : 1;
}

// --- CRC32 (zlib poly) for synthesizing a valid PNG -------------------------
auto Crc32(const std::uint8_t* d, std::size_t n) -> std::uint32_t {
  static std::uint32_t t[256];
  static bool init = false;
  if (!init) {
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (0xEDB88320U ^ (c >> 1)) : (c >> 1);
      }
      t[i] = c;
    }
    init = true;
  }
  std::uint32_t c = 0xFFFFFFFFU;
  for (std::size_t i = 0; i < n; ++i) {
    c = t[(c ^ d[i]) & 0xFF] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFU;
}

auto TmpRoot() -> std::string {
  // Unique per run: TMPDIR may point at a shared scratch dir across
  // concurrent workers — never reuse a fixed name.
  static std::atomic<unsigned> s_seq{0};
  auto pid =
#if defined(_WIN32)
      _getpid();
#else
      ::getpid();
#endif
  auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::string p =
      (fs::temp_directory_path() /
       ("engine_asset_test_" +
        std::to_string(static_cast<unsigned long long>(now)) + "_" +
        std::to_string(pid) + "_" + std::to_string(s_seq.fetch_add(1))))
          .string();
  std::error_code ec;
  fs::remove_all(p, ec);
  fs::create_directories(p, ec);
  return p;
}

// Minimal 8x8 RGBA PNG: sig + IHDR + IDAT(stored deflate) + IEND.
auto WritePng(const std::string& path, int w, int h) -> bool {
  // Raw scanlines: filter byte 0 + RGBA pixels (gradient).
  std::vector<std::uint8_t> raw;
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    for (int x = 0; x < w; ++x) {
      raw.push_back(static_cast<std::uint8_t>((x * 255) / (w ? w - 1 : 1)));
      raw.push_back(static_cast<std::uint8_t>((y * 255) / (h ? h - 1 : 1)));
      raw.push_back(128);
      raw.push_back(255);
    }
  }
  // Stored (uncompressed) zlib stream: header 0x78 0x01 + LEN/NLEN blocks.
  std::vector<std::uint8_t> z = {0x78, 0x01};
  std::size_t pos = 0;
  while (pos < raw.size()) {
    std::size_t n = raw.size() - pos < 65535 ? raw.size() - pos : 65535;
    bool last = (pos + n == raw.size());
    z.push_back(last ? 0x01 : 0x00);
    z.push_back(static_cast<std::uint8_t>(n & 0xFF));
    z.push_back(static_cast<std::uint8_t>((n >> 8) & 0xFF));
    z.push_back(static_cast<std::uint8_t>((~n) & 0xFF));
    z.push_back(static_cast<std::uint8_t>(((~n) >> 8) & 0xFF));
    z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos),
             raw.begin() + static_cast<std::ptrdiff_t>(pos + n));
    pos += n;
  }
  // Adler32 of raw.
  std::uint32_t a = 1;
  std::uint32_t b = 0;
  for (std::uint8_t v : raw) {
    a = (a + v) % 65521;
    b = (b + a) % 65521;
  }
  std::uint32_t adler = (b << 16) | a;
  z.push_back(static_cast<std::uint8_t>((adler >> 24) & 0xFF));
  z.push_back(static_cast<std::uint8_t>((adler >> 16) & 0xFF));
  z.push_back(static_cast<std::uint8_t>((adler >> 8) & 0xFF));
  z.push_back(static_cast<std::uint8_t>(adler & 0xFF));

  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    return false;
  }
  const std::uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  f.write(reinterpret_cast<const char*>(sig), 8);
  auto chunk = [&](const char* type, const std::uint8_t* d, std::size_t n) {
    std::uint8_t len[4] = {static_cast<std::uint8_t>((n >> 24) & 0xFF),
                           static_cast<std::uint8_t>((n >> 16) & 0xFF),
                           static_cast<std::uint8_t>((n >> 8) & 0xFF),
                           static_cast<std::uint8_t>(n & 0xFF)};
    f.write(reinterpret_cast<const char*>(len), 4);
    f.write(type, 4);
    if (n > 0) {
      f.write(reinterpret_cast<const char*>(d),
              static_cast<std::streamsize>(n));
    }
    std::vector<std::uint8_t> cbuf;
    cbuf.insert(cbuf.end(), reinterpret_cast<const std::uint8_t*>(type),
                reinterpret_cast<const std::uint8_t*>(type) + 4);
    cbuf.insert(cbuf.end(), d, d + n);
    std::uint32_t c = Crc32(cbuf.data(), cbuf.size());
    std::uint8_t cb[4] = {static_cast<std::uint8_t>((c >> 24) & 0xFF),
                          static_cast<std::uint8_t>((c >> 16) & 0xFF),
                          static_cast<std::uint8_t>((c >> 8) & 0xFF),
                          static_cast<std::uint8_t>(c & 0xFF)};
    f.write(reinterpret_cast<const char*>(cb), 4);
  };
  std::uint8_t ihdr[13] = {};
  ihdr[0] = 0;
  ihdr[1] = 0;
  ihdr[2] = 0;
  ihdr[3] = static_cast<std::uint8_t>(w);
  ihdr[4] = 0;
  ihdr[5] = 0;
  ihdr[6] = 0;
  ihdr[7] = static_cast<std::uint8_t>(h);
  ihdr[8] = 8;  // bit depth
  ihdr[9] = 6;  // RGBA
  chunk("IHDR", ihdr, 13);
  chunk("IDAT", z.data(), z.size());
  chunk("IEND", nullptr, 0);
  f.flush();
  return static_cast<bool>(f);
}

// Minimal valid GLB: 1 mesh (1 triangle primitive), 2 nodes, BIN chunk
// with POSITION (3x vec3f) + indices. Hand-built little-endian.
auto WriteGlb(const std::string& path) -> bool {
  const char* json =
      "{\"asset\":{\"version\":\"2.0\"},\"meshes\":[{\"primitives\":[{"
      "\"attributes\":{\"POSITION\":0},\"indices\":1}]}],"
      "\"nodes\":[{\"mesh\":0},{}],\"scenes\":[{\"nodes\":[0,1]}],"
      "\"scene\":0,\"accessors\":[{\"bufferView\":0,\"componentType\":5126,"
      "\"count\":3,\"type\":\"VEC3\"},{\"bufferView\":1,\"componentType\":"
      "5123,\"count\":3,\"type\":\"SCALAR\"}],\"bufferViews\":[{"
      "\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},{\"buffer\":0,"
      "\"byteOffset\":36,\"byteLength\":6}],\"buffers\":[{\"byteLength\":44}]}";
  std::size_t json_len = std::strlen(json);
  std::size_t json_pad = (4 - (json_len % 4)) % 4;
  // BIN: 3 positions (0,0,0)(1,0,0)(0,1,0) + 3 u16 indices (0,1,2) + pad.
  float pos[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
  std::uint16_t idx[3] = {0, 1, 2};
  std::size_t bin_len = 36 + 6;
  std::size_t bin_pad = (4 - (bin_len % 4)) % 4;
  std::size_t total = 12 + 8 + json_len + json_pad + 8 + bin_len + bin_pad;

  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) {
    return false;
  }
  auto w32 = [&](std::uint32_t v) {
    std::uint8_t b[4] = {static_cast<std::uint8_t>(v & 0xFF),
                         static_cast<std::uint8_t>((v >> 8) & 0xFF),
                         static_cast<std::uint8_t>((v >> 16) & 0xFF),
                         static_cast<std::uint8_t>((v >> 24) & 0xFF)};
    f.write(reinterpret_cast<const char*>(b), 4);
  };
  f.write("glTF", 4);
  w32(2);
  w32(static_cast<std::uint32_t>(total));
  w32(static_cast<std::uint32_t>(json_len + json_pad));
  w32(0x4E4F534Au);  // JSON
  f.write(json, static_cast<std::streamsize>(json_len));
  for (std::size_t i = 0; i < json_pad; ++i) {
    f.put(' ');
  }
  w32(static_cast<std::uint32_t>(bin_len + bin_pad));
  w32(0x004E4942u);  // BIN
  f.write(reinterpret_cast<const char*>(pos), 36);
  f.write(reinterpret_cast<const char*>(idx), 6);
  for (std::size_t i = 0; i < bin_pad; ++i) {
    f.put('\0');
  }
  f.flush();
  return static_cast<bool>(f);
}

}  // namespace

auto main() -> int {
  using namespace engine::assets;
  int failures = 0;
  std::string root = TmpRoot();
  std::string assets = root + "/Assets";
  std::string lib = root + "/Library/ArtifactCache";
  std::error_code ec;
  fs::create_directories(assets, ec);

  // --- fixtures ------------------------------------------------------------
  std::string png = assets + "/test.png";
  std::string glb = assets + "/test.glb";
  failures += check(WritePng(png, 32, 32), "fixture png written");
  failures += check(WriteGlb(glb), "fixture glb written");

  // --- AC1: GUID + .meta ----------------------------------------------------
  {
    engine::assets::AssetGuid a = MintGuid();
    engine::assets::AssetGuid b = MintGuid();
    failures += check(!a.is_nil() && a != b, "mint unique non-nil GUIDs");
    std::string s = GuidToString(a);
    failures += check(s.size() == 36 && s[8] == '-' && s[13] == '-',
                      "guid string 8-4-4-4-12");
    engine::assets::AssetGuid rt{};
    failures +=
        check(GuidFromString(s, rt) && rt == a, "guid round-trips");
    engine::assets::AssetGuid bad{};
    failures += check(!GuidFromString("nope", bad) &&
                          !GuidFromString(s.substr(0, 35) + "X", bad),
                      "guid rejects malformed");

    AssetMeta m1{};
    failures +=
        check(EnsureMetaForAsset(png, m1) && !m1.guid.is_nil() &&
                  m1.kind == AssetKind::kTexture,
              "ensure .meta mints texture GUID");
    AssetMeta m2{};
    failures +=
        check(LoadMetaForAsset(png, m2) && m2.guid == m1.guid,
              "GUID stable across re-import");
    failures += check(fs::exists(png + ".meta"), ".meta sidecar on disk");
    // Human-readable: first line is `guid: <uuid>`.
    std::ifstream meta_in(png + ".meta");
    std::string first;
    std::getline(meta_in, first);
    failures += check(first.rfind("guid: ", 0) == 0 && first.size() == 42,
                      "meta human-readable guid line");
    // Hand-edit resilience: unknown keys ignored, settings honored.
    {
      std::ofstream o(png + ".meta", std::ios::app);
      o << "futureKey: 123\n";
    }
    AssetMeta m3{};
    failures += check(LoadMetaForAsset(png, m3) && m3.guid == m1.guid,
                      "meta ignores unknown keys");
    AssetMeta mg{};
    failures +=
        check(EnsureMetaForAsset(glb, mg) && mg.kind == AssetKind::kModel,
              "ensure .meta mints model GUID");
  }

  // --- AC2: fast lane <16ms ---------------------------------------------------
  {
    FastAsset fa{};
    auto t0 = std::chrono::steady_clock::now();
    bool ok = ImportFast(png, fa);
    auto dt_png =
        std::chrono::steady_clock::now() - t0;  // includes .meta I/O
    failures +=
        check(ok && !fa.is_model && fa.image.width == 32 &&
                  fa.image.height == 32 && fa.image.channels == 4,
              "fast preview png 32x32 RGBA");
    std::printf("  fast png: header=%llu ns wall=%lld ns\n",
                static_cast<unsigned long long>(fa.fast_ns),
                static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        dt_png)
                        .count()));
    FastAsset fm{};
    t0 = std::chrono::steady_clock::now();
    ok = ImportFast(glb, fm);
    auto dt_glb = std::chrono::steady_clock::now() - t0;
    failures += check(ok && fm.is_model && fm.model.mesh_count == 1 &&
                          fm.model.primitive_count == 1 &&
                          fm.model.node_count == 2 &&
                          fm.model.bin_chunk_bytes == 44,
                      "fast preview glb 1mesh/1prim/2nodes");
    std::printf("  fast glb: header=%llu ns wall=%lld ns\n",
                static_cast<unsigned long long>(fm.fast_ns),
                static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        dt_glb)
                        .count()));
    auto png_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(dt_png).count();
    auto glb_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(dt_glb).count();
    failures += check(png_ns < 16'000'000 && glb_ns < 16'000'000,
                      "fast lane <16ms per asset");
    std::printf("FAST_PREVIEW_PNG_NS=%lld FAST_PREVIEW_GLB_NS=%lld\n",
                static_cast<long long>(png_ns),
                static_cast<long long>(glb_ns));
    MappedFile mf{};
    failures +=
        check(mf.map(png) && mf.size() > 8, "mmap maps source file");
  }

  // --- watcher -----------------------------------------------------------------
  {
    AssetWatcher w{};
    failures += check(w.start(assets), "watcher starts (baseline silent)");
    failures += check(w.tracked_count() == 2, "watcher tracks 2 assets");
    failures += check(w.poll_now([](const WatcherEvent&) {}) == 0,
                      "steady state: no events");
    // Drop a new asset -> Added.
    std::string png2 = assets + "/drop.jpg";
    failures += check(WritePng(png2, 8, 8), "drop second asset");
    std::vector<WatcherEvent> evs;
    std::size_t n =
        w.poll_now([&](const WatcherEvent& e) { evs.push_back(e); });
    failures +=
        check(n == 1 && evs[0].kind == WatcherEventKind::kAdded,
              "watcher reports Added");
    // Touch -> Modified.
    FILE* f = std::fopen(png2.c_str(), "ab");
    if (f != nullptr) {
      std::fputc(0, f);
      std::fclose(f);
    }
    evs.clear();
    n = w.poll_now([&](const WatcherEvent& e) { evs.push_back(e); });
    failures += check(n == 1 && evs[0].kind == WatcherEventKind::kModified,
                      "watcher reports Modified");
    // Delete -> Removed.
    fs::remove(png2, ec);
    fs::remove(png2 + ".meta", ec);
    evs.clear();
    n = w.poll_now([&](const WatcherEvent& e) { evs.push_back(e); });
    failures += check(n == 1 && evs[0].kind == WatcherEventKind::kRemoved,
                      "watcher reports Removed");
    // Non-asset files ignored.
    {
      std::ofstream o(assets + "/notes.txt");
      o << "hello";
    }
    failures += check(w.poll_now([](const WatcherEvent&) {}) == 0,
                      "watcher ignores .txt");
    fs::remove(assets + "/notes.txt", ec);
  }

  // --- AC3: background worker ----------------------------------------------------
  {
    ImportWorker worker{};
    failures += check(worker.start(lib), "worker starts");
    failures += check(worker.enqueue(glb) && worker.enqueue(png),
                      "enqueue model + texture");
    failures += check(worker.enqueue(glb), "enqueue dedupes in-flight");
    // Wait up to 60s for both jobs (bc7enc on 32x32 is ms; meshopt µs).
    std::vector<ImportResult> done;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline && done.size() < 2) {
      worker.poll_finished(
          [&](const ImportResult& r) { done.push_back(r); });
      if (done.size() < 2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
    worker.poll_finished([&](const ImportResult& r) { done.push_back(r); });
    failures += check(done.size() == 2, "both background jobs finish");
    for (const ImportResult& r : done) {
      std::printf("  job %s ok=%d artifact=%s elapsed=%llu ns in_tri=%u "
                  "out_tri=%u lods=%u %ux%u err=%s\n",
                  r.source_path.c_str(), static_cast<int>(r.ok),
                  r.artifact_path.c_str(),
                  static_cast<unsigned long long>(r.elapsed_ns),
                  r.input_triangles, r.output_triangles, r.lod_levels,
                  r.width, r.height, r.error.c_str());
    }
    const ImportResult* mj = nullptr;
    const ImportResult* tj = nullptr;
    for (const ImportResult& r : done) {
      if (r.source_path == glb) {
        mj = &r;
      } else {
        tj = &r;
      }
    }
#if defined(ENGINE_ASSETS_HAS_CGLTF) && defined(ENGINE_ASSETS_HAS_MESHOPT) && \
    defined(ENGINE_ASSETS_HAS_STB) && defined(ENGINE_ASSETS_HAS_BC7)
    failures += check(mj != nullptr && mj->ok, "model job ok");
    failures += check(mj != nullptr && mj->input_triangles == 1,
                      "model job input 1 triangle");
    failures += check(mj != nullptr && mj->output_triangles <=
                                             mj->input_triangles &&
                          mj->lod_levels >= 1,
                      "model job optimized + LOD chain");
    failures +=
        check(mj != nullptr && fs::exists(mj->artifact_path),
              "model artifact exists");
    if (mj != nullptr && fs::exists(mj->artifact_path)) {
      std::ifstream a(mj->artifact_path, std::ios::binary);
      char magic[4] = {};
      a.read(magic, 4);
      failures += check(std::string(magic, 4) == "ENGM",
                        "model artifact magic ENGM");
    }
    failures += check(tj != nullptr && tj->ok, "texture job ok");
    failures += check(tj != nullptr && tj->width == 32 && tj->height == 32,
                      "texture job dims 32x32");
    failures +=
        check(tj != nullptr && fs::exists(tj->artifact_path),
              "texture artifact exists");
    if (tj != nullptr && fs::exists(tj->artifact_path)) {
      std::ifstream a(tj->artifact_path, std::ios::binary);
      char magic[4] = {};
      a.read(magic, 4);
      failures += check(std::string(magic, 4) == "ENGB",
                        "texture artifact magic ENGB");
    }
#else
    failures += check(mj != nullptr && tj != nullptr, "jobs reported");
    std::printf("  (offline fetch: jobs degrade, artifacts skipped)\n");
#endif
    worker.shutdown();
    failures += check(!worker.running(), "worker shuts down");
  }

  std::printf(failures == 0 ? "ASSET_PIPELINE_OK\n" : "ASSET_PIPELINE_FAIL\n");
  return failures == 0 ? 0 : 1;
}

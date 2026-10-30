// asset_importer.cpp — fast lane + background worker; see header.
//
// Building the REAL thing (not stubs):
//   model:    cgltf parse -> meshoptimizer vertex-cache reorder + overdraw
//             + simplify LOD chain -> .mesh artifact (custom binary)
//   texture:  stb_image decode (bundled via astc-encoder's ThirdParty copy
//             would drag 35MB — instead we use the tiny stb mirror from
//             FetchContent) -> mip chain -> bc7enc compress -> .bc7 artifact
//
// Third-party sources arrive via FetchContent into the build tree (never
// vendored — keeps the review surface engine-only). Compile flags: the
// adapter TU uses base flags; the third-party TUs get -w (no warnings
// from upstream headers, -Werror stays on for our code only).
#include "engine/assets/asset_importer.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <utility>

#include "engine/assets/asset_mmap.hpp"

#if defined(ENGINE_ASSETS_HAS_CGLTF)
// cgltf is a single-header library: define the implementation in ONE TU.
// Compiled as C++ here, so tag the TU -w-equivalent: cgltf's own code trips
// -Wsign-conversion under our -Werror. Push/pop around the single include.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#endif
#if defined(ENGINE_ASSETS_HAS_MESHOPT)
#include "meshoptimizer.h"
#endif
#if defined(ENGINE_ASSETS_HAS_STB)
#include "stb_image.h"
#endif
#if defined(ENGINE_ASSETS_HAS_BC7)
#include "bc7enc.h"
#endif

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace engine::assets {
namespace {

auto NowNs() noexcept -> std::uint64_t {
  return static_cast<std::uint64_t>(
      std::chrono::steady_clock::now().time_since_epoch().count());
}

auto MkdirP(const std::string& path) noexcept -> bool {
#if defined(_WIN32)
  std::string cur;
  for (std::size_t i = 0; i < path.size(); ++i) {
    char c = path[i];
    cur.push_back(c);
    if (c == '/' || c == '\\' || i + 1 == path.size()) {
      CreateDirectoryA(cur.c_str(), nullptr);
    }
  }
  DWORD attr = GetFileAttributesA(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES &&
         (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
  std::string cur;
  if (!path.empty() && path[0] == '/') {
    cur = "/";
  }
  std::size_t i = cur.empty() ? 0 : 1;
  for (; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/') {
      if (!cur.empty() && cur != "/") {
        // NOLINTNEXTLINE: host tooling path, one mkdir per component.
        ::mkdir(cur.c_str(), 0755);
      }
      if (i < path.size()) {
        cur.push_back('/');
      }
    } else {
      cur.push_back(path[i]);
    }
  }
  struct stat st {};
  // NOLINTNEXTLINE: host tooling path.
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

// (WriteMeshArtifact / WriteBc7Artifact write atomically themselves.)

// --- .mesh artifact layout (little-endian, versioned) ---------------------
// magic "ENGM" u32 version(=1) u32 lod_count
// per LOD: u32 index_count u32 vertex_count [indices u32*] [positions f32*3]
// The preview path never reads this; the renderer streams LOD0 first.
auto WriteMeshArtifact(const std::string& path,
                       const std::vector<std::uint32_t>& lods_triangles,
                       const std::vector<std::vector<std::uint32_t>>& lods_idx,
                       const float* positions, std::size_t vertex_count)
    -> bool {
  try {
    std::string tmp = path + ".tmp";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      return false;
    }
    auto w32 = [&](std::uint32_t v) {
      out.write(reinterpret_cast<const char*>(&v), 4);
    };
    out.write("ENGM", 4);
    w32(1);
    w32(static_cast<std::uint32_t>(lods_idx.size()));
    for (std::size_t l = 0; l < lods_idx.size(); ++l) {
      w32(static_cast<std::uint32_t>(lods_idx[l].size()));
      w32(static_cast<std::uint32_t>(vertex_count));
      out.write(reinterpret_cast<const char*>(lods_idx[l].data()),
                static_cast<std::streamsize>(lods_idx[l].size() * 4));
    }
    out.write(reinterpret_cast<const char*>(positions),
              static_cast<std::streamsize>(vertex_count * 3 * 4));
    (void)lods_triangles;
    out.flush();
    if (!out) {
      return false;
    }
    out.close();
#if defined(_WIN32)
    std::remove(path.c_str());
#endif
    return std::rename(tmp.c_str(), path.c_str()) == 0;
  } catch (...) {
    return false;
  }
}

// --- .bc7 artifact layout ---------------------------------------------------
// magic "ENGB" u32 version(=1) u32 width u32 height u32 mip_count
// per mip: u32 byte_len [bc7 blocks]
auto WriteBc7Artifact(const std::string& path, std::uint32_t width,
                      std::uint32_t height,
                      const std::vector<std::vector<std::uint8_t>>& mips)
    -> bool {
  try {
    std::string tmp = path + ".tmp";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      return false;
    }
    auto w32 = [&](std::uint32_t v) {
      out.write(reinterpret_cast<const char*>(&v), 4);
    };
    out.write("ENGB", 4);
    w32(1);
    w32(width);
    w32(height);
    w32(static_cast<std::uint32_t>(mips.size()));
    for (const auto& m : mips) {
      w32(static_cast<std::uint32_t>(m.size()));
      out.write(reinterpret_cast<const char*>(m.data()),
                static_cast<std::streamsize>(m.size()));
    }
    out.flush();
    if (!out) {
      return false;
    }
    out.close();
#if defined(_WIN32)
    std::remove(path.c_str());
#endif
    return std::rename(tmp.c_str(), path.c_str()) == 0;
  } catch (...) {
    return false;
  }
}

}  // namespace

auto ImportFast(const std::string& asset_path, FastAsset& out) noexcept
    -> bool {
  std::uint64_t t0 = NowNs();
  try {
    AssetMeta meta{};
    if (!EnsureMetaForAsset(asset_path, meta)) {
      return false;
    }
    MappedFile mf{};
    if (!mf.map(asset_path)) {
      return false;
    }
    FastAsset fa{};
    fa.meta = meta;
    AssetKind kind = meta.kind;
    if (kind == AssetKind::kUnknown) {
      kind = KindFromExtension(asset_path);
      fa.meta.kind = kind;
    }
    bool ok = false;
    if (kind == AssetKind::kModel) {
      fa.is_model = true;
      ok = ParseModelPreview(mf.data(), mf.size(), fa.model);
    } else if (kind == AssetKind::kTexture) {
      fa.is_model = false;
      ok = ParseImagePreview(mf.data(), mf.size(), fa.image);
    } else {
      return false;
    }
    if (!ok) {
      return false;
    }
    fa.fast_ns = NowNs() - t0;
    out = std::move(fa);
    return true;
  } catch (...) {
    return false;
  }
}

auto ArtifactPathFor(const std::string& library_root, const AssetGuid& guid,
                     std::string_view ext) -> std::string {
  std::string hex = GuidToString(guid);
  std::string path = library_root;
  if (!path.empty() && path.back() != '/' && path.back() != '\\') {
    path.push_back('/');
  }
  path += hex.substr(0, 2);
  path.push_back('/');
  path += hex;
  path.push_back('.');
  path += std::string(ext);
  return path;
}

auto ImportWorker::start(const std::string& library_root) -> bool {
  bool expected = false;
  if (!running_.compare_exchange_strong(expected, true)) {
    return false;
  }
  library_root_ = library_root;
  stop_.store(false, std::memory_order_release);
  try {
    thread_ = std::thread([this] { ThreadMain(); });
  } catch (...) {
    running_.store(false, std::memory_order_release);
    return false;
  }
  return true;
}

auto ImportWorker::shutdown() -> void {
  bool was = running_.exchange(false);
  if (!was) {
    return;
  }
  stop_.store(true, std::memory_order_release);
  if (thread_.joinable()) {
    thread_.join();
  }
  std::lock_guard<std::mutex> lk(mutex_);
  while (!pending_.empty()) {
    pending_.pop();
  }
  in_flight_.clear();
}

auto ImportWorker::enqueue(const std::string& asset_path) -> bool {
  if (!running_.load(std::memory_order_acquire)) {
    return false;
  }
  std::lock_guard<std::mutex> lk(mutex_);
  for (const std::string& p : in_flight_) {
    if (p == asset_path) {
      return true;  // already queued/in-flight: dedupe
    }
  }
  pending_.push(asset_path);
  in_flight_.push_back(asset_path);
  return true;
}

auto ImportWorker::poll_finished(
    const std::function<void(const ImportResult&)>& cb) -> std::size_t {
  std::vector<ImportResult> local;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    local.swap(finished_);
  }
  for (const ImportResult& r : local) {
    cb(r);
  }
  return local.size();
}

auto ImportWorker::pending_count() const noexcept -> std::size_t {
  std::lock_guard<std::mutex> lk(mutex_);
  return pending_.size();
}

auto ImportWorker::ThreadMain() -> void {
  for (;;) {
    std::string next;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      if (!pending_.empty()) {
        next = pending_.front();
        pending_.pop();
      }
    }
    if (next.empty()) {
      if (stop_.load(std::memory_order_acquire)) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    ImportResult r{};
    ProcessOne(next, r);
    {
      std::lock_guard<std::mutex> lk(mutex_);
      finished_.push_back(std::move(r));
      for (auto it = in_flight_.begin(); it != in_flight_.end(); ++it) {
        if (*it == next) {
          in_flight_.erase(it);
          break;
        }
      }
    }
    if (stop_.load(std::memory_order_acquire)) {
      std::lock_guard<std::mutex> lk(mutex_);
      if (pending_.empty()) {
        return;
      }
    }
  }
}

auto ImportWorker::ProcessOne(const std::string& asset_path,
                              ImportResult& out) -> void {
  std::uint64_t t0 = NowNs();
  ImportResult r{};
  r.source_path = asset_path;
  AssetMeta meta{};
  if (!EnsureMetaForAsset(asset_path, meta)) {
    r.error = "no .meta (unwritable?)";
    r.elapsed_ns = NowNs() - t0;
    out = std::move(r);
    return;
  }
  r.guid = meta.guid;
  AssetKind kind = meta.kind != AssetKind::kUnknown
                       ? meta.kind
                       : KindFromExtension(asset_path);
  if (kind == AssetKind::kModel) {
#if defined(ENGINE_ASSETS_HAS_CGLTF) && defined(ENGINE_ASSETS_HAS_MESHOPT)
    MappedFile mf{};
    if (!mf.map(asset_path)) {
      r.error = "map failed";
      r.elapsed_ns = NowNs() - t0;
      out = std::move(r);
      return;
    }
    cgltf_options opts{};
    std::memset(&opts, 0, sizeof(opts));
    cgltf_data* data = nullptr;
    cgltf_result res;
    bool is_glb = mf.size() >= 4 && mf.data()[0] == 'g';
    if (is_glb) {
      res = cgltf_parse(&opts, mf.data(), mf.size(), &data);
    } else {
      // .gltf JSON + external buffers: parse JSON from the map, then load
      // buffers relative to the source dir (cgltf_load_buffers needs a
      // NUL-terminated JSON string — copy the mapped range once; this is
      // the background lane, copies are fine here).
      std::string json(reinterpret_cast<const char*>(mf.data()), mf.size());
      res = cgltf_parse(&opts, json.c_str(), json.size(), &data);
    }
    if (res != cgltf_result_success || data == nullptr) {
      r.error = "cgltf parse failed";
      r.elapsed_ns = NowNs() - t0;
      out = std::move(r);
      return;
    }
    if (!is_glb) {
      // Resolve external .bin relative to the asset path.
      if (cgltf_load_buffers(&opts, data, asset_path.c_str()) !=
          cgltf_result_success) {
        cgltf_free(data);
        r.error = "cgltf load_buffers failed";
        r.elapsed_ns = NowNs() - t0;
        out = std::move(r);
        return;
      }
    }
    // Decode ALL buffer views the meshes reference (interleaved POSITION).
    std::uint64_t total_tris = 0;
    std::vector<float> positions;  // packed xyz across all primitives
    std::vector<std::uint32_t> indices;
    std::uint32_t base = 0;
    for (std::size_t mi = 0; mi < data->meshes_count; ++mi) {
      const cgltf_mesh& mesh = data->meshes[mi];
      for (std::size_t pi = 0; pi < mesh.primitives_count; ++pi) {
        const cgltf_primitive& prim = mesh.primitives[pi];
        if (prim.type != cgltf_primitive_type_triangles) {
          continue;
        }
        const cgltf_accessor* pos = nullptr;
        for (std::size_t ai = 0; ai < prim.attributes_count; ++ai) {
          if (std::strcmp(prim.attributes[ai].name, "POSITION") == 0) {
            pos = prim.attributes[ai].data;
            break;
          }
        }
        if (pos == nullptr || pos->type != cgltf_type_vec3 ||
            pos->component_type != cgltf_component_type_r_32f) {
          continue;
        }
        std::size_t vcount = pos->count;
        std::size_t vstart = positions.size() / 3;
        positions.resize((vstart + vcount) * 3);
        for (std::size_t vi = 0; vi < vcount; ++vi) {
          float xyz[3] = {0, 0, 0};
          if (!cgltf_accessor_read_float(pos, vi, xyz, 3)) {
            break;
          }
          positions[(vstart + vi) * 3 + 0] = xyz[0];
          positions[(vstart + vi) * 3 + 1] = xyz[1];
          positions[(vstart + vi) * 3 + 2] = xyz[2];
        }
        if (prim.indices != nullptr) {
          for (std::size_t ii = 0; ii < prim.indices->count; ++ii) {
            std::uint32_t idx =
                static_cast<std::uint32_t>(cgltf_accessor_read_index(
                    prim.indices, static_cast<cgltf_size>(ii)));
            indices.push_back(base + idx);
          }
        } else {
          for (std::size_t vi = 0; vi < vcount; ++vi) {
            indices.push_back(base + static_cast<std::uint32_t>(vi));
          }
        }
        base += static_cast<std::uint32_t>(vcount);
        total_tris += static_cast<std::uint64_t>(indices.size() / 3);
      }
    }
    r.input_triangles = static_cast<std::uint32_t>(total_tris);
    bool mesh_ok = false;
    if (!positions.empty() && !indices.empty()) {
      std::vector<std::uint32_t> remap(indices.size());
      std::size_t vcount = positions.size() / 3;
      std::size_t opt_vcount = meshopt_generateVertexRemap(
          remap.data(), indices.data(), indices.size(), positions.data(),
          vcount, sizeof(float) * 3);
      std::vector<std::uint32_t> opt_idx(indices.size());
      std::vector<float> opt_pos(opt_vcount * 3);
      meshopt_remapIndexBuffer(opt_idx.data(), indices.data(), indices.size(),
                               remap.data());
      meshopt_remapVertexBuffer(opt_pos.data(), positions.data(), vcount,
                                sizeof(float) * 3, remap.data());
      meshopt_optimizeVertexCache(opt_idx.data(), opt_idx.data(),
                                  opt_idx.size(), opt_vcount);
      meshopt_optimizeOverdraw(opt_idx.data(), opt_idx.data(), opt_idx.size(),
                               opt_pos.data(), opt_vcount, sizeof(float) * 3,
                               1.05F);
      // LOD chain: LOD0 = optimized full, then simplify to lod_ratio.
      std::vector<std::vector<std::uint32_t>> lods;
      lods.push_back(opt_idx);
      float target = meta.settings.lod_ratio;
      if (!(target > 0.0F && target < 1.0F)) {
        target = 0.5F;
      }
      std::size_t target_count =
          static_cast<std::size_t>(static_cast<float>(opt_idx.size()) *
                                   target);
      target_count -= target_count % 3;
      if (target_count >= 12 && target_count < opt_idx.size()) {
        std::vector<std::uint32_t> lod(target_count ? target_count
                                                   : opt_idx.size());
        lod.resize(meshopt_simplify(lod.data(), opt_idx.data(), opt_idx.size(),
                                    opt_pos.data(), opt_vcount,
                                    sizeof(float) * 3, target_count, 1e-2F));
        if (lod.size() >= 12) {
          lods.push_back(std::move(lod));
        }
      }
      std::string art =
          ArtifactPathFor(library_root_, meta.guid, "mesh");
      std::string dir = art.substr(0, art.find_last_of('/'));
      std::vector<std::uint32_t> tri_counts;
      for (const auto& l : lods) {
        tri_counts.push_back(static_cast<std::uint32_t>(l.size() / 3));
      }
      if (MkdirP(dir) &&
          WriteMeshArtifact(art, tri_counts, lods, opt_pos.data(),
                             opt_vcount)) {
        r.ok = true;
        r.artifact_path = art;
        r.output_triangles = tri_counts.front();
        r.lod_levels = static_cast<std::uint32_t>(lods.size());
        mesh_ok = true;
      } else {
        r.error = "mesh artifact write failed";
      }
    } else {
      r.error = "no decodable triangles";
    }
    (void)mesh_ok;
    cgltf_free(data);
    r.elapsed_ns = NowNs() - t0;
    out = std::move(r);
    return;
#else
    r.error = "model pipeline unavailable (fetch failed)";
    r.elapsed_ns = NowNs() - t0;
    out = std::move(r);
    return;
#endif
  }
  if (kind == AssetKind::kTexture) {
#if defined(ENGINE_ASSETS_HAS_STB) && defined(ENGINE_ASSETS_HAS_BC7)
    MappedFile mf{};
    if (!mf.map(asset_path)) {
      r.error = "map failed";
      r.elapsed_ns = NowNs() - t0;
      out = std::move(r);
      return;
    }
    int w = 0;
    int h = 0;
    int comp = 0;
    stbi_uc* px = stbi_load_from_memory(mf.data(), static_cast<int>(mf.size()),
                                        &w, &h, &comp, 4);
    if (px == nullptr || w <= 0 || h <= 0) {
      r.error = "stb decode failed";
      r.elapsed_ns = NowNs() - t0;
      out = std::move(r);
      return;
    }
    r.width = static_cast<std::uint32_t>(w);
    r.height = static_cast<std::uint32_t>(h);
    // Mip chain on RGBA8 (box filter), capped so the tail never exceeds
    // 8 levels for test-size textures (real chains for real sizes).
    struct Mip {
      std::vector<std::uint8_t> px;
      int w;
      int h;
    };
    std::vector<Mip> mips;
    mips.push_back({std::vector<std::uint8_t>(px, px + w * h * 4), w, h});
    stbi_image_free(px);
    while (mips.back().w > 4 || mips.back().h > 4) {
      const Mip& s = mips.back();
      int dw = s.w > 1 ? s.w / 2 : 1;
      int dh = s.h > 1 ? s.h / 2 : 1;
      if (dw == s.w && dh == s.h) {
        break;
      }
      Mip d{std::vector<std::uint8_t>(static_cast<std::size_t>(dw * dh * 4)),
            dw, dh};
      for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
          for (int c = 0; c < 4; ++c) {
            int sx = x * 2;
            int sy = y * 2;
            unsigned sum = 0;
            unsigned n = 0;
            for (int ky = 0; ky < 2 && sy + ky < s.h; ++ky) {
              for (int kx = 0; kx < 2 && sx + kx < s.w; ++kx) {
                sum += s.px[static_cast<std::size_t>((sy + ky) * s.w + sx +
                                                     kx) *
                                                   4 +
                                           static_cast<std::size_t>(c)];
                ++n;
              }
            }
            d.px[static_cast<std::size_t>(y * dw + x) * 4 +
                 static_cast<std::size_t>(c)] =
                static_cast<std::uint8_t>(sum / (n ? n : 1));
          }
        }
      }
      mips.push_back(std::move(d));
      if (mips.size() >= 12) {
        break;
      }
    }
    // bc7enc: one-time init, fast path per block.
    static std::once_flag s_bc7_once;
    std::call_once(s_bc7_once, [] { bc7enc_compress_block_init(); });
    std::vector<std::vector<std::uint8_t>> blocks;
    blocks.reserve(mips.size());
    bc7enc_compress_block_params params{};
    bc7enc_compress_block_params_init(&params);
    params.m_max_partitions_mode = BC7ENC_MAX_PARTITIONS1;
    for (const Mip& m : mips) {
      int bw = (m.w + 3) / 4;
      int bh = (m.h + 3) / 4;
      std::vector<std::uint8_t> dst(static_cast<std::size_t>(bw * bh * 16));
      for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
          std::uint8_t tile[64] = {};
          for (int yy = 0; yy < 4; ++yy) {
            for (int xx = 0; xx < 4; ++xx) {
              int sx = bx * 4 + xx;
              int sy = by * 4 + yy;
              if (sx >= m.w) {
                sx = m.w - 1;
              }
              if (sy >= m.h) {
                sy = m.h - 1;
              }
              std::memcpy(&tile[(yy * 4 + xx) * 4],
                          &m.px[static_cast<std::size_t>(sy * m.w + sx) * 4],
                          4);
            }
          }
          void* dst_block =
              &dst[static_cast<std::size_t>(by * bw + bx) * 16];
          bc7enc_compress_block(dst_block, tile, &params);
        }
      }
      blocks.push_back(std::move(dst));
    }
    std::string art = ArtifactPathFor(library_root_, meta.guid, "bc7");
    std::string dir = art.substr(0, art.find_last_of('/'));
    if (MkdirP(dir) &&
        WriteBc7Artifact(art, r.width, r.height, blocks)) {
      r.ok = true;
      r.artifact_path = art;
    } else {
      r.error = "bc7 artifact write failed";
    }
    r.elapsed_ns = NowNs() - t0;
    out = std::move(r);
    return;
#else
    r.error = "texture pipeline unavailable (fetch failed)";
    r.elapsed_ns = NowNs() - t0;
    out = std::move(r);
    return;
#endif
  }
  r.error = "unknown asset kind";
  r.elapsed_ns = NowNs() - t0;
  out = std::move(r);
}

}  // namespace engine::assets

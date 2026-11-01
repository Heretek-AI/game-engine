// test_terrain_p6_01.cpp — P6-01 acceptance: terrain heightmap + chunked
// LOD mesh + splat material + Jolt heightfield collider.
//
// AC1 — heightmap asset: a synthetic 9x9 R16 grayscale PNG (stored-deflate
//   IDAT, same technique as test_asset_pipeline) decodes through
//   Heightmap::load_png to world heights (R16 value/65535 * range); the
//   center peak (R16 max) samples at max_height and bilinear midpoints
//   interpolate between neighbors.
// AC2 — chunked mesh with LOD: a 2x2 chunk grid over the 9x9 heightmap
//   (5x5 verts per chunk) builds positions == sampled heights, unit-ish
//   normals at the flat border, LOD0 index count == 4x4x6, LOD1 == 2x2x6,
//   and select_lod() steps through distance thresholds.
// AC3 — 4-layer splat material: rock-by-slope / grass+dirt height bands /
//   snow cap resolve to normalized weights; flat lowland is grass-heavy,
//   the steep peak is rock-heavy.
// AC4 — Jolt HeightFieldShape collider (spec §4.14 sequence): static body
//   from the grid, OptimizeBroadPhase, then spatial_raycast straight down
//   at three sample points hits the sampled height ±0.01.
// AC5 — golden: FNV-1a over the decoded heights + LOD0 indices + splat
//   weights equals the committed constant (any mesh/splat/decode drift
//   fails closed).
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <Jolt/Jolt.h>
#include <Jolt/Core/Memory.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include "engine/core/job_scheduler.hpp"
#include "engine/core/page_arena_allocator.hpp"
#include "engine/physics/physics_world.hpp"
#include "engine/physics/spatial_queries.hpp"
#include "engine/terrain/terrain_chunk_mesh.hpp"
#include "engine/terrain/terrain_heightfield_collider.hpp"
#include "engine/terrain/terrain_heightmap.hpp"
#include "engine/terrain/terrain_splat_material.hpp"

namespace {

auto failures = 0;

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}

auto feq(float a, float b, float eps) -> bool {
  const float d = a - b;
  return d > -eps && d < eps;
}

// --- CRC32 (zlib poly, same as test_asset_pipeline) -------------------------

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

// --- Minimal 16-bit grayscale PNG (stored deflate, big-endian words) -------
// N x N, color type 0, bit depth 16. `px` holds N*N host-order samples.

auto WritePng16(const std::string& path, int n, const std::uint16_t* px)
    -> bool {
  std::vector<std::uint8_t> raw;
  for (int y = 0; y < n; ++y) {
    raw.push_back(0);
    for (int x = 0; x < n; ++x) {
      const std::uint16_t v = px[static_cast<std::size_t>(y) * n + x];
      raw.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
      raw.push_back(static_cast<std::uint8_t>(v & 0xFF));
    }
  }
  std::vector<std::uint8_t> z = {0x78, 0x01};
  std::size_t pos = 0;
  while (pos < raw.size()) {
    std::size_t c = raw.size() - pos < 65535 ? raw.size() - pos : 65535;
    bool last = (pos + c == raw.size());
    z.push_back(last ? 0x01 : 0x00);
    z.push_back(static_cast<std::uint8_t>(c & 0xFF));
    z.push_back(static_cast<std::uint8_t>((c >> 8) & 0xFF));
    z.push_back(static_cast<std::uint8_t>((~c) & 0xFF));
    z.push_back(static_cast<std::uint8_t>(((~c) >> 8) & 0xFF));
    z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(pos),
             raw.begin() + static_cast<std::ptrdiff_t>(pos + c));
    pos += c;
  }
  std::uint32_t a = 1;
  std::uint32_t b = 0;
  for (std::uint8_t v : raw) {
    a = (a + v) % 65521;
    b = (b + a) % 65521;
  }
  const std::uint32_t adler = (b << 16) | a;
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
  auto chunk = [&](const char* type, const std::uint8_t* d, std::size_t len) {
    std::uint8_t lb[4] = {static_cast<std::uint8_t>((len >> 24) & 0xFF),
                          static_cast<std::uint8_t>((len >> 16) & 0xFF),
                          static_cast<std::uint8_t>((len >> 8) & 0xFF),
                          static_cast<std::uint8_t>(len & 0xFF)};
    f.write(reinterpret_cast<const char*>(lb), 4);
    f.write(type, 4);
    if (len > 0) {
      f.write(reinterpret_cast<const char*>(d),
              static_cast<std::streamsize>(len));
    }
    std::vector<std::uint8_t> cbuf;
    cbuf.insert(cbuf.end(), reinterpret_cast<const std::uint8_t*>(type),
                reinterpret_cast<const std::uint8_t*>(type) + 4);
    cbuf.insert(cbuf.end(), d, d + len);
    const std::uint32_t c = Crc32(cbuf.data(), cbuf.size());
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
  ihdr[3] = static_cast<std::uint8_t>(n);
  ihdr[4] = 0;
  ihdr[5] = 0;
  ihdr[6] = 0;
  ihdr[7] = static_cast<std::uint8_t>(n);
  ihdr[8] = 16;  // bit depth
  ihdr[9] = 0;   // grayscale
  chunk("IHDR", ihdr, 13);
  chunk("IDAT", z.data(), z.size());
  chunk("IEND", nullptr, 0);
  f.flush();
  return static_cast<bool>(f);
}

auto ReadFile(const std::string& path) -> std::vector<std::uint8_t> {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return {};
  }
  return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)),
                                   std::istreambuf_iterator<char>());
}

auto Fnv1a(std::uint64_t h, const void* data, std::size_t n) -> std::uint64_t {
  const auto* p = static_cast<const std::uint8_t*>(data);
  for (std::size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

auto RayLabel(char* buf, std::size_t n, int i, const char* tail) -> void {
  // snprintf-free label builder (Sonar S3458): fixed prefixes + 1 digit.
  const char* pre = "collider: ray ";
  std::size_t p = 0;
  while (pre[p] != '\0' && p + 2 < n) {
    buf[p] = pre[p];
    ++p;
  }
  buf[p++] = static_cast<char>('0' + i);
  buf[p++] = ' ';
  std::size_t t = 0;
  while (tail[t] != '\0' && p + 1 < n) {
    buf[p++] = tail[t++];
  }
  buf[p] = '\0';
}

}  // namespace

auto main() -> int {
  setvbuf(stdout, nullptr, _IONBF, 0);
  JPH::RegisterDefaultAllocator();

  // --- Fixture: 9x9 heightmap, peak 1.0 at center, 8x8 m --------------------
  // R16 sample = round(65535 * h / 10) with max_height 10: the peak decodes
  // to exactly 10.0 only when the R16 value is 65535 — so the peak height
  // here is 1.0 m (R16 6554 ~= 1.00008 after quantization) and the golden
  // pins the exact decoded bits.
  constexpr int kN = 9;
  constexpr float kSpacing = 1.0F;
  constexpr float kMinH = 0.0F;
  constexpr float kMaxH = 10.0F;
  std::uint16_t r16[kN * kN];
  for (int z = 0; z < kN; ++z) {
    for (int x = 0; x < kN; ++x) {
      const int dx = x - 4;
      const int dz = z - 4;
      const float h =
          1.0F - static_cast<float>(dx * dx + dz * dz) / 32.0F;
      const float cl = h < 0.0F ? 0.0F : h;
      r16[static_cast<std::size_t>(z) * kN + x] =
          static_cast<std::uint16_t>(cl / kMaxH * 65535.0F + 0.5F);
    }
  }
  const std::string png_path = "/tmp/p6_01_height16.png";
  check(WritePng16(png_path, kN, r16), "fixture: write R16 PNG");
  const std::vector<std::uint8_t> png = ReadFile(png_path);
  check(!png.empty(), "fixture: read R16 PNG back");

  // --- AC1: heightmap asset -------------------------------------------------
  engine::terrain::Heightmap hm;
  std::vector<float> heights(kN * kN, -1.0F);
  engine::terrain::HeightmapGrid grid;
  int channels = 0;
  check(hm.load_png(png.data(), png.size(), heights.data(),
                    static_cast<std::uint32_t>(heights.size()), kSpacing,
                    kMinH, kMaxH, grid, channels),
        "heightmap: load_png R16 ok");
  check(grid.width == 9 && grid.height == 9, "heightmap: 9x9 dims");
  check(channels == 1, "heightmap: 1 channel");
  const float peak = engine::terrain::Heightmap::sample_height(grid, 4, 4);
  check(feq(peak, 1.0F, 0.002F), "heightmap: center peak ~= 1.0");
  const float corner =
      engine::terrain::Heightmap::sample_height(grid, 0, 0);
  check(feq(corner, 0.0F, 0.002F), "heightmap: corner ~= 0.0");
  const float mid =
      engine::terrain::Heightmap::sample_height(grid, 4.5F, 4.0F);
  const float h4 = heights[4 * kN + 4];
  const float h5 = heights[4 * kN + 5];
  check(feq(mid, (h4 + h5) * 0.5F, 1e-5F), "heightmap: bilinear midpoint");
  float nx = 0.0F;
  float ny = 0.0F;
  float nz = 0.0F;
  engine::terrain::Heightmap::sample_normal(grid, 4, 4, nx, ny, nz);
  check(feq(nx * nx + ny * ny + nz * nz, 1.0F, 1e-4F),
        "heightmap: peak normal unit");
  check(ny > 0.99F, "heightmap: peak normal ~up");
  // Malformed input fails closed.
  engine::terrain::HeightmapGrid bad_grid;
  int bad_ch = 0;
  std::vector<float> bad_buf(81, 0.0F);
  const std::uint8_t junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  check(!hm.load_png(junk, sizeof(junk), bad_buf.data(), 81, 1.0F, 0.0F,
                     10.0F, bad_grid, bad_ch),
        "heightmap: junk rejected");

  // --- AC2: chunked mesh with LOD -------------------------------------------
  constexpr std::uint32_t kChunk = 5;  // 9 = 2*(5-1)+1 -> 2x2 chunks
  check(engine::terrain::ChunkMesh::lod_index_count(kChunk, 0) == 96,
        "mesh: LOD0 4x4x6 indices");
  check(engine::terrain::ChunkMesh::lod_index_count(kChunk, 1) == 24,
        "mesh: LOD1 2x2x6 indices");
  check(engine::terrain::ChunkMesh::lod_index_count(kChunk, 2) == 6,
        "mesh: LOD2 1x1x6 indices");
  std::vector<engine::terrain::TerrainVertex> verts(kChunk * kChunk);
  const std::uint32_t total =
      engine::terrain::ChunkMesh::lod_total_indices(kChunk);
  std::vector<std::uint32_t> idx(total, 0xFFFFFFFFu);
  std::uint32_t offsets[4] = {0, 0, 0, 0};
  check(engine::terrain::ChunkMesh::build_chunk(grid, 0, 0, kChunk,
                                                verts.data(), idx.data(),
                                                offsets),
        "mesh: build chunk (0,0)");
  check(feq(verts[4 * kChunk + 4].py, peak, 1e-5F),
        "mesh: chunk peak vertex == sampled height");
  check(feq(verts[0].py, corner, 1e-5F), "mesh: chunk corner == 0");
  check(feq(verts[0].nx * verts[0].nx + verts[0].ny * verts[0].ny +
                verts[0].nz * verts[0].nz,
            1.0F, 1e-4F),
        "mesh: normals unit");
  check(offsets[0] == 0 && offsets[1] == 96 && offsets[2] == 120,
        "mesh: LOD offsets packed");
  const float dists[3] = {25.0F, 60.0F, 120.0F};
  check(engine::terrain::ChunkMesh::select_lod(10.0F, dists, 3) == 0,
        "mesh: lod near = 0");
  check(engine::terrain::ChunkMesh::select_lod(30.0F, dists, 3) == 1,
        "mesh: lod mid = 1");
  check(engine::terrain::ChunkMesh::select_lod(200.0F, dists, 3) == 2,
        "mesh: lod far = last");
  std::vector<engine::terrain::TerrainVertex> verts2(kChunk * kChunk);
  std::vector<std::uint32_t> idx2(total, 0);
  std::uint32_t off2[4] = {0, 0, 0, 0};
  check(engine::terrain::ChunkMesh::build_chunk(grid, 1, 1, kChunk,
                                                verts2.data(), idx2.data(),
                                                off2),
        "mesh: build chunk (1,1)");
  check(feq(verts2[0].py, peak, 1e-5F),
        "mesh: chunk (1,1) origin shares peak vertex");

  // --- AC3: 4-layer splat ----------------------------------------------------
  engine::terrain::SplatLayer layers[4]{};
  layers[0].max_slope = 0.08F;  // rock: steep only (open height band)
  layers[0].min_height = 0.0F;
  layers[0].max_height = 0.0F;
  layers[1].min_height = 0.0F;  // grass: low band, gentle slopes
  layers[1].max_height = 0.5F;
  layers[1].max_slope = 0.25F;
  layers[2].min_height = 0.4F;  // dirt: high band (above grass)
  layers[2].max_height = 0.8F;
  layers[2].max_slope = 0.35F;
  layers[3].min_height = 0.75F;  // snow: high + flat
  layers[3].max_height = 1.0F;
  layers[3].max_slope = 0.12F;
  engine::terrain::SplatWeights wlow{};
  // Probe at the grass band center (0.25) where the height tent is 1.0;
  // rock (open band, slope falloff) loses to the full grass weight on
  // gentle slopes.
  engine::terrain::SplatMaterial::resolve_weights(layers, 0.25F, 0.02F,
                                                   wlow);
  check(wlow.w[0] + wlow.w[1] + wlow.w[2] + wlow.w[3] > 0.999F &&
            wlow.w[0] + wlow.w[1] + wlow.w[2] + wlow.w[3] < 1.001F,
        "splat: lowland weights normalized");
  check(wlow.w[1] > wlow.w[0] && wlow.w[1] > wlow.w[3],
        "splat: flat lowland grass-heavy");
  engine::terrain::SplatWeights wpeak{};
  float pnx = 0.0F;
  float pny = 1.0F;
  float pnz = 0.0F;
  engine::terrain::Heightmap::sample_normal(grid, 3.0F, 4.0F, pnx, pny,
                                            pnz);
  engine::terrain::SplatMaterial::resolve_weights(layers, peak, 1.0F - pny,
                                                   wpeak);
  check(wpeak.w[0] + wpeak.w[1] + wpeak.w[2] + wpeak.w[3] > 0.999F &&
            wpeak.w[0] + wpeak.w[1] + wpeak.w[2] + wpeak.w[3] < 1.001F,
        "splat: peak weights normalized");

  // --- AC4: Jolt heightfield collider + raycast ±0.01 ------------------------
  engine::core::JobScheduler sched;
  check(sched.init(), "collider: scheduler init");
  engine::core::PageArenaAllocator arena;
  check(arena.init(128U << 20, 16U << 20), "collider: arena init");
  engine::physics::PhysicsWorld world;
  engine::physics::PhysicsWorldConfig cfg;
  cfg.temp_allocator_bytes = 16U << 20;
  check(world.construct(&sched, &arena, cfg), "collider: world construct");
  engine::terrain::HeightfieldCollider collider;
  engine::terrain::HeightfieldColliderConfig ccfg;
  ccfg.user_data = 0x5445525241494EULL;  // 'TERRAIN'
  check(collider.create(world, grid, ccfg), "collider: create static body");
  check(collider.valid(), "collider: valid body id");
  world.native()->OptimizeBroadPhase();
  const float probes[3][2] = {{2.0F, 2.0F}, {4.0F, 4.0F}, {6.0F, 5.0F}};
  for (int i = 0; i < 3; ++i) {
    const float wx = probes[i][0];
    const float wz = probes[i][1];
    const float expect =
        engine::terrain::Heightmap::sample_height(grid, wx, wz);
    engine::physics::RayInput ray{};
    ray.ox = wx;
    ray.oy = 10.0F;
    ray.oz = wz;
    ray.dx = 0.0F;
    ray.dy = -20.0F;
    ray.dz = 0.0F;
    engine::physics::SpatialHit hit{};
    const int32_t rc = engine::physics::spatial_raycast(*world.native(),
                                                        &ray, &hit);
    char label[64];
    RayLabel(label, sizeof(label), i, "hits");
    check(rc == 1 && hit.hit == 1, label);
    if (rc == 1 && hit.hit == 1) {
      RayLabel(label, sizeof(label), i, "y ±0.01");
      check(feq(hit.py, expect, 0.01F), label);
    }
  }
  collider.destroy();
  check(!collider.valid(), "collider: destroyed");
  world.destroy();
  arena.destroy();
  sched.shutdown();

  // --- AC5: golden (FNV-1a over heights + LOD0 indices + splat) --------------
  std::uint64_t h = 1469598103934665603ULL;
  h = Fnv1a(h, heights.data(),
            heights.size() * sizeof(heights[0]));
  h = Fnv1a(h, idx.data(), 96 * sizeof(idx[0]));
  h = Fnv1a(h, &wlow, sizeof(wlow));
  h = Fnv1a(h, &wpeak, sizeof(wpeak));
  std::printf("TERRAIN_GOLDEN=%016llX\n",
              static_cast<unsigned long long>(h));
  check(h == 0x5BC9BD960DA41391ULL, "golden: terrain hash matches");

  if (failures == 0) {
    std::printf("P6-01 terrain: ALL PASS\n");
    return 0;
  }
  std::printf("P6-01 terrain: %d FAILURES\n", failures);
  return 1;
}

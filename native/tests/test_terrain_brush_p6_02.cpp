// test_terrain_brush_p6_02.cpp — P6-02 acceptance: terrain editing tools.
//
// AC1 — radius containment: a raise dab on a flat 16x16 grid changes
//   heights ONLY inside the brush radius (every sample at d >= radius is
//   bit-exactly untouched); the center rises by strength * falloff(0).
// AC2 — brush modes: lower mirrors raise; smooth reduces variance on a
//   spike grid without touching outside samples; flatten converges to the
//   target at the center.
// AC3 — splat painting: paint_splat pulls weights toward the target layer
//   inside the radius only; weights stay normalized (sum 1); outside
//   samples untouched.
// AC4 — stroke undo: begin_stroke + dab + undo_stroke restores the exact
//   previous heights (memcmp bit-exact) for every mode; splat undo
//   restores weights; undo on an empty journal fails cleanly.
// AC5 — falloff shape: falloff(0)==1, falloff(r)==0, monotone decreasing.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "engine/terrain/terrain_brush.hpp"
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

constexpr std::uint32_t kW = 16;
constexpr std::uint32_t kH = 16;
constexpr float kSpacing = 1.0F;

// Flat grid helper: all heights = base.
struct Grid {
  std::vector<float> h;
  std::vector<float> scratch;
  std::vector<engine::terrain::SplatWeights> splat;
  engine::terrain::HeightmapGrid view{};

  explicit Grid(float base = 0.0F) : h(kW * kH, base), scratch(kW * kH, 0.0F) {
    splat.resize(static_cast<std::size_t>(kW) * kH);
    for (auto& sw : splat) {
      sw.w[0] = 0.25F;
      sw.w[1] = 0.25F;
      sw.w[2] = 0.25F;
      sw.w[3] = 0.25F;
    }
    view.samples = h.data();
    view.width = kW;
    view.height = kH;
    view.spacing = kSpacing;
    view.min_height = -100.0F;
    view.max_height = 100.0F;
  }

  void rebind() { view.samples = h.data(); }
};

auto dist(float x, float z, float cx, float cz) -> float {
  const float dx = x - cx;
  const float dz = z - cz;
  return std::sqrt(dx * dx + dz * dz);
}

}  // namespace

int main() {
  using namespace engine::terrain;

  // --- AC5: falloff shape ---------------------------------------------
  check(feq(TerrainBrush::falloff(0.0F, 3.0F), 1.0F, 1e-6F), "falloff center 1");
  check(feq(TerrainBrush::falloff(3.0F, 3.0F), 0.0F, 1e-6F), "falloff edge 0");
  check(feq(TerrainBrush::falloff(99.0F, 3.0F), 0.0F, 1e-6F), "falloff outside 0");
  check(feq(TerrainBrush::falloff(1.0F, 0.0F), 0.0F, 1e-6F), "falloff degenerate radius 0");
  {
    const float a = TerrainBrush::falloff(0.5F, 3.0F);
    const float b = TerrainBrush::falloff(1.5F, 3.0F);
    const float c = TerrainBrush::falloff(2.5F, 3.0F);
    check(a > b && b > c && c > 0.0F, "falloff monotone decreasing");
  }

  // --- AC1: raise containment ------------------------------------------
  {
    Grid g(5.0F);
    const float cx = 7.5F;
    const float cz = 7.5F;
    const float radius = 3.0F;
    BrushParams p;
    p.mode = BrushMode::kRaise;
    p.center_x = cx;
    p.center_z = cz;
    p.radius = radius;
    p.strength = 2.0F;
    check(TerrainBrush::apply_brush(g.view, g.h.data(), p, nullptr), "raise apply ok");

    bool inside_changed = false;
    bool outside_clean = true;
    bool center_exact = false;
    for (std::uint32_t z = 0; z < kH; ++z) {
      for (std::uint32_t x = 0; x < kW; ++x) {
        const float d = dist(static_cast<float>(x), static_cast<float>(z), cx, cz);
        const float v = g.h[static_cast<std::size_t>(z) * kW + x];
        if (d < radius) {
          const float expect = 5.0F + 2.0F * TerrainBrush::falloff(d, radius);
          if (!feq(v, expect, 1e-5F)) {
            outside_clean = false;  // reuse flag: any mismatch fails
          } else if (v != 5.0F) {
            inside_changed = true;
          }
          // Center sample (nearest grid point): exact delta check.
          if (x == 7 && z == 7) {
            center_exact = feq(v, 5.0F + 2.0F * TerrainBrush::falloff(d, radius), 1e-5F);
          }
        } else {
          if (v != 5.0F) {  // bit-exact no-touch outside
            outside_clean = false;
          }
        }
      }
    }
    check(inside_changed, "raise changes inside");
    check(outside_clean, "raise inside matches falloff curve");
    check(center_exact, "raise center exact delta");
    // Strict outside pass (separate loop for a clear signal).
    bool strict_out = true;
    for (std::uint32_t z = 0; z < kH; ++z) {
      for (std::uint32_t x = 0; x < kW; ++x) {
        const float d = dist(static_cast<float>(x), static_cast<float>(z), cx, cz);
        if (d >= radius &&
            g.h[static_cast<std::size_t>(z) * kW + x] != 5.0F) {
          strict_out = false;
        }
      }
    }
    check(strict_out, "raise outside bit-exact untouched");
  }

  // --- AC2a: lower mirrors raise ----------------------------------------
  {
    Grid g(5.0F);
    BrushParams p;
    p.mode = BrushMode::kLower;
    p.center_x = 7.5F;
    p.center_z = 7.5F;
    p.radius = 3.0F;
    p.strength = 2.0F;
    check(TerrainBrush::apply_brush(g.view, g.h.data(), p, nullptr), "lower apply ok");
    bool all_le = true;
    bool center_down = false;
    for (std::uint32_t z = 0; z < kH; ++z) {
      for (std::uint32_t x = 0; x < kW; ++x) {
        const float v = g.h[static_cast<std::size_t>(z) * kW + x];
        if (v > 5.0F) {
          all_le = false;
        }
        if (x == 7 && z == 7 && v < 5.0F) {
          center_down = true;
        }
      }
    }
    check(all_le, "lower never raises");
    check(center_down, "lower center descends");
  }

  // --- AC2b: smooth reduces a spike, keeps outside ----------------------
  {
    Grid g(5.0F);
    g.h[static_cast<std::size_t>(8) * kW + 8] = 25.0F;  // spike
    g.rebind();
    BrushParams p;
    p.mode = BrushMode::kSmooth;
    p.center_x = 8.0F;
    p.center_z = 8.0F;
    p.radius = 3.0F;
    p.strength = 1.0F;
    check(TerrainBrush::apply_brush(g.view, g.h.data(), p, g.scratch.data()),
          "smooth apply ok");
    const float peak = g.h[static_cast<std::size_t>(8) * kW + 8];
    check(peak < 25.0F && peak > 5.0F, "smooth tames spike");
    bool out_clean = true;
    for (std::uint32_t z = 0; z < kH; ++z) {
      for (std::uint32_t x = 0; x < kW; ++x) {
        const float d = dist(static_cast<float>(x), static_cast<float>(z), 8.0F, 8.0F);
        if (d >= 3.0F && g.h[static_cast<std::size_t>(z) * kW + x] != 5.0F) {
          out_clean = false;
        }
      }
    }
    check(out_clean, "smooth outside untouched");
    // Smooth without scratch must fail, grid untouched.
    Grid g2(5.0F);
    g2.h[static_cast<std::size_t>(8) * kW + 8] = 25.0F;
    g2.rebind();
    check(!TerrainBrush::apply_brush(g2.view, g2.h.data(), p, nullptr),
          "smooth without scratch rejected");
    check(g2.h[static_cast<std::size_t>(8) * kW + 8] == 25.0F, "rejected smooth no-touch");
  }

  // --- AC2c: flatten converges to target --------------------------------
  {
    Grid g(0.0F);
    for (std::uint32_t z = 0; z < kH; ++z) {
      for (std::uint32_t x = 0; x < kW; ++x) {
        g.h[static_cast<std::size_t>(z) * kW + x] =
            static_cast<float>((x + z) % 7);  // rough terrain 0..6
      }
    }
    g.rebind();
    BrushParams p;
    p.mode = BrushMode::kFlatten;
    p.center_x = 8.0F;
    p.center_z = 8.0F;
    p.radius = 4.0F;
    p.strength = 1.0F;
    p.target_height = 3.0F;
    check(TerrainBrush::apply_brush(g.view, g.h.data(), p, nullptr), "flatten apply ok");
    // Single dab converges by strength*falloff: exact center (d=0) hits
    // the target; ring samples move partway (verify against the curve).
    bool center_flat = true;
    for (std::uint32_t z = 7; z <= 9; ++z) {
      for (std::uint32_t x = 7; x <= 9; ++x) {
        const float dx = static_cast<float>(x) - 8.0F;
        const float dz = static_cast<float>(z) - 8.0F;
        const float d = std::sqrt(dx * dx + dz * dz);
        const float f = TerrainBrush::falloff(d, 4.0F);
        const float before = static_cast<float>((x + z) % 7);
        const float expect = before + (3.0F - before) * f;  // strength 1
        const float v = g.h[static_cast<std::size_t>(z) * kW + x];
        if (!feq(v, expect, 1e-4F)) {
          center_flat = false;
        }
      }
    }
    check(center_flat, "flatten follows strength*falloff curve");
    check(feq(g.h[static_cast<std::size_t>(8) * kW + 8], 3.0F, 1e-5F),
          "flatten exact center hits target");
  }

  // --- AC3: splat painting ----------------------------------------------
  {
    Grid g(5.0F);
    check(TerrainBrush::paint_splat(g.view, g.splat.data(), 7.5F, 7.5F, 3.0F, 1.0F, 2),
          "splat paint ok");
    bool norm = true;
    bool center_layer2 = false;
    bool outside_clean = true;
    for (std::uint32_t z = 0; z < kH; ++z) {
      for (std::uint32_t x = 0; x < kW; ++x) {
        const auto& sw =
            g.splat[static_cast<std::size_t>(z) * kW + x];
        const float sum = sw.w[0] + sw.w[1] + sw.w[2] + sw.w[3];
        if (!feq(sum, 1.0F, 1e-5F)) {
          norm = false;
        }
        const float d = dist(static_cast<float>(x), static_cast<float>(z), 7.5F, 7.5F);
        if (d < 3.0F) {
          if (x == 7 && z == 7) {
            // One dab moves partway: expect the exact lerp value, and a
            // second dab at full strength must keep converging.
            const float f = TerrainBrush::falloff(d, 3.0F);
            const float w2exp = (0.25F + 0.75F * f) / (0.25F + 0.75F * f + 3.0F * 0.25F * (1.0F - f));
            center_layer2 = feq(sw.w[2], w2exp, 1e-5F) && (sw.w[2] > 0.25F);
          }
        } else {
          for (int i = 0; i < 4; ++i) {
            if (sw.w[i] != 0.25F) {
              outside_clean = false;
            }
          }
        }
      }
    }
    check(norm, "splat weights normalized");
    check(center_layer2, "splat center pulled to layer 2");
    check(outside_clean, "splat outside untouched");
    check(!TerrainBrush::paint_splat(g.view, g.splat.data(), 7.5F, 7.5F, 3.0F, 1.0F, 9),
          "splat bad layer rejected");
  }

  // --- AC4: stroke undo (bit-exact) --------------------------------------
  {
    // Journal room: 64-entry ring, 8x8 max bbox per entry.
    std::vector<StrokeEntry> entries(64);
    std::vector<float> hstore(64 * 64, 0.0F);
    std::vector<SplatWeights> sstore(64 * 64);
    StrokeJournal j;
    j.entries = entries.data();
    j.height_store = hstore.data();
    j.splat_store = sstore.data();
    j.max_entries = 64;
    j.entry_capacity = 64;

    check(TerrainBrush::undo_count(j) == 0, "journal starts empty");
    check(!TerrainBrush::undo_stroke(j, nullptr, nullptr, kW, kH), "empty undo fails");

    for (int mode_i = 0; mode_i < 4; ++mode_i) {
      Grid g(5.0F);
      // Rough pre-stroke content so restore is meaningful.
      for (std::uint32_t z = 0; z < kH; ++z) {
        for (std::uint32_t x = 0; x < kW; ++x) {
          g.h[static_cast<std::size_t>(z) * kW + x] =
              5.0F + 0.1F * static_cast<float>((x * 3 + z * 7) % 11);
        }
      }
      g.rebind();
      std::vector<float> before = g.h;

      BrushParams p;
      p.mode = static_cast<BrushMode>(mode_i);
      p.center_x = 8.0F;
      p.center_z = 8.0F;
      p.radius = 2.5F;
      p.strength = (mode_i < 2) ? 1.5F : 1.0F;
      p.target_height = 7.0F;
      check(TerrainBrush::begin_stroke(j, g.view, g.h.data(), nullptr, 8.0F, 8.0F, 2.5F,
                                       true, false),
            "begin_stroke heights ok");
      float* scr = (mode_i == 2) ? g.scratch.data() : nullptr;
      check(TerrainBrush::apply_brush(g.view, g.h.data(), p, scr), "stroke dab ok");
      check(std::memcmp(g.h.data(), before.data(), before.size() * sizeof(float)) != 0,
            "stroke changed heights");
      check(TerrainBrush::undo_stroke(j, g.h.data(), nullptr, kW, kH), "undo ok");
      check(std::memcmp(g.h.data(), before.data(), before.size() * sizeof(float)) == 0,
            "undo restores exact heights");
    }

    // Splat undo path.
    {
      Grid g(5.0F);
      std::vector<SplatWeights> wbefore = g.splat;
      check(TerrainBrush::begin_stroke(j, g.view, nullptr, g.splat.data(), 7.5F, 7.5F,
                                       2.5F, false, true),
            "begin_stroke splat ok");
      check(TerrainBrush::paint_splat(g.view, g.splat.data(), 7.5F, 7.5F, 2.5F, 1.0F, 0),
            "splat stroke ok");
      check(TerrainBrush::undo_stroke(j, nullptr, g.splat.data(), kW, kH),
            "splat undo ok");
      check(std::memcmp(g.splat.data(), wbefore.data(),
                        wbefore.size() * sizeof(SplatWeights)) == 0,
            "splat undo restores exact weights");
    }
    check(TerrainBrush::undo_count(j) == 0, "journal drained");
  }

  // --- bad-arg guards ----------------------------------------------------
  {
    Grid g(5.0F);
    BrushParams p;
    p.radius = 0.0F;
    check(!TerrainBrush::apply_brush(g.view, g.h.data(), p, nullptr), "zero radius rejected");
    check(!TerrainBrush::paint_splat(g.view, nullptr, 1, 1, 2, 1, 0), "null splat rejected");
    StrokeJournal empty{};
    check(!TerrainBrush::begin_stroke(empty, g.view, g.h.data(), nullptr, 1, 1, 2, true,
                                      false),
          "null journal rejected");
  }

  if (failures == 0) {
    std::printf("ALL PASS\n");
    return 0;
  }
  std::printf("%d FAILURES\n", failures);
  return 1;
}

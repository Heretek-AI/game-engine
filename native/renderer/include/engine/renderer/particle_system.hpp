// particle_system.hpp — P5-05: GPU particle system CPU contract.
//
// ParticleSystem holds the emission + appearance settings from spec §6
// P5-05 (rate, burst, lifetime, start speed/size/color, gravity, color
// and size over lifetime, blend mode). Particle is one live particle
// (48 bytes, align 4 — plain data, no frame-path allocation).
//
// Bit-exact CPU reference: ParticleSimulateCpu advances the same fixed
// Euler step the compute shaders run (emit.comp spawns with the same
// PCG32 stream seeded per (seed, spawn_index); update.comp integrates
// vel += gravity*dt, pos += vel*dt, age += dt, kills at age >= life).
// The GPU golden test runs the CPU reference twice with the same seed
// and asserts byte-identical buffers, then runs the GPU path and
// compares against the checked-in golden PNG (P1-05 tolerance).
//
// Blend modes: kAlpha (sorted back-to-front by view depth, SRC_ALPHA +
// ONE_MINUS_SRC_ALPHA) and kAdditive (unsorted, SRC_ALPHA + ONE).
// Sorting is a CPU heapsort of draw indices over view depth (<2 ms at
// keeps the lavapipe path free of a GPU sort kernel).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"

namespace engine::renderer {

// Blend mode for the billboard pass (matches the graphics pipelines the
// test builds: alpha = sorted SRC_ALPHA/ONE_MINUS_SRC_ALPHA, additive =
// unsorted SRC_ALPHA/ONE).
enum class ParticleBlend : std::uint32_t {
  kAlpha = 0,
  kAdditive = 1,
};

// Emission + appearance settings (spec §6 P5-05). 64 bytes, align 4.
struct ParticleSystem {
  float emission_rate{0.0F};   // particles per second (continuous)
  float burst_count{0.0F};     // particles per burst tick (0 = no bursts)
  float burst_interval{0.0F};  // seconds between bursts (0 = no bursts)
  float lifetime{1.0F};        // seconds each particle lives
  float start_speed{1.0F};     // spawn speed (units/s, cone around +Y)
  float start_size{1.0F};      // spawn billboard size (world units)
  float start_color[4]{1.0F, 1.0F, 1.0F, 1.0F};  // spawn tint (RGBA)
  float gravity{0.0F};         // Y acceleration (units/s^2, negative = down)
  float end_color[4]{1.0F, 1.0F, 1.0F, 0.0F};  // tint at age == life
  float end_size{1.0F};        // billboard size at age == life
  std::uint32_t blend{0};      // ParticleBlend as u32 (kAlpha default)
  std::uint32_t seed{0};       // fixed seed for deterministic emission
  std::uint32_t pad[1]{0};
};
static_assert(sizeof(ParticleSystem) == 76, "ParticleSystem must be 76 bytes");
static_assert(alignof(ParticleSystem) == 4, "ParticleSystem 4-byte aligned");

// One live particle (CPU mirror of the compute SSBO record). 60 bytes.
struct Particle {
  float pos[3]{0.0F, 0.0F, 0.0F};
  float vel[3]{0.0F, 0.0F, 0.0F};
  float age{0.0F};
  float life{1.0F};
  float size0{1.0F};
  float seed{0.0F};  // spawn random [0,1) for size/color jitter echo
  float color0[4]{1.0F, 1.0F, 1.0F, 1.0F};
  float pad{0.0F};
};
static_assert(sizeof(Particle) == 60, "Particle must be 60 bytes");
static_assert(alignof(Particle) == 4, "Particle must be 4-byte aligned");

// PCG32 (O'Neill minimal): the same stream the emit shader draws from.
// state = seed * 2 + 1 applied per spawn index; output is the XSH-RR u32.
[[nodiscard]] inline auto ParticlePcg32(std::uint64_t* state) noexcept
    -> std::uint32_t {
  const std::uint64_t old = *state;
  *state = old * 6364136223846793005ULL + 1442695040888963407ULL;
  const std::uint32_t xorshifted =
      static_cast<std::uint32_t>(((old >> 18u) ^ old) >> 27u);
  const std::uint32_t rot = static_cast<std::uint32_t>(old >> 59u);
  return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
}

// Spawn RNG: k-th spawned particle of a system draws from an independent
// PCG32 stream seeded by (system seed, spawn index) — order-independent,
// so CPU and GPU emit paths agree regardless of dispatch shape.
[[nodiscard]] inline auto ParticleSpawnRand(std::uint32_t seed,
                                            std::uint32_t spawn_index,
                                            std::uint32_t draw) noexcept
    -> float {
  // Seed stream: s = ((seed << 32) | spawn_index) * 2 + 1, wrapping.
  // Matches the emit shader's split-word arithmetic exactly.
  std::uint32_t lo = spawn_index * 2u + 1u;
  std::uint32_t hi = seed * 2u + (spawn_index >> 31u);
  std::uint64_t state =
      (static_cast<std::uint64_t>(hi) << 32u) | lo;
  std::uint32_t r = ParticlePcg32(&state);
  for (std::uint32_t i = 0; i < draw; ++i) {
    r = ParticlePcg32(&state);
  }
  return static_cast<float>(r >> 8u) * (1.0F / 16777216.0F);
}

// Fixed-step CPU reference: advances `count` slots by `dt` seconds.
// `alive` (in/out): packed count of live particles in slots [0, alive).
// `spawned_total` (in/out): running spawn counter feeding ParticleSpawnRand.
// Emission: continuous accumulator (rate*dt carried in `emit_carry`) plus
// periodic bursts (burst_count every burst_interval, tracked in
// `burst_clock`). Dead slots are compacted by swap-with-last so the live
// prefix stays packed — the same layout the GPU path compacts to before
// the sort + draw. Returns the new alive count.
ENGINE_CORE_API auto ParticleSimulateCpu(ParticleSystem* sys, Particle* slots,
                                         std::uint32_t capacity,
                                         std::uint32_t* alive,
                                         std::uint32_t* spawned_total,
                                         float* emit_carry, float* burst_clock,
                                         float dt) noexcept -> std::uint32_t;

// Sort key fill: squared view depth of each live particle along +Z
// (camera looks down -Z; larger depth = farther = drawn first).
ENGINE_CORE_API void ParticleSortKeys(const Particle* slots,
                                      std::uint32_t alive,
                                      const float* view_row2,
                                      float* keys_out) noexcept;

// Index sort (ascending keys; caller reverses for back-to-front alpha).
// Deterministic: ties break by slot index, so the golden never depends
// on sort stability. `idx` inits to 0..alive-1. In-place heapsort:
// O(n log n), O(1) extra space, no malloc/new on the frame path.
ENGINE_CORE_API void ParticleSortIndices(const float* keys,
                                         std::uint32_t* idx,
                                         std::uint32_t alive) noexcept;

}  // namespace engine::renderer

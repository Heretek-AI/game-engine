// particle_system.cpp — P5-05 CPU reference (bit-exact contract for the
// compute emit/update shaders + the sort used before the alpha draw).
//
// No malloc/new on the frame path here either: all buffers are
// caller-owned (slots, idx, keys); this TU only fills them.

#include "engine/renderer/particle_system.hpp"

namespace engine::renderer {

auto ParticleSimulateCpu(ParticleSystem* sys, Particle* slots,
                         std::uint32_t capacity, std::uint32_t* alive,
                         std::uint32_t* spawned_total, float* emit_carry,
                         float* burst_clock, float dt) noexcept
    -> std::uint32_t {
  if (sys == nullptr || slots == nullptr || alive == nullptr ||
      spawned_total == nullptr || emit_carry == nullptr ||
      burst_clock == nullptr || capacity == 0 || dt <= 0.0F) {
    return (alive != nullptr) ? *alive : 0;
  }
  // 1. Integrate + age; swap-kill the dead so [0, alive) stays packed.
  std::uint32_t n = *alive;
  std::uint32_t i = 0;
  while (i < n) {
    Particle* p = &slots[i];
    p->age += dt;
    if (p->age >= p->life) {
      --n;
      if (i != n) {
        slots[i] = slots[n];
      }
      continue;
    }
    p->vel[1] += sys->gravity * dt;
    p->pos[0] += p->vel[0] * dt;
    p->pos[1] += p->vel[1] * dt;
    p->pos[2] += p->vel[2] * dt;
    ++i;
  }
  // 2. Continuous emission (fractional accumulator survives across steps).
  float want = *emit_carry + sys->emission_rate * dt;
  std::uint32_t burst = 0;
  if (sys->burst_count > 0.0F && sys->burst_interval > 0.0F) {
    *burst_clock += dt;
    while (*burst_clock >= sys->burst_interval) {
      *burst_clock -= sys->burst_interval;
      burst += static_cast<std::uint32_t>(sys->burst_count);
    }
  }
  std::uint32_t spawn = static_cast<std::uint32_t>(want);
  *emit_carry = want - static_cast<float>(spawn);
  spawn += burst;
  // 3. Spawn into the packed tail; each spawn draws 4 RNG values
  // (azimuth, elevation jitter, speed jitter, size jitter) from its own
  // (seed, spawn_index) stream — dispatch-shape independent.
  constexpr float kPi = 3.14159265358979323846F;
  while (spawn > 0 && n < capacity) {
    const std::uint32_t k = *spawned_total;
    const float u0 = ParticleSpawnRand(sys->seed, k, 0);
    const float u1 = ParticleSpawnRand(sys->seed, k, 1);
    const float u2 = ParticleSpawnRand(sys->seed, k, 2);
    const float u3 = ParticleSpawnRand(sys->seed, k, 3);
    Particle* p = &slots[n];
    const float az = u0 * 2.0F * kPi;
    const float tilt = (u1 - 0.5F) * 0.6F;  // cone around +Y
    const float speed = sys->start_speed * (0.7F + 0.6F * u2);
    p->vel[0] = std::cos(az) * tilt * speed;
    p->vel[1] = speed;
    p->vel[2] = std::sin(az) * tilt * speed;
    p->pos[0] = 0.0F;
    p->pos[1] = 0.0F;
    p->pos[2] = 0.0F;
    p->age = 0.0F;
    p->life = sys->lifetime;
    p->size0 = sys->start_size * (0.8F + 0.4F * u3);
    p->seed = u0;
    p->color0[0] = sys->start_color[0];
    p->color0[1] = sys->start_color[1];
    p->color0[2] = sys->start_color[2];
    p->color0[3] = sys->start_color[3];
    p->pad = 0.0F;
    ++n;
    ++(*spawned_total);
    --spawn;
  }
  *alive = n;
  return n;
}

void ParticleSortKeys(const Particle* slots, std::uint32_t alive,
                      const float* view_row2, float* keys_out) noexcept {
  if (slots == nullptr || keys_out == nullptr || view_row2 == nullptr) {
    return;
  }
  for (std::uint32_t i = 0; i < alive; ++i) {
    keys_out[i] = slots[i].pos[0] * view_row2[0] +
                  slots[i].pos[1] * view_row2[1] +
                  slots[i].pos[2] * view_row2[2] + view_row2[3];
  }
}

void ParticleSortIndices(const float* keys, std::uint32_t* idx,
                         std::uint32_t alive) noexcept {
  if (keys == nullptr || idx == nullptr) {
    return;
  }
  for (std::uint32_t i = 0; i < alive; ++i) {
    idx[i] = i;
  }
  // In-place heapsort (ascending keys, ties by slot index — fully
  // deterministic regardless of input order). No malloc/new: O(1) extra
  // space, O(n log n); 10k keys sort in ~1 ms (measured in test_cpu_stress).
  // less(a, b): slot a sorts strictly before slot b.
  const auto less = [&](std::uint32_t a, std::uint32_t b) noexcept -> bool {
    if (keys[a] != keys[b]) {
      return keys[a] < keys[b];
    }
    return a < b;
  };
  // Heapify-down from `root` within [0, size).
  for (std::uint32_t start = alive / 2; start > 0;) {
    --start;
    std::uint32_t root = start;
    for (;;) {
      std::uint32_t child = root * 2 + 1;
      if (child >= alive) {
        break;
      }
      if (child + 1 < alive && less(idx[child], idx[child + 1])) {
        ++child;
      }
      if (less(idx[root], idx[child])) {
        const std::uint32_t t = idx[root];
        idx[root] = idx[child];
        idx[child] = t;
        root = child;
      } else {
        break;
      }
    }
  }
  for (std::uint32_t end = alive; end > 1;) {
    --end;
    const std::uint32_t t = idx[0];
    idx[0] = idx[end];
    idx[end] = t;
    std::uint32_t root = 0;
    for (;;) {
      std::uint32_t child = root * 2 + 1;
      if (child >= end) {
        break;
      }
      if (child + 1 < end && less(idx[child], idx[child + 1])) {
        ++child;
      }
      if (less(idx[root], idx[child])) {
        const std::uint32_t s = idx[root];
        idx[root] = idx[child];
        idx[child] = s;
        root = child;
      } else {
        break;
      }
    }
  }
}

}  // namespace engine::renderer

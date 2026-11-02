// particle_gpu_sim.hpp — P5-05: GPU-resident particle simulation.
//
// ParticleGpuSim owns the slot buffer (capacity x 60 B Particle), the
// ParticleSystem params (76 B) and a counters block {alive,
// spawned_total, last_spawned, pad}, plus three compute pipelines:
//   particle_update.comp  — age + integrate slots[0, alive)
//   particle_compact.comp — mode 0: stable in-place removal of the dead;
//                           mode 1: finalize counters after emit
//   particle_emit.comp    — spawn into slots[alive, ...) from the
//                           (seed, spawned_total + i) PCG32 streams
// record_step() records one fixed step (update -> compact -> emit ->
// finalize, with compute barriers between) into a caller command buffer.
// Only the spawn budget (rate accumulator + burst clock, plain scalars)
// lives on the host; it is computed by ParticleSpawnBudget, the same
// function the CPU reference uses, so both paths request identical
// spawns. alive and spawned_total stay on the GPU.
//
// The CPU reference (ParticleSimulateCpu) swap-kills, the GPU compacts
// stably, so the two agree as sets of particles, not slot for slot.
//
//   ParticleGpuSim sim;
//   sim.create(dev, 512);
//   sim.reset(system);                 // params + zeroed counters
//   for (...) sim.record_step(cmd, dt);
//   // submit + wait, then:
//   sim.read_back(out, cap, &alive, &spawned);
//
// Buffers are host-visible + coherent (VMA, device-local preferred), so
// read_back is a memcpy after the caller's fence. Compiles SDK-free:
// create() reports kVulkanLoaderFailed without the SDK.
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"
#include "engine/renderer/engine_status.hpp"
#include "engine/renderer/particle_system.hpp"

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#else
using VkCommandBuffer = void*;
#endif

namespace engine::renderer {

class VulkanDevice;

// 20-byte push shared by the three sim shaders.
struct ParticleSimPush {
  float dt{0.0F};
  float gravity{0.0F};
  std::uint32_t spawn_request{0};
  std::uint32_t capacity{0};
  std::uint32_t mode{0};
};
static_assert(sizeof(ParticleSimPush) == 20, "particle sim push is 20 bytes");

class ENGINE_CORE_API ParticleGpuSim {
 public:
  ParticleGpuSim() noexcept = default;
  ParticleGpuSim(const ParticleGpuSim&) = delete;
  auto operator=(const ParticleGpuSim&) -> ParticleGpuSim& = delete;
  ~ParticleGpuSim() noexcept;

  // Allocate buffers for `capacity` slots and build the three pipelines.
  // capacity 0 or a null device -> kInvalidArgument; double create ->
  // kAlreadyInitialized.
  auto create(VulkanDevice& dev, std::uint32_t capacity) noexcept -> Status;
  auto destroy() noexcept -> void;
  [[nodiscard]] auto ready() const noexcept -> bool { return ready_; }
  [[nodiscard]] auto capacity() const noexcept -> std::uint32_t {
    return capacity_;
  }

  // Upload `sys` as the params block, zero the counters and the host
  // spawn state. Call when no step is in flight.
  auto reset(const ParticleSystem& sys) noexcept -> Status;

  // Record one fixed step of `dt` seconds into `cmd` (outside a render
  // pass). Ends with a compute -> host/vertex-read barrier.
  auto record_step(VkCommandBuffer cmd, float dt) noexcept -> Status;

  // Copy the live prefix into `out` (up to `out_capacity` slots) and the
  // counters. Only valid after the steps' submission has completed.
  auto read_back(Particle* out, std::uint32_t out_capacity,
                 std::uint32_t* alive, std::uint32_t* spawned_total) const
      noexcept -> Status;

 private:
  [[maybe_unused]] ParticleSystem sys_{};
  [[maybe_unused]] float emit_carry_{0.0F};
  [[maybe_unused]] float burst_clock_{0.0F};
  std::uint32_t capacity_{0};
  bool ready_{false};
  // Opaque Vulkan/VMA handles (void* keeps the header SDK-free).
  [[maybe_unused]] void* device_{nullptr};
  [[maybe_unused]] void* vma_alloc_{nullptr};
  [[maybe_unused]] std::uint64_t buf_bits_[3]{0, 0, 0};  // slots, params, counters
  [[maybe_unused]] void* buf_alloc_[3]{nullptr, nullptr, nullptr};
  [[maybe_unused]] void* buf_mapped_[3]{nullptr, nullptr, nullptr};
  [[maybe_unused]] void* set_layout_{nullptr};
  [[maybe_unused]] void* pipe_layout_{nullptr};
  [[maybe_unused]] void* desc_pool_{nullptr};
  [[maybe_unused]] void* desc_set_{nullptr};
  [[maybe_unused]] void* pipes_[3]{nullptr, nullptr, nullptr};  // update, compact, emit
};

}  // namespace engine::renderer

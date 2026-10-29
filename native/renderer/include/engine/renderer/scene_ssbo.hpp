// scene_ssbo.hpp — M3-02: persistent GPU Scene SSBO streamed from Flecs.
//
// The Scene SSBO is the renderer's single source of truth for per-instance
// data: one SceneInstance (world matrix + bounding sphere + material slot)
// per renderable entity, indexed by instance id. The CULL compute shader
// reads it (buffer-device-address), the DRAW vertex shader reads it via
// the bindless set (binding 0).
//
// Streaming contract ("dirty WorldTransform matrices at end of frame"):
// Flecs owns the WorldTransform columns; scene_stream_begin() maps the
// upload ring region, scene_stream_write() copies the dirty column ranges
// (plain memcpy — zero-copy view on the native side, no malloc), and
// scene_stream_end() records the copy into the persistent device buffer.
// The persistent buffer + upload staging are VMA allocations in the real
// path; in the stub path the stream writes into a CPU-side shadow so the
// layout math stays testable without a GPU.
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
#include "engine/renderer/engine_status.hpp"

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#else
using VkBuffer = void*;
using VkDevice = void*;
#endif

namespace engine::renderer {

// One GPU instance record: 96 bytes (64B world matrix + 16B sphere +
// 8B ids + 8B pad). 250k instances = 24MB — fits one VMA allocation.
struct SceneInstance {
  float world[16];      // row-major 4x4 (== EngineWorldTransform::m order)
  float sphere[4];      // xyz center (world), w radius
  std::uint32_t mesh_index;     // index into mesh LUT (M3-03 geometry)
  std::uint32_t material_slot;  // index into bindless material SSBO
  std::uint32_t flags;          // bit0: visible mask; rest reserved
  std::uint32_t pad;
};
static_assert(sizeof(SceneInstance) == 96, "scene instance record is 96B");
static_assert(alignof(SceneInstance) == 4, "scene instance align");

// Material record mirrored in the material SSBO (binding 1): 64 bytes.
struct SceneMaterial {
  float base_color[4];
  float metallic_roughness[4];  // x metallic, y roughness, zw reserved
  std::uint32_t albedo_tex;     // bindless texture slot (0xFFFF = none)
  std::uint32_t sampler_index;
  std::uint32_t flags;
  std::uint32_t pad;
  float emissive[4];
};
static_assert(sizeof(SceneMaterial) == 64, "scene material record is 64B");

// Stream cursor: one begin/write/end triple per frame. `mapped` is the
// ring-mapped CPU pointer; `device_offset` is the byte offset inside the
// persistent Scene SSBO the range will land at.
struct SceneStreamCursor {
  void* mapped{nullptr};
  std::uint64_t device_offset{0};
  std::uint32_t capacity_instances{0};
  std::uint32_t written{0};
};

class ENGINE_CORE_API SceneSsbo {
 public:
  SceneSsbo() noexcept = default;
  SceneSsbo(const SceneSsbo&) = delete;
  auto operator=(const SceneSsbo&) -> SceneSsbo& = delete;
  ~SceneSsbo() noexcept;

  // Reserve CPU shadow for `max_instances` (no GPU). Idempotent.
  auto init(std::uint32_t max_instances) noexcept -> Status;
  auto shutdown() noexcept -> void;
  [[nodiscard]] auto ready() const noexcept -> bool { return ready_; }
  [[nodiscard]] auto max_instances() const noexcept -> std::uint32_t {
    return max_instances_;
  }
  [[nodiscard]] auto instance_bytes() const noexcept -> std::uint64_t {
    return static_cast<std::uint64_t>(max_instances_) *
           sizeof(SceneInstance);
  }

  // Begin a frame stream: maps `count` slots (count <= max). Returns the
  // CPU pointer + device offset; writes go straight into it.
  auto stream_begin(std::uint32_t count, SceneStreamCursor* out) noexcept
      -> Status;
  // Copy one dirty column range (world matrices) into the stream at slot
  // `first_instance`. `matrices` points at `count` packed 64B matrices.
  auto stream_write(const SceneStreamCursor& cursor,
                    std::uint32_t first_instance, const void* matrices,
                    std::uint32_t count) noexcept -> Status;
  auto stream_end(const SceneStreamCursor& cursor) noexcept -> Status;

  // Read back one streamed instance (CPU shadow; works without GPU).
  [[nodiscard]] auto instance_at(std::uint32_t i) const noexcept
      -> const SceneInstance*;

  // GPU path (real Vulkan only): allocate the persistent device SSBO and
  // upload staging via VMA. Without the SDK reports kVulkanLoaderFailed.
  auto create_gpu(VkDevice device, std::uint64_t device_addr_hint) noexcept
      -> Status;
  auto destroy_gpu() noexcept -> void;
  [[nodiscard]] auto gpu_ready() const noexcept -> bool {
    return gpu_buffer_ != nullptr;
  }
  [[nodiscard]] auto gpu_buffer() const noexcept -> VkBuffer {
    return gpu_buffer_;
  }

 private:
  SceneInstance* shadow_{nullptr};
  std::uint32_t max_instances_{0};
  std::uint32_t shadow_written_{0};
  bool ready_{false};
  VkBuffer gpu_buffer_{nullptr};
};

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif

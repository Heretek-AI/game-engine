// bindless_table.hpp — M3-02: single global bindless descriptor set.
//
// One VkDescriptorSet holds every texture / sampler / material buffer the
// frame can reference, indexed by a 32-bit material slot carried in the
// per-instance push data (M3-03 consumes the same set for its PBR pass).
// VK_EXT_descriptor_indexing features (partially-bound array,
// variable-count, non-uniform indexing, runtime array) are enabled by the
// M3-01 device bootstrap; this header adds the set-layout/pipeline-layout
// plan + slot allocator ON TOP of that feature set.
//
// Compiles WITHOUT the Vulkan SDK: all Vulkan handles degrade to void* /
// uint32_t stand-ins and create() reports kVulkanLoaderFailed. The slot
// allocator + SPIR-V bytes (below) are SDK-free and unit-tested on every
// CI runner; the real Vulkan path is exercised on Lavapipe.
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
using VkDescriptorSet = void*;
using VkDescriptorSetLayout = void*;
using VkDevice = void*;
using VkPipelineLayout = void*;
using VkSampler = void*;
using VkImageView = void*;
using VkBuffer = void*;
#endif

namespace engine::renderer {

// Slot budget for the single global set. 4096 texture slots x 4B index =
// 16KiB of push/instance data; comfortably under the 128B push-constant
// limit because shaders page the index from the Scene SSBO, not push.
inline constexpr std::uint32_t kBindlessMaxTextures = 4096;
inline constexpr std::uint32_t kBindlessMaxSamplers = 16;
inline constexpr std::uint32_t kBindlessMaxMaterials = 4096;

// Descriptor bindings inside the global set (fixed for M3-02..M3-03).
inline constexpr std::uint32_t kBindSceneSsbo = 0;    // SceneSsboBlock
inline constexpr std::uint32_t kBindMaterialSsbo = 1;  // MaterialBlock[]
inline constexpr std::uint32_t kBindTextureArray = 2;  // sampled2D[4096]
inline constexpr std::uint32_t kBindSamplerArray = 3;  // sampler[16]

// CPU-side slot allocator: free-list over texture/material slots. No Vulkan
// needed; the GPU write path (update()) consumes the committed slots.
class ENGINE_CORE_API BindlessTable {
 public:
  BindlessTable() noexcept = default;
  BindlessTable(const BindlessTable&) = delete;
  auto operator=(const BindlessTable&) -> BindlessTable& = delete;
  ~BindlessTable() noexcept;

  // Reserve slot storage (no Vulkan). Idempotent; second call is a no-op.
  auto init() noexcept -> Status;
  auto shutdown() noexcept -> void;
  [[nodiscard]] auto ready() const noexcept -> bool { return ready_; }

  // Allocate one texture slot + one material slot. Returns false when the
  // table is full (caller evicts or grows in M3-03).
  auto alloc_texture(std::uint32_t* slot_out) noexcept -> bool;
  auto alloc_material(std::uint32_t* slot_out) noexcept -> bool;
  auto free_texture(std::uint32_t slot) noexcept -> void;
  auto free_material(std::uint32_t slot) noexcept -> void;

  [[nodiscard]] auto texture_used() const noexcept -> std::uint32_t {
    return tex_used_;
  }
  [[nodiscard]] auto material_used() const noexcept -> std::uint32_t {
    return mat_used_;
  }

  // GPU path (real Vulkan only): build the descriptor set layout +
  // pipeline layout for `device` and bind `scene`/`material` SSBO buffers.
  // Without the SDK reports kVulkanLoaderFailed. Safe to call once; second
  // call returns kAlreadyInitialized.
  auto create_gpu(VkDevice device, VkBuffer scene_buffer,
                  std::uint64_t scene_bytes, VkBuffer material_buffer,
                  std::uint64_t material_bytes) noexcept -> Status;
  auto destroy_gpu() noexcept -> void;
  [[nodiscard]] auto gpu_ready() const noexcept -> bool {
    return set_ != nullptr;
  }
  [[nodiscard]] auto descriptor_set() const noexcept -> VkDescriptorSet {
    return set_;
  }
  [[nodiscard]] auto set_layout() const noexcept -> VkDescriptorSetLayout {
    return set_layout_;
  }
  [[nodiscard]] auto pipeline_layout() const noexcept -> VkPipelineLayout {
    return pipeline_layout_;
  }

 private:
  static constexpr std::uint32_t kStackSlots = 64;
  std::uint32_t tex_free_[kBindlessMaxTextures]{};
  std::uint32_t mat_free_[kBindlessMaxMaterials]{};
  std::uint32_t tex_top_{0};
  std::uint32_t mat_top_{0};
  std::uint32_t tex_used_{0};
  std::uint32_t mat_used_{0};
  bool ready_{false};

  VkDescriptorSet set_{nullptr};
  VkDescriptorSetLayout set_layout_{nullptr};
  VkPipelineLayout pipeline_layout_{nullptr};
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  VkDevice device_{nullptr};
#endif
};

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif

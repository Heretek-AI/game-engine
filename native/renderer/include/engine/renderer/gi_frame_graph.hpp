// gi_frame_graph.hpp — M3-04: frame-graph wiring for the GI passes.
//
// Owns the GPU side of the zero-bake GI stack on top of the M3-02/M3-03
// objects the caller already built:
//
//   BindlessTable (set layout + pipeline layout with the push ranges the
//     PBR passes use) + VulkanDevice (device handle) + PbrPipeline blobs.
//
// build() creates the five GI-relevant shader modules (reuses the three
// M3-03 blobs held by PbrPipeline — this class only adds the two new
// passes) plus the two GI graphics pipelines:
//
//   gi_resolve  fullscreen triangle, RGBA16F HDR target, depth off.
//   shadow_trace fullscreen triangle, R32F shadow-factor target.
//
// Dynamic rendering throughout (no VkRenderPass objects — same as M3-03).
// All Vulkan calls go through VulkanDevice::proc() (device-level entry
// points resolved for THIS device; no volk globals in consumer TUs).
// Compiles SDK-free (handles degrade to void*): build() reports
// kVulkanLoaderFailed without the SDK.
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
using VkPipeline = void*;
using VkPipelineLayout = void*;
using VkShaderModule = void*;
using VkDescriptorSetLayout = void*;
#endif

namespace engine::renderer {

class PbrPipeline;
class VulkanDevice;

// Push ranges carved from the bindless pipeline layout (M3-02): the GI
// passes reuse the same layout, so no new set layout is built here.
inline constexpr std::uint32_t kGiVertPushSize = 0;    // fullscreen: no vert push
inline constexpr std::uint32_t kGiResolvePushWords = 16;  // 64B / 4
inline constexpr std::uint32_t kShadowPushWords = 16;    // 64B / 4

class ENGINE_CORE_API GiFrameGraph {
 public:
  GiFrameGraph() noexcept = default;
  GiFrameGraph(const GiFrameGraph&) = delete;
  auto operator=(const GiFrameGraph&) -> GiFrameGraph& = delete;
  ~GiFrameGraph() noexcept;

  // Build the GI modules + pipelines against `device` using the bindless
  // SET layout (owned by BindlessTable): this creates a dedicated GI
  // pipeline layout with the fragment-stage push range the GI resolve +
  // shadow shaders need (bindless create_gpu() ships zero push ranges —
  // the M3-03 PBR passes never recorded against a layout either, see the
  // deferred create_gpu() note). Modules are owned here; blobs are
  // borrowed. Null blobs/handles -> kInvalidArgument; double-build ->
  // kAlreadyInitialized.
  auto build(const VulkanDevice& dev, VkDescriptorSetLayout bindless_set,
             const std::uint32_t* gi_vert, std::size_t gi_vert_words,
             const std::uint32_t* gi_frag, std::size_t gi_frag_words,
             const std::uint32_t* shadow_vert, std::size_t shadow_vert_words,
             const std::uint32_t* shadow_frag, std::size_t shadow_frag_words,
             const PbrPipeline& pbr) noexcept -> Status;
  auto destroy() noexcept -> void;

  [[nodiscard]] auto ready() const noexcept -> bool { return ready_; }
  [[nodiscard]] auto gi_pipeline() const noexcept -> VkPipeline {
    return gi_pipeline_;
  }
  [[nodiscard]] auto shadow_pipeline() const noexcept -> VkPipeline {
    return shadow_pipeline_;
  }
  [[nodiscard]] auto pipeline_layout() const noexcept -> VkPipelineLayout {
    return pipe_layout_;
  }
  // Owned module count (4: gi vert/frag + shadow vert/frag).
  [[nodiscard]] auto module_count() const noexcept -> std::uint32_t {
    return module_count_;
  }

 private:
  VkDevice device_{nullptr};
  void* proc_create_module_{nullptr};
  void* proc_destroy_module_{nullptr};
  void* proc_create_pipelines_{nullptr};
  void* proc_destroy_pipeline_{nullptr};
  void* proc_create_layout_{nullptr};
  void* proc_destroy_layout_{nullptr};
  void* proc_destroy_set_layout_{nullptr};
  VkDescriptorSetLayout set1_layout_{nullptr};
  VkShaderModule modules_[4]{nullptr, nullptr, nullptr, nullptr};
  VkPipeline gi_pipeline_{nullptr};
  VkPipeline shadow_pipeline_{nullptr};
  VkPipelineLayout pipe_layout_{nullptr};
  std::uint32_t module_count_{0};
  bool ready_{false};
};

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif

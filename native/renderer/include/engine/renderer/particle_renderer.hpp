// particle_renderer.hpp — P5-05: GPU particle billboard pipelines.
//
// ParticleRenderer owns two graphics pipelines (particle.vert/
// particle.frag, embedded as particle_*_spv.inc) against a swapchain
// UNORM target: one sorted-alpha (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) and
// one additive (SRC_ALPHA, ONE). Both share one pipeline layout with an
// 80-byte push constant (ParticlePush { view_proj[16], viewport_px[2],
// pad[2] } — same shape as SpritePush so the vert shader's 80B read
// stays in bounds).
//
//   ParticleRenderer pr;
//   pr.create(device, swapchain_format);          // builds modules+pipes
//   // inside an open VkCommandBuffer with `view` in COLOR_ATTACHMENT_OPTIMAL:
//   pr.begin_particle_pass(cmd, view, extent);    // CLEAR, no depth
//   pr.record_draw(cmd, mesh, push, ParticleBlend::kAlpha, ...);
//   pr.end_particle_pass(cmd);
//
// Vertex input is one interleaved binding of ParticleVertex (pos[3]
// R32G32B32_SFLOAT @0, color[4] @12, size R32_SFLOAT @28, corner[2]
// R32G32_SFLOAT @32 = 40 bytes), triangle-list, FILL/NONE/CCW, no depth
// test, viewport+scissor dynamic. record_draw binds the quad buffers in
// `mesh` (uploaded ParticleVertex/u32 data) and issues exactly one
// vkCmdDrawIndexed per call.
//
// Compiles SDK-free (handles degrade to void*): create() reports
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
#include "engine/renderer/vk_extent_stub.hpp"

namespace engine::renderer {

class VulkanDevice;
class GpuMesh;

}  // namespace engine::renderer

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#else
#include "engine/renderer/vk_extent_stub.hpp"
using VkAttachmentLoadOp = std::uint32_t;
using VkCommandBuffer = void*;
using VkDevice = void*;
using VkFormat = std::uint32_t;
using VkImageView = void*;
using VkPipeline = void*;
using VkPipelineLayout = void*;
using VkShaderModule = void*;
#endif

namespace engine::renderer {

// One billboard corner: particle center + age-lerped color + age-lerped
// size + quad corner in [-0.5, 0.5]. 40 bytes, align 4 (plain data).
struct ParticleVertex {
  float pos[3]{0.0F, 0.0F, 0.0F};
  float color[4]{1.0F, 1.0F, 1.0F, 1.0F};
  float size{1.0F};
  float corner[2]{0.0F, 0.0F};
};
static_assert(sizeof(ParticleVertex) == 40, "particle vertex is 40 bytes");
static_assert(alignof(ParticleVertex) == 4, "particle vertex 4-aligned");

// 80-byte push constant shared by particle.vert/particle.frag.
struct ParticlePush {
  float view_proj[16]{};
  float viewport_px[2]{0.0F, 0.0F};
  float pad[2]{0.0F, 0.0F};
};
static_assert(sizeof(ParticlePush) == 80, "particle push is 80 bytes");

class ENGINE_CORE_API ParticleRenderer {
 public:
  ParticleRenderer() noexcept = default;
  ParticleRenderer(const ParticleRenderer&) = delete;
  auto operator=(const ParticleRenderer&) -> ParticleRenderer& = delete;
  ~ParticleRenderer() noexcept;

  // Build the particle modules + alpha/additive pipelines against
  // `device` for swapchain-format `color_format` (B8G8R8A8_UNORM or
  // R8G8B8A8_UNORM). Blobs are borrowed (default: the checked-in
  // particle_*_spv.inc). Null device/handles or bad blobs ->
  // kInvalidArgument; double-build -> kAlreadyInitialized.
  auto create(VulkanDevice& dev, VkFormat color_format,
              const std::uint32_t* vert_spv = nullptr,
              std::size_t vert_words = 0,
              const std::uint32_t* frag_spv = nullptr,
              std::size_t frag_words = 0) noexcept -> Status;
  auto destroy() noexcept -> void;

  [[nodiscard]] auto ready() const noexcept -> bool { return ready_; }
  [[nodiscard]] auto alpha_pipeline() const noexcept -> VkPipeline {
    return pipe_alpha_;
  }
  [[nodiscard]] auto additive_pipeline() const noexcept -> VkPipeline {
    return pipe_add_;
  }
  [[nodiscard]] auto pipeline_layout() const noexcept -> VkPipelineLayout {
    return pipe_layout_;
  }
  [[nodiscard]] auto module_count() const noexcept -> std::uint32_t {
    return module_count_;
  }

  // Open a color-only dynamic-rendering pass (CLEAR to transparent
  // black, STORE): the first particle draw of a frame. Returns
  // kNotInitialized when create() has not succeeded.
  auto begin_particle_pass(VkCommandBuffer cmd, VkImageView color_view,
                           VkExtent2D extent) noexcept -> Status;
  // Close the pass opened by begin_particle_pass.
  auto end_particle_pass(VkCommandBuffer cmd) noexcept -> Status;

  // Record ONE indexed draw for `quad_count` billboards (one draw call):
  // binds the quad buffers in `mesh` (uploaded ParticleVertex/u32 data),
  // pushes `push`, draws quad_count*6 indices starting at
  // (first_quad*6). `additive` selects the additive pipeline, otherwise
  // the sorted-alpha pipeline. kNotInitialized when create() has not
  // succeeded; kInvalidArgument on null/empty/out-of-range args.
  auto record_draw(VkCommandBuffer cmd, const GpuMesh& mesh,
                   const ParticlePush& push, bool additive,
                   std::uint32_t first_quad, std::uint32_t quad_count,
                   std::uint32_t total_quads) noexcept -> Status;

 private:
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  [[maybe_unused]] VkDevice device_{nullptr};
  [[maybe_unused]] void* proc_bind_pipeline_{nullptr};
  [[maybe_unused]] void* proc_cmd_viewport_{nullptr};
  [[maybe_unused]] void* proc_cmd_scissor_{nullptr};
  [[maybe_unused]] void* proc_cmd_draw_indexed_{nullptr};
  [[maybe_unused]] void* proc_cmd_bind_vb_{nullptr};
  [[maybe_unused]] void* proc_cmd_bind_ib_{nullptr};
  [[maybe_unused]] void* proc_cmd_push_{nullptr};
  [[maybe_unused]] void* proc_begin_rendering_{nullptr};
  [[maybe_unused]] void* proc_end_rendering_{nullptr};
  [[maybe_unused]] void* proc_create_module_{nullptr};
  [[maybe_unused]] void* proc_destroy_module_{nullptr};
  [[maybe_unused]] void* proc_create_pipelines_{nullptr};
  [[maybe_unused]] void* proc_destroy_pipeline_{nullptr};
  [[maybe_unused]] void* proc_create_layout_{nullptr};
  [[maybe_unused]] void* proc_destroy_layout_{nullptr};
  VkShaderModule modules_[2]{nullptr, nullptr};
  VkPipeline pipe_alpha_{nullptr};
  VkPipeline pipe_add_{nullptr};
  VkPipelineLayout pipe_layout_{nullptr};
#else
  VkShaderModule modules_[2]{nullptr, nullptr};
  VkPipeline pipe_alpha_{nullptr};
  VkPipeline pipe_add_{nullptr};
  VkPipelineLayout pipe_layout_{nullptr};
  [[maybe_unused]] VkDevice device_{nullptr};
#endif
  std::uint32_t module_count_{0};
  bool ready_{false};
};

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif

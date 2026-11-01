// sprite_renderer.hpp — P6-06: GPU sprite pipeline (one draw per atlas).
//
// SpriteRenderer owns one graphics pipeline (sprite.vert/sprite.frag,
// embedded as sprite_*_spv.inc) plus its pipeline layout, and records
// one indexed draw per SpriteBatch:
//
//   SpriteRenderer sr;
//   sr.create(device, swapchain_format);          // builds modules+pipeline
//   // inside an open VkCommandBuffer with `view` in COLOR_ATTACHMENT_OPTIMAL:
//   sr.begin_sprite_pass(cmd, view, extent);      // CLEAR, no depth
//   sr.record_batch(cmd, mesh, view_proj, batch, ...);  // one draw call
//   sr.end_sprite_pass(cmd);
//
// Pipeline: dynamic rendering (no VkRenderPass objects), vertex input =
// interleaved SpriteVertex (pos[2] R32G32_SFLOAT @0, uv[2] @8,
// color[4] @16), triangle-list, FILL/NONE/CCW, no depth test, alpha
// blending (SRC_ALPHA, ONE_MINUS_SRC_ALPHA), viewport+scissor dynamic.
// Push constant: SpritePush { float view_proj[16]; float tint_uv[4]; }
// = 80 bytes (<= 128B minimum).
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
struct SpriteBatch;

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

// 80-byte push constant shared by sprite.vert/sprite.frag.
struct SpritePush {
  float view_proj[16]{};
  float tint_uv[4]{1.0F, 1.0F, 0.0F, 0.0F};
};
static_assert(sizeof(SpritePush) == 80, "sprite push is 80 bytes");

class ENGINE_CORE_API SpriteRenderer {
 public:
  SpriteRenderer() noexcept = default;
  SpriteRenderer(const SpriteRenderer&) = delete;
  auto operator=(const SpriteRenderer&) -> SpriteRenderer& = delete;
  ~SpriteRenderer() noexcept;

  // Build the sprite modules + pipeline against `device` for
  // swapchain-format `color_format` (B8G8R8A8_UNORM or R8G8B8A8_UNORM).
  // Blobs are borrowed (default: the checked-in sprite_*_spv.inc). Null
  // device/handles or bad blobs -> kInvalidArgument; double-build ->
  // kAlreadyInitialized.
  auto create(VulkanDevice& dev, VkFormat color_format,
              const std::uint32_t* vert_spv = nullptr,
              std::size_t vert_words = 0,
              const std::uint32_t* frag_spv = nullptr,
              std::size_t frag_words = 0) noexcept -> Status;
  auto destroy() noexcept -> void;

  [[nodiscard]] auto ready() const noexcept -> bool { return ready_; }
  [[nodiscard]] auto pipeline() const noexcept -> VkPipeline {
    return pipeline_;
  }
  [[nodiscard]] auto pipeline_layout() const noexcept -> VkPipelineLayout {
    return pipe_layout_;
  }
  [[nodiscard]] auto module_count() const noexcept -> std::uint32_t {
    return module_count_;
  }

  // Open a color-only dynamic-rendering pass (CLEAR to transparent
  // black, STORE): the first sprite draw of a frame. Returns
  // kNotInitialized when create() has not succeeded.
  auto begin_sprite_pass(VkCommandBuffer cmd, VkImageView color_view,
                         VkExtent2D extent) noexcept -> Status;
  // Close the pass opened by begin_sprite_pass.
  auto end_sprite_pass(VkCommandBuffer cmd) noexcept -> Status;

  // Record ONE indexed draw for `batch` (one draw call per atlas run):
  // binds the quad buffers in `mesh` (uploaded SpriteVertex/u32 data),
  // pushes `push`, draws batch.sprite_count*6 indices starting at
  // (batch.first_sprite*6). kNotInitialized when create() has not
  // succeeded; kInvalidArgument on null/empty/out-of-range args.
  auto record_batch(VkCommandBuffer cmd, const GpuMesh& mesh,
                    const SpritePush& push, const SpriteBatch& batch,
                    std::uint32_t quad_count) noexcept -> Status;

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
  VkPipeline pipeline_{nullptr};
  VkPipelineLayout pipe_layout_{nullptr};
#else
  VkShaderModule modules_[2]{nullptr, nullptr};
  VkPipeline pipeline_{nullptr};
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

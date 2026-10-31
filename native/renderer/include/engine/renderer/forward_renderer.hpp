// forward_renderer.hpp — P1-06: minimal forward pass (triangle).
//
// ForwardRenderer owns one graphics pipeline (tri.vert/tri.frag, embedded
// as tri_*_spv.inc) plus its pipeline layout, and records a single
// indexed-by-gl_VertexIndex triangle draw into an caller-open command
// buffer:
//
//   ForwardRenderer fr;
//   fr.create(device, swapchain_format);          // builds modules+pipeline
//   // inside an open VkCommandBuffer with `view` in COLOR_ATTACHMENT_OPTIMAL:
//   fr.record_triangle(cmd, view, extent);        // dynamic rendering draw
//   fr.destroy();
//
// Pipeline: dynamic rendering (VkPipelineRenderingCreateInfo, no
// VkRenderPass objects), no vertex input (positions/colors from constant
// arrays indexed by gl_VertexIndex), dynamic viewport + scissor, no
// culling, no depth, no blending. Compiles SDK-free (handles degrade to
// void*): create() reports kVulkanLoaderFailed without the SDK.
#pragma once

#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
#include "engine/renderer/engine_status.hpp"

namespace engine::renderer {

class VulkanDevice;
class GpuMesh;
struct MeshPush;

}  // namespace engine::renderer

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#else
#include "engine/renderer/vk_extent_stub.hpp"
using VkCommandBuffer = void*;
using VkDevice = void*;
using VkFormat = std::uint32_t;
using VkAttachmentLoadOp = std::uint32_t;
using VkImageView = void*;
using VkPipeline = void*;
using VkPipelineLayout = void*;
using VkShaderModule = void*;
#endif

namespace engine::renderer {

// Stub-mode load-op values mirror the Vulkan enum so shared test code
// compiles without the SDK.
#if !defined(ENGINE_RENDERER_HAS_VULKAN)
inline constexpr VkAttachmentLoadOp kStubLoadOpLoad = 0;
inline constexpr VkAttachmentLoadOp kStubLoadOpClear = 1;
#endif

class ENGINE_CORE_API ForwardRenderer {
 public:
  ForwardRenderer() noexcept = default;
  ForwardRenderer(const ForwardRenderer&) = delete;
  auto operator=(const ForwardRenderer&) -> ForwardRenderer& = delete;
  ~ForwardRenderer() noexcept;

  // Build the tri modules + pipeline against `device` for swapchain-format
  // `color_format` (B8G8R8A8_UNORM or R8G8B8A8_UNORM). `depth_format` is
  // reserved for the mesh path: UNDEFINED today, D32_SFLOAT for the lit
  // cube (create_mesh); other values fail closed. Blobs are borrowed
  // (default: the checked-in tri_*_spv.inc). Null device/handles or bad
  // blobs -> kInvalidArgument; double-build -> kAlreadyInitialized.
  auto create(
      VulkanDevice& dev, VkFormat color_format,
#if defined(ENGINE_RENDERER_HAS_VULKAN)
      VkFormat depth_format = VK_FORMAT_UNDEFINED,
#else
      VkFormat depth_format = 0,
#endif
      const std::uint32_t* vert_spv = nullptr, std::size_t vert_words = 0,
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
  [[nodiscard]] auto mesh_ready() const noexcept -> bool {
    return mesh_pipeline_ != nullptr && mesh_layout_ != nullptr;
  }
  [[nodiscard]] auto mesh_module_count() const noexcept -> std::uint32_t {
    return mesh_module_count_;
  }

  // Record a triangle draw into the OPEN command buffer `cmd` targeting
  // `color_view` (must already be in COLOR_ATTACHMENT_OPTIMAL). Opens and
  // closes its own dynamic-rendering pass (CLEAR to black, STORE) with a
  // full-extent viewport/scissor, binds the pipeline, and draws 3 vertices.
  // No-op returning kNotInitialized when create() has not succeeded.
  auto record_triangle(VkCommandBuffer cmd, VkImageView color_view,
                       VkExtent2D extent) noexcept -> Status;

  // Build the mesh modules + pipeline (mesh.vert/mesh.frag, embedded as
  // mesh_*_spv.inc) for `color_format` + D32_SFLOAT depth. Same blob
  // override shape as create(); depth_format MUST be D32_SFLOAT (this is
  // the P1-07 entry — the tri create() stays UNDEFINED-only).
  auto create_mesh(
      VulkanDevice& dev, VkFormat color_format, VkFormat depth_format,
      const std::uint32_t* vert_spv = nullptr, std::size_t vert_words = 0,
      const std::uint32_t* frag_spv = nullptr,
      std::size_t frag_words = 0) noexcept -> Status;

  // Record one indexed mesh draw into the OPEN command buffer `cmd`.
  // `color_view` must be in COLOR_ATTACHMENT_OPTIMAL; `depth_view` in
  // DEPTH_ATTACHMENT_OPTIMAL with the pass cleared on first use (pass
  // management is the caller's: begin with CLEAR, continue with LOAD).
  // Binds vertex/index buffers, pushes `push` (160 bytes), and draws
  // mesh.index_count() indices. kNotInitialized when create_mesh() has
  // not succeeded.
  auto record_mesh(VkCommandBuffer cmd, const GpuMesh& mesh,
                   const MeshPush& push) noexcept -> Status;

  // Open a color+depth dynamic-rendering pass (CLEAR both, STORE color):
  // the first draw of a frame. Returns kNotInitialized when neither
  // pipeline is built.
  auto begin_mesh_pass(VkCommandBuffer cmd, VkImageView color_view,
                       VkImageView depth_view,
                       VkExtent2D extent) noexcept -> Status;
  // Open a color+depth dynamic-rendering pass (LOAD both, STORE color):
  // follow-up draws in the same frame (depth test hides back faces).
  auto continue_mesh_pass(VkCommandBuffer cmd, VkImageView color_view,
                          VkImageView depth_view,
                          VkExtent2D extent) noexcept -> Status;
  // Close the pass opened by begin/continue_mesh_pass.
  auto end_mesh_pass(VkCommandBuffer cmd) noexcept -> Status;

  // Open a color+depth dynamic-rendering pass with the given color/depth
  // load op (CLEAR for begin_mesh_pass, LOAD for continue_mesh_pass),
  // full-extent viewport/scissor, and the mesh pipeline bound.
  auto open_mesh_pass(VkCommandBuffer cmd, VkImageView color_view,
                      VkImageView depth_view, VkExtent2D extent,
                      VkAttachmentLoadOp load_op) noexcept -> Status;

 private:
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  // NOTE: device_ + proc_* are stored (not re-queried) so destroy() can
  // run after VulkanDevice::shutdown(). destroy() references all of them
  // (create-time procs via (void) casts), so no -Wunused-private-field
  // under -Werror. [[maybe_unused]] is the portable suppression.
  [[maybe_unused]] VkDevice device_{nullptr};
  [[maybe_unused]] void* proc_bind_pipeline_{nullptr};
  [[maybe_unused]] void* proc_cmd_viewport_{nullptr};
  [[maybe_unused]] void* proc_cmd_scissor_{nullptr};
  [[maybe_unused]] void* proc_cmd_draw_{nullptr};
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
  [[maybe_unused]] void* proc_limits_{nullptr};  // reserved (see destroy)
  VkShaderModule modules_[2]{nullptr, nullptr};
  VkPipeline pipeline_{nullptr};
  VkPipelineLayout pipe_layout_{nullptr};
  // P1-07 mesh pipeline (second pipeline in the same renderer).
  VkShaderModule mesh_modules_[2]{nullptr, nullptr};
  VkPipeline mesh_pipeline_{nullptr};
  VkPipelineLayout mesh_layout_{nullptr};
  std::uint32_t mesh_module_count_{0};
#else
  VkShaderModule modules_[2]{nullptr, nullptr};
  VkPipeline pipeline_{nullptr};
  VkPipelineLayout pipe_layout_{nullptr};
  VkShaderModule mesh_modules_[2]{nullptr, nullptr};
  VkPipeline mesh_pipeline_{nullptr};
  VkPipelineLayout mesh_layout_{nullptr};
  std::uint32_t mesh_module_count_{0};
#endif
  std::uint32_t module_count_{0};
  bool ready_{false};
};

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif

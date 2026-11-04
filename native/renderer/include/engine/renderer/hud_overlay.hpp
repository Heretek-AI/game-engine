// hud_overlay.hpp — P5-04-followup: textured HUD overlay in the real frame.
//
// HudOverlay owns one graphics pipeline (hud_overlay.vert/
// hud_overlay.frag, embedded as hud_overlay_*_spv.inc) plus the sampled
// HUD texture it composites:
//
//   HudOverlay hud;
//   hud.create(device, swapchain_format);   // builds modules+pipe+texture
//   hud.update(pixels);                     // staging-upload 640x360 RGBA8
//   // inside an open VkCommandBuffer, `view` in COLOR_ATTACHMENT_OPTIMAL:
//   hud.continue_hud_pass(cmd, view, extent);  // LOAD composite, STORE
//   hud.record_draw(cmd);                   // one fullscreen-triangle draw
//   hud.end_hud_pass(cmd);
//   hud.destroy();
//
// Design (see docs/notes/P5-04-followup.md):
//   - The HUD backbuffer is 640x360 RGBA8 *premultiplied* CPU bytes. The
//     frag shader samples them VERBATIM (no sRGB encode) and the blend
//     state is ONE/ONE_MINUS_SRC_ALPHA (premultiplied src-over).
//   - No vertex buffers: fullscreen triangle from gl_VertexIndex.
//   - One combined image sampler (set 0, binding 0, frag stage), NEAREST,
//     CLAMP_TO_EDGE. One pool (1 set), descriptor written at update().
//   - Staging: one host-visible VMA buffer (921600 B, persistent mapped);
//     update() memcpys into it; record_upload() copies buffer->image on a
//     fence-waited one-shot CB owned by the caller (same idiom as the
//     particle pre-frame compute). Exact upload sequencing (barriers +
//     layout transitions) lives in record_upload().
//   - Push constant: HudPush { viewport_px[2] } = 8 bytes (kept for the
//     viewport-size contract; the vert shader needs no transform).
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

// Canonical HUD frame: the GameUi backbuffer size (bytes = W*H*4).
inline constexpr std::uint32_t kHudFrameW = 640;
inline constexpr std::uint32_t kHudFrameH = 360;
inline constexpr std::uint32_t kHudFrameBytes = kHudFrameW * kHudFrameH * 4;

// 8-byte push constant (viewport size in px; reserved for the shader).
struct HudPush {
  float viewport_px[2]{0.0F, 0.0F};
};
static_assert(sizeof(HudPush) == 8, "hud push is 8 bytes");

class ENGINE_CORE_API HudOverlay {
 public:
  HudOverlay() noexcept = default;
  HudOverlay(const HudOverlay&) = delete;
  auto operator=(const HudOverlay&) -> HudOverlay& = delete;
  ~HudOverlay() noexcept;

  // Build modules + pipeline + HUD texture + sampler + descriptor set
  // against `device` for swapchain-format `color_format`
  // (B8G8R8A8_UNORM or R8G8B8A8_UNORM). Blobs are borrowed (default:
  // the checked-in hud_overlay_*_spv.inc). Null device or bad blobs ->
  // kInvalidArgument; double-build -> kAlreadyInitialized.
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

  // Stage one HUD frame for upload: memcpys `kHudFrameBytes` from `pixels`
  // into the persistent mapped staging buffer (marks dirty). Null pixels
  // -> kInvalidArgument; not built -> kNotInitialized. Cheap: no GPU work
  // here (record_upload does the copy on the caller's one-shot CB).
  auto update(const std::uint8_t* pixels) noexcept -> Status;

  // Record buffer->image upload + SHADER_READ transition on the caller's
  // one-shot `cmd` (fence-waited submit BEFORE the draw CB opens, same
  // idiom as the P5-05 particle pre-frame compute). No-op (Ok) when no
  // update() is pending. kNotInitialized when create() has not succeeded.
  auto record_upload(VkCommandBuffer cmd) noexcept -> Status;

  // Open a color-only dynamic-rendering pass (LOAD composite OVER the
  // already-drawn frame, STORE): the HUD overlay slot after the mesh /
  // particle passes. No depth attachment (overlay, depth test off).
  auto continue_hud_pass(VkCommandBuffer cmd, VkImageView color_view,
                         VkExtent2D extent) noexcept -> Status;
  // Close the pass opened by continue_hud_pass.
  auto end_hud_pass(VkCommandBuffer cmd) noexcept -> Status;

  // Record ONE fullscreen-triangle draw (exactly one vkCmdDraw, 3 verts,
  // no vertex buffers). kNotInitialized when create() has not succeeded;
  // kInvalidArgument on null cmd.
  auto record_draw(VkCommandBuffer cmd, float vp_w, float vp_h) noexcept
      -> Status;

 private:
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  [[maybe_unused]] VkDevice device_{nullptr};
  [[maybe_unused]] void* proc_bind_pipeline_{nullptr};
  [[maybe_unused]] void* proc_cmd_viewport_{nullptr};
  [[maybe_unused]] void* proc_cmd_scissor_{nullptr};
  [[maybe_unused]] void* proc_cmd_draw_{nullptr};
  [[maybe_unused]] void* proc_cmd_push_{nullptr};
  [[maybe_unused]] void* proc_begin_rendering_{nullptr};
  [[maybe_unused]] void* proc_end_rendering_{nullptr};
  [[maybe_unused]] void* proc_create_module_{nullptr};
  [[maybe_unused]] void* proc_destroy_module_{nullptr};
  [[maybe_unused]] void* proc_create_pipelines_{nullptr};
  [[maybe_unused]] void* proc_destroy_pipeline_{nullptr};
  [[maybe_unused]] void* proc_create_layout_{nullptr};
  [[maybe_unused]] void* proc_destroy_layout_{nullptr};
  [[maybe_unused]] void* proc_create_ds_layout_{nullptr};
  [[maybe_unused]] void* proc_destroy_ds_layout_{nullptr};
  [[maybe_unused]] void* proc_create_pool_{nullptr};
  [[maybe_unused]] void* proc_destroy_pool_{nullptr};
  [[maybe_unused]] void* proc_alloc_ds_{nullptr};
  [[maybe_unused]] void* proc_update_ds_{nullptr};
  [[maybe_unused]] void* proc_bind_ds_{nullptr};
  [[maybe_unused]] void* proc_create_sampler_{nullptr};
  [[maybe_unused]] void* proc_destroy_sampler_{nullptr};
  [[maybe_unused]] void* proc_create_view_{nullptr};
  [[maybe_unused]] void* proc_destroy_view_{nullptr};
  [[maybe_unused]] void* proc_cmd_copy_b2i_{nullptr};
  [[maybe_unused]] void* proc_cmd_barrier_{nullptr};
  VkShaderModule modules_[2]{nullptr, nullptr};
  VkPipeline pipeline_{nullptr};
  VkPipelineLayout pipe_layout_{nullptr};
  void* set_layout_{nullptr};
  void* pool_{nullptr};
  void* set_{nullptr};
  void* sampler_{nullptr};
  // HUD texture: VMA image + view + staging buffer (opaque bits; the
  // header stays vk_mem_alloc.h-free, same rule as GpuMesh).
  std::uint64_t image_bits_{0};
  void* image_alloc_{nullptr};
  void* image_view_{nullptr};
  std::uint64_t staging_bits_{0};
  void* staging_alloc_{nullptr};
  void* staging_mapped_{nullptr};
  bool image_live_{false};
#else
  VkShaderModule modules_[2]{nullptr, nullptr};
  VkPipeline pipeline_{nullptr};
  VkPipelineLayout pipe_layout_{nullptr};
  [[maybe_unused]] VkDevice device_{nullptr};
#endif
  std::uint32_t module_count_{0};
  bool ready_{false};
  bool dirty_{false};
  bool image_shader_read_{false};
};

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif

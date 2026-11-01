// pick_buffer.hpp — P3-06: offscreen object-id target + one-texel readback.
//
// PickIdBuffer owns a transient command pool, an R32G32_UINT id image + view
// sized to the pick extent, a D32_SFLOAT depth image + view (depth test
// hides back faces so the topmost entity wins each pixel), and a
// host-visible staging buffer for the one-texel readback in pick().
// The buffer is window- and swapchain-free: the same path works offscreen
// in CTest (SDL_VIDEO_DRIVER=offscreen) and in the editor viewport.
//
// Frame flow (owned command buffer, one-shot submit + fence wait):
//   begin_pick() -> TRANSFER_SRC barrier -> end pass -> pick(x, y)
// reads one R32G32_UINT texel (2x u32 = flecs id lo/hi; 0 = background).
// Compiles SDK-free (handles degrade to void*): create() reports
// kVulkanLoaderFailed without the SDK.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/core/engine_core_export.hpp"
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
#include "engine/renderer/engine_status.hpp"
#include "engine/renderer/vk_extent_stub.hpp"

namespace engine::renderer {

class VulkanDevice;
class ForwardRenderer;
class MeshRegistry;

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
using VkFence = void*;
using VkFormat = std::uint32_t;
using VkImage = void*;
using VkImageView = void*;
#endif

namespace engine::renderer {

class ENGINE_CORE_API PickIdBuffer {
 public:
  PickIdBuffer() noexcept = default;
  PickIdBuffer(const PickIdBuffer&) = delete;
  auto operator=(const PickIdBuffer&) -> PickIdBuffer& = delete;
  ~PickIdBuffer() noexcept;

  // Create the R32G32_UINT id image + view, D32_SFLOAT depth image + view,
  // command pool, staging buffer (one RGBA-uint texel), and the pick
  // pipeline on `forward` (idempotent: double-create fails closed).
  // Zero extent -> kInvalidArgument; double-create -> kAlreadyInitialized.
  auto create(VulkanDevice& dev, ForwardRenderer& forward,
              VkExtent2D extent) noexcept -> Status;
  auto destroy() noexcept -> void;

  [[nodiscard]] auto ready() const noexcept -> bool { return ready_; }
  [[nodiscard]] auto extent_width() const noexcept -> std::uint32_t {
    return width_;
  }
  [[nodiscard]] auto extent_height() const noexcept -> std::uint32_t {
    return height_;
  }
  [[nodiscard]] auto id_view() const noexcept -> VkImageView { return id_view_; }
  [[nodiscard]] auto depth_view() const noexcept -> VkImageView {
    return depth_view_;
  }

  // Record the id pass into an OPEN command buffer `cmd`: barrier the id
  // image UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL (+ depth UNDEFINED ->
  // ATTACHMENT on first use), begin_pick_pass, render_ids, end_pick_pass,
  // barrier the id image to TRANSFER_SRC_OPTIMAL. The caller submits `cmd`
  // and waits before pick(). `forward` must be the same renderer create()
  // built the pick pipeline on.
  auto record_id_pass(VulkanDevice& dev, ForwardRenderer& forward,
                      void* world, const MeshRegistry& registry,
                      VkCommandBuffer cmd, float aspect) noexcept -> Status;

  // Read back one R32G32_UINT texel at (x, y) into `out_lo`/`out_hi`
  // (flecs id lanes; 0/0 = background). OOB reads return 0/0 with
  // MakeOk (same rule as ObjectIdBuffer.Pick in managed). The id image
  // must be in TRANSFER_SRC_OPTIMAL (record_id_pass leaves it there).
  // Records its own one-shot copy + fence wait on an owned command
  // buffer; never touches the caller's `cmd`.
  auto pick(VulkanDevice& dev, std::uint32_t x, std::uint32_t y,
            std::uint32_t& out_lo, std::uint32_t& out_hi) noexcept -> Status;

 private:
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  friend class ForwardRenderer;
  [[maybe_unused]] VkDevice device_{nullptr};
  [[maybe_unused]] void* vma_alloc_{nullptr};
  [[maybe_unused]] void* vma_id_alloc_{nullptr};
  [[maybe_unused]] void* vma_depth_alloc_{nullptr};
  [[maybe_unused]] void* vma_stage_alloc_{nullptr};
  [[maybe_unused]] std::uint64_t id_image_bits_{0};
  [[maybe_unused]] std::uint64_t depth_image_bits_{0};
  [[maybe_unused]] std::uint64_t stage_bits_{0};
  [[maybe_unused]] VkImageView id_view_{nullptr};
  [[maybe_unused]] VkImageView depth_view_{nullptr};
  [[maybe_unused]] void* proc_destroy_view_{nullptr};
  [[maybe_unused]] void* stage_mapped_{nullptr};
  [[maybe_unused]] void* proc_record_barrier_{nullptr};
#else
  friend class ForwardRenderer;
  [[maybe_unused]] std::uint64_t id_image_bits_{0};
  [[maybe_unused]] std::uint64_t depth_image_bits_{0};
  [[maybe_unused]] std::uint64_t stage_bits_{0};
  [[maybe_unused]] VkImageView id_view_{nullptr};
  [[maybe_unused]] VkImageView depth_view_{nullptr};
#endif
  std::uint32_t width_{0};
  std::uint32_t height_{0};
  bool depth_live_{false};
  bool ready_{false};
};

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif

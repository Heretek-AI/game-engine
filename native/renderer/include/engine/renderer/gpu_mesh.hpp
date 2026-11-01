// gpu_mesh.hpp — P1-07: VMA-uploaded mesh buffers + D32_SFLOAT depth.
//
// GpuMesh uploads interleaved 32-byte vertices (pos[3], normal[3],
// uv[2]) and uint32_t indices through VMA (host-visible staging on
// this slice; device-local staging arrives when the transfer queue
// exists). DepthBuffer owns a D32_SFLOAT image + view sized to the
// render extent. Both compile SDK-free (handles degrade to void*):
// create() reports kVulkanLoaderFailed without the SDK.
//
// ForwardRenderer::create() accepts VK_FORMAT_D32_SFLOAT from this
// slice on (P1-06 fail-closed gate removed) and record_mesh() draws
// one indexed mesh with a 160-byte push constant:
//
//   { mat4 mvp; mat4 model; vec4 light_dir; vec4 base_color; }
//
// (within the 256-byte minimum; query maxPushConstantsSize at record
// time and fail closed when the device offers less).
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
using VkCommandBuffer = void*;
using VkDevice = void*;
using VkFormat = std::uint32_t;
using VkImage = void*;
using VkImageView = void*;
#endif

namespace engine::renderer {

// One interleaved vertex: 3+3+2 floats = 32 bytes.
struct MeshVertex {
  float pos[3]{};
  float normal[3]{};
  float uv[2]{};
};
static_assert(sizeof(MeshVertex) == 32, "mesh vertex is 32 bytes");

// 160-byte push constant shared by mesh.vert/mesh.frag.
struct MeshPush {
  float mvp[16]{};
  float model[16]{};
  float light_dir[4]{};
  float base_color[4]{};
};
static_assert(sizeof(MeshPush) == 160, "mesh push is 160 bytes");

class ENGINE_CORE_API GpuMesh {
 public:
  GpuMesh() noexcept = default;
  GpuMesh(const GpuMesh&) = delete;
  auto operator=(const GpuMesh&) -> GpuMesh& = delete;
  ~GpuMesh() noexcept;

  // Upload `vertex_count` vertices + `index_count` u32 indices through
  // VMA. Null device/vertex/index data or zero counts ->
  // kInvalidArgument; double-create -> kAlreadyInitialized.
  auto create(VulkanDevice& dev, const MeshVertex* vertices,
              std::uint32_t vertex_count, const std::uint32_t* indices,
              std::uint32_t index_count) noexcept -> Status;
  // Raw-bytes upload for non-mesh vertex layouts (P6-06 sprite quads:
  // SpriteVertex is also 32 bytes but a different field split). Same
  // guards; vertex_stride must be > 0.
  auto create_raw(VulkanDevice& dev, const void* vertices,
                  std::size_t vertex_stride, std::uint32_t vertex_count,
                  const std::uint32_t* indices,
                  std::uint32_t index_count) noexcept -> Status;
  auto destroy() noexcept -> void;

  [[nodiscard]] auto ready() const noexcept -> bool { return ready_; }
  [[nodiscard]] auto vertex_count() const noexcept -> std::uint32_t {
    return vertex_count_;
  }
  [[nodiscard]] auto index_count() const noexcept -> std::uint32_t {
    return index_count_;
  }
  // Raw handles for ForwardRenderer::record_mesh (valid after create()).
  [[nodiscard]] auto vertex_buffer_bits() const noexcept -> std::uint64_t {
    return vert_bits_;
  }
  [[nodiscard]] auto index_buffer_bits() const noexcept -> std::uint64_t {
    return idx_bits_;
  }

 private:
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  friend class ForwardRenderer;
  friend class SpriteRenderer;
  [[maybe_unused]] VkDevice device_{nullptr};
  [[maybe_unused]] void* vma_alloc_{nullptr};
  [[maybe_unused]] void* vma_vert_alloc_{nullptr};
  [[maybe_unused]] void* vma_idx_alloc_{nullptr};
  [[maybe_unused]] std::uint64_t vert_bits_{0};
  [[maybe_unused]] std::uint64_t idx_bits_{0};
#else
  friend class ForwardRenderer;
  [[maybe_unused]] std::uint64_t vert_bits_{0};
  [[maybe_unused]] std::uint64_t idx_bits_{0};
#endif
  std::uint32_t vertex_count_{0};
  std::uint32_t index_count_{0};
  bool ready_{false};
};

class ENGINE_CORE_API DepthBuffer {
 public:
  DepthBuffer() noexcept = default;
  DepthBuffer(const DepthBuffer&) = delete;
  auto operator=(const DepthBuffer&) -> DepthBuffer& = delete;
  ~DepthBuffer() noexcept;

  // Create a D32_SFLOAT image + view for `extent`. Only D32_SFLOAT is
  // legal today; other formats fail closed. Zero extent ->
  // kInvalidArgument; double-create -> kAlreadyInitialized.
  auto create(VulkanDevice& dev, VkExtent2D extent,
              VkFormat format) noexcept -> Status;
  auto destroy() noexcept -> void;

  [[nodiscard]] auto ready() const noexcept -> bool { return ready_; }
  [[nodiscard]] auto extent_width() const noexcept -> std::uint32_t {
    return width_;
  }
  [[nodiscard]] auto extent_height() const noexcept -> std::uint32_t {
    return height_;
  }
  // Raw handles for ForwardRenderer::begin/continue_mesh_pass.
  [[nodiscard]] auto image_bits() const noexcept -> std::uint64_t {
    return image_bits_;
  }
  [[nodiscard]] auto view() const noexcept -> VkImageView { return view_; }

 private:
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  friend class ForwardRenderer;
  [[maybe_unused]] VkDevice device_{nullptr};
  [[maybe_unused]] void* vma_alloc_{nullptr};
  [[maybe_unused]] void* vma_img_alloc_{nullptr};
  [[maybe_unused]] std::uint64_t image_bits_{0};
  [[maybe_unused]] VkImageView view_{nullptr};
  [[maybe_unused]] void* proc_destroy_view_{nullptr};
#else
  friend class ForwardRenderer;
  [[maybe_unused]] std::uint64_t image_bits_{0};
  [[maybe_unused]] VkImageView view_{nullptr};
#endif
  std::uint32_t width_{0};
  std::uint32_t height_{0};
  bool ready_{false};
};

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif

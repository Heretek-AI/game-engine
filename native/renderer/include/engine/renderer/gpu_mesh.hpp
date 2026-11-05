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
#include <vector>

#include "engine/core/engine_core_export.hpp"
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
#include "engine/renderer/engine_status.hpp"
#include "engine/renderer/sdf_brick_builder.hpp"
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
// light_dir.w is reserved for the P4-03 shadow toggle (DirectionalLight
// .shadow lane; no-op until the forward CSM atlas wire lands, t_07cf149e).
struct MeshPush {
  float mvp[16]{};
  float model[16]{};
  float light_dir[4]{};
  float base_color[4]{};
};
static_assert(sizeof(MeshPush) == 160, "mesh push is 160 bytes");

// P5-02: per-vertex skin payload (binding 1, locations 3/4). Kept OUT of
// MeshVertex so the 32-byte legacy layout, its static_asserts, and every
// existing pipeline/consumer stay byte-identical. joints[] are palette
// indices, weights[] the 4 blend weights (normalized on the GPU; a zero
// total falls back to the bind pose).
struct SkinnedVertexExtra {
  std::uint32_t joints[4]{};
  float weights[4]{};
};
static_assert(sizeof(SkinnedVertexExtra) == 32, "skin extra is 32 bytes");
static_assert(offsetof(SkinnedVertexExtra, weights) == 16, "weights offset");

// P5-02: mesh.vert specialization constant 0 (SKIN_JOINT_COUNT) + hard cap.
// Joint matrices ride a UBO (set 0, binding 0): 64 x 64B = 4KiB, inside
// the guaranteed 64KiB maxUniformBufferRange. kSkinMaxJoints bounds the
// palette; the baked joint_count (1..kSkinMaxJoints via create_mesh_skinned)
// tells the shader how many leading entries are live.
inline constexpr std::uint32_t kSkinMaxJoints = 64;
inline constexpr std::uint32_t kSkinSpecJointCount = 0;
inline constexpr std::uint32_t kSkinSetJointUbo = 0;
inline constexpr std::uint32_t kSkinBindJointUbo = 0;

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
  // guards; vertex_stride must be > 0. The stride is retained so
  // record_mesh_skinned can validate the skin buffer shape.
  auto create_raw(VulkanDevice& dev, const void* vertices,
                  std::size_t vertex_stride, std::uint32_t vertex_count,
                  const std::uint32_t* indices,
                  std::uint32_t index_count) noexcept -> Status;
  [[nodiscard]] auto vertex_stride() const noexcept -> std::size_t {
    return vertex_stride_;
  }
  // P5-05-followup: refresh the contents of an ALREADY-CREATED mesh in
  // place (host-visible coherent mapping, same memcpy as create_raw).
  // `vertex_bytes`/`index_bytes` must fit inside the created capacities;
  // `vert_count`/`idx_count` replace the draw counts so one persistent
  // mesh serves a variable-size particle frame without reallocation.
  // Both counts 0 with null data clears the draw (empty, still ready).
  auto write_raw(const void* vertices, std::size_t vertex_bytes,
                 const std::uint32_t* indices, std::size_t index_bytes,
                 std::uint32_t vert_count,
                 std::uint32_t idx_count) noexcept -> Status;
  auto destroy() noexcept -> void;

  // P5-05-followup: created buffer capacities (bytes) so tests can assert
  // write_raw bounds without Vulkan handles.
  [[nodiscard]] auto vert_capacity_bytes() const noexcept -> std::uint64_t {
    return vert_bytes_;
  }
  [[nodiscard]] auto idx_capacity_bytes() const noexcept -> std::uint64_t {
    return idx_bytes_;
  }

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
  // GI bricks baked at upload (P4-07 part 2): SDF shell of the mesh in
  // mesh-local space for the probe baker. Empty when the mesh was made
  // with create_raw (sprite quads) or the bake dropped everything.
  // Borrowed storage (valid while *this lives); count may be 0.
  [[nodiscard]] auto gi_bricks(std::uint32_t* count_out) const noexcept
      -> const SdfBrick*;

 private:
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  friend class ForwardRenderer;
  friend class SpriteRenderer;
  friend class ParticleRenderer;
  [[maybe_unused]] VkDevice device_{nullptr};
  [[maybe_unused]] void* vma_alloc_{nullptr};
  [[maybe_unused]] void* vma_vert_alloc_{nullptr};
  [[maybe_unused]] void* vma_idx_alloc_{nullptr};
  // P5-05-followup: persistent mapped pointers backing write_raw
  // (host-visible + coherent at create time; null after destroy).
  [[maybe_unused]] void* vert_mapped_{nullptr};
  [[maybe_unused]] void* idx_mapped_{nullptr};
  [[maybe_unused]] std::uint64_t vert_bytes_{0};
  [[maybe_unused]] std::uint64_t idx_bytes_{0};
  [[maybe_unused]] std::uint64_t vert_stride_{0};
  [[maybe_unused]] std::uint64_t vert_bits_{0};
  [[maybe_unused]] std::uint64_t idx_bits_{0};
#else
  friend class ForwardRenderer;
  [[maybe_unused]] std::uint64_t vert_bits_{0};
  [[maybe_unused]] std::uint64_t idx_bits_{0};
#endif
  std::uint32_t vertex_count_{0};
  std::uint32_t index_count_{0};
  // Baked SDF shell (P4-07 part 2 probe input). Bricks are baked once at
  // create() from the upload verts; create_raw() leaves this empty.
  // Capped (kGiBrickCap) so a huge mesh can't blow up upload memory;
  // drops are counted on the MeshSdf view (conservative: missing shell
  // leaks light, never blocks it).
  std::vector<SdfBrick> gi_bricks_;
  std::size_t vertex_stride_{0};
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

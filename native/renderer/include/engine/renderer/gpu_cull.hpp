// gpu_cull.hpp — M3-02: two-phase GPU compute culling.
//
// Phase A (frustum): one thread per instance; tests the bounding sphere
// against the 6 frustum planes; writes surviving instance ids into the
// compacted visible list via an atomic counter.
//
// Phase B (Hi-Z occlusion): surviving instances test their sphere against
// the depth-pyramid mip chain (conservative: screen rect + closest depth
// vs max-depth in the touched pyramid texel, 2x2 taps for watertightness);
// survivors append a VkDrawIndexedIndirectCommand into the indirect buffer
// via the SAME atomic counter (single counter, phase B runs after a
// pipeline barrier on phase A's counter write).
//
// Both phases are GLSL compute shaders compiled to SPIR-V at build time
// (shaders/cull.comp, shaders/hiz.comp below); this header owns the CPU
// plan: push constants, buffer sizing, dispatch shape, and the CPU
// reference implementations used by tests (bit-exact contract) and by the
// benchmark fallback when no GPU is present.
//
// vkCmdDrawIndexedIndirectCount path (real Vulkan): the indirect buffer
// holds capacity draw commands, the count buffer holds ONE uint32 (written
// by phase B's atomic). The draw call binds the bindless set + vertex/
// index buffers and issues a single count-draw — ZERO CPU per-draw work.
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
using VkDeviceSize = std::uint64_t;
// Stand-in draw command (matches VkDrawIndexedIndirectCommand layout).
struct VkDrawIndexedIndirectCommand {
  std::uint32_t indexCount;
  std::uint32_t instanceCount;
  std::uint32_t firstIndex;
  std::int32_t vertexOffset;
  std::uint32_t firstInstance;
};
#endif

namespace engine::renderer {

// One draw's static geometry (index range). Mesh LUT entry in the CULL
// shader (push constant): the shader fills indexCount/firstIndex from the
// instance's mesh_index; instanceCount=1, firstInstance=instance id.
struct CullMeshRange {
  std::uint32_t index_count;
  std::uint32_t first_index;
  std::int32_t vertex_offset;
  std::uint32_t pad;
};

// Frustum planes from a row-major view-projection matrix: 6 planes
// (left/right/bottom/top/near/far), normalized (a,b,c,d) with inside =
// dot(n, p) + d >= 0.
struct FrustumPlanes {
  float planes[6][4];
};

// Push constants for the CULL shader: ids + scene/mesh BDA, then the
// atomic counter + frame-plane BDA. Both BDA blocks stay 8B aligned.
// Total: 8 + 5*8 = 48 bytes (well under the 128B Vulkan push limit).
struct CullPush {
  std::uint32_t instance_count;
  std::uint32_t mesh_count;
  std::uint64_t scene_addr;  // BDA: SceneInstance[input]
  std::uint64_t mesh_addr;   // BDA: CullMeshRange[mesh_count]
  std::uint64_t vis_addr;    // BDA: uint32 visible ids (phase A out)
  std::uint64_t count_addr;  // BDA: uint32 atomic counter (in/out)
  std::uint64_t planes_addr;  // BDA: CullFrame (6 normalized planes)
};
static_assert(sizeof(CullPush) == 48, "cull push fits 128B limit");

// Frame block read via BDA by BOTH shaders: 6 frustum planes (phase A)
// + row-major view-projection rows (phase B reprojection). 160 bytes.
struct CullFrame {
  float planes[6][4];  // normalized, inside = dot(n,p)+d >= 0
  float mvp[16];       // row-major view-projection
};
static_assert(sizeof(CullFrame) == 160, "frame block is 160B");

// Push constants for the HIZ shader (72 bytes: 4 u32 + 5 BDA + 4 u32 +
// frame BDA replaced the inline 64B MVP rows — the old layout was 144B,
// OVER the 128B Vulkan push-constant limit and would fail pipeline
// creation on strict drivers).
struct HizPush {
  std::uint32_t candidate_count;  // phase A survivors (counter readback)
  std::uint32_t mesh_count;
  std::uint32_t fb_w;
  std::uint32_t fb_h;
  std::uint64_t scene_addr;
  std::uint64_t vis_addr;   // phase A list (in)
  std::uint64_t draw_addr;  // indirect commands (append)
  std::uint64_t count_addr;   // atomic (cont.)
  std::uint64_t pyramid_addr;  // BDA: Hi-Z mip chain (float max-depth)
  std::uint64_t mesh_addr;     // BDA: CullMeshRange[mesh_count]
  std::uint64_t frame_addr;    // BDA: CullFrame (planes + MVP rows)
  std::uint32_t pyramid_w;
  std::uint32_t pyramid_h;
  std::uint32_t pyramid_mips;
  std::uint32_t pad3;
};
static_assert(sizeof(HizPush) == 88, "hiz push fits 128B limit");

// CPU reference: frustum test of one sphere. Returns true when visible.
auto CullSphereInFrustum(const float sphere[4], const FrustumPlanes& fr)
    -> bool;

// CPU reference: phase A over `count` instances. Writes surviving instance
// ids into `visible_out`, returns survivor count. Bit-exact contract for
// the CULL shader (tests pin both against the same randomized scene).
auto CullFrustumCpu(const void* scene_instances, std::uint32_t count,
                    const FrustumPlanes& fr, std::uint32_t* visible_out)
    -> std::uint32_t;

// CPU reference: screen-rect of a sphere at `mvp` (row-major VP).
// Returns false when behind the near plane (caller culls).
auto CullProjectSphere(const float sphere[4], const float mvp[16],
                       std::uint32_t fb_w, std::uint32_t fb_h,
                       std::uint32_t* x0_out, std::uint32_t* y0_out,
                       std::uint32_t* x1_out, std::uint32_t* y1_out,
                       float* closest_depth_out) -> bool;

// CPU reference: phase B over `candidate_count` ids. Appends one indirect
// command per pyramid-visible instance, returns commands written.
auto CullHizCpu(const void* scene_instances, const std::uint32_t* visible,
                std::uint32_t candidate_count, const float* pyramid,
                std::uint32_t pyr_w, std::uint32_t pyr_h,
                std::uint32_t pyr_mips, const float mvp[16],
                std::uint32_t fb_w, std::uint32_t fb_h,
                const CullMeshRange* meshes, std::uint32_t mesh_count,
                VkDrawIndexedIndirectCommand* draws_out) -> std::uint32_t;

// Dispatch shape: 256 threads/group, covering `count` instances.
[[nodiscard]] inline auto CullGroupCount(std::uint32_t count) noexcept
    -> std::uint32_t {
  return (count + 255u) / 256u;
}

// CPU-side indirect-draw plan: sizing for the indirect + count buffers.
class ENGINE_CORE_API GpuCull {
 public:
  GpuCull() noexcept = default;
  GpuCull(const GpuCull&) = delete;
  auto operator=(const GpuCull&) -> GpuCull& = delete;
  ~GpuCull() noexcept;

  // Configure for `max_instances` + precompiled shader bytes (the .inc
  // blobs below give size+pointer; no file IO on the frame path).
  auto init(std::uint32_t max_instances, const std::uint32_t* cull_spv,
            std::size_t cull_spv_words, const std::uint32_t* hiz_spv,
            std::size_t hiz_spv_words) noexcept -> Status;
  auto shutdown() noexcept -> void;
  [[nodiscard]] auto ready() const noexcept -> bool { return ready_; }
  [[nodiscard]] auto max_instances() const noexcept -> std::uint32_t {
    return max_instances_;
  }

  // Indirect buffer sizing: capacity draw commands + 4B count + N u32 ids.
  [[nodiscard]] auto indirect_bytes() const noexcept -> std::uint64_t {
    return static_cast<std::uint64_t>(max_instances_) *
           sizeof(VkDrawIndexedIndirectCommand);
  }
  [[nodiscard]] auto visible_bytes() const noexcept -> std::uint64_t {
    return static_cast<std::uint64_t>(max_instances_) * sizeof(std::uint32_t);
  }
  static constexpr std::uint64_t kCountBytes = sizeof(std::uint32_t);

  // GPU path (real Vulkan only): create compute pipelines + allocate
  // indirect/count/visible buffers via VMA. Stub: kVulkanLoaderFailed.
  auto create_gpu(VkDevice device, VkPipelineLayout bindless_layout) noexcept
      -> Status;
  auto destroy_gpu() noexcept -> void;
  [[nodiscard]] auto gpu_ready() const noexcept -> bool {
    return cull_pipeline_ != nullptr;
  }

 private:
  std::uint32_t max_instances_{0};
  const std::uint32_t* cull_spv_{nullptr};
  std::size_t cull_words_{0};
  const std::uint32_t* hiz_spv_{nullptr};
  std::size_t hiz_words_{0};
  bool ready_{false};
  VkPipeline cull_pipeline_{nullptr};
  VkPipeline hiz_pipeline_{nullptr};
  VkBuffer indirect_buffer_{nullptr};
  VkBuffer count_buffer_{nullptr};
  VkBuffer visible_buffer_{nullptr};
};

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif

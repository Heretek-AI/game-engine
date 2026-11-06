// gpu_mesh.cpp — P1-07: VMA-uploaded mesh buffers + D32_SFLOAT depth.
//
// GpuMesh::create validates args, uploads vertices/indices through
// EngineVmaCreateMeshBuffer (host-visible staging, memcpy into the
// mapping), and records the opaque handles. DepthBuffer::create
// validates the D32_SFLOAT-only contract, allocates the image through
// EngineVmaCreateDepthImage, and builds the VkImageView (aspect DEPTH).
// Both destroy() paths tolerate never-created and post-shutdown (null
// allocator) states.
#include "engine/renderer/gpu_mesh.hpp"

#include <cstring>
#include <span>
#include <vector>

#include "engine/renderer/sdf_brick_builder.hpp"

#if defined(ENGINE_RENDERER_HAS_VULKAN)
// volk-first include order (same rule as vulkan_device.cpp).
#include <volk.h>
#endif

#include "engine/renderer/vulkan_device.hpp"
#include "vma_bridge.hpp"

namespace engine::renderer {
namespace {

// GI brick budget per mesh (P4-07 part 2): 256 bricks x ~528 B ~= 135 KB
// worst case; test scenes bake a handful. Over budget -> the builder
// reports dropped_bricks and the baker treats missing shell as open
// (light leaks, never blocks).
inline constexpr std::uint32_t kGiBrickCap = 256;
inline constexpr float kGiBrickTexel = 0.1f;
// Upload-time bake bound: the builder's per-candidate work is triangle
// dependent, and mesh upload runs with GI disabled as often as enabled
// (the bricks are mesh-local and must exist whenever GI turns on, so the
// bake is unconditional by design — see below). Cap the input so one huge
// mesh cannot stall upload; over budget the mesh draws with no bricks and
// GI skips it, like any failed best-effort bake.
inline constexpr std::uint32_t kGiBakeTriangleCap = 1u << 20;

// Pack mesh-local xyz + bake the SDF shell. Best-effort: any failure
// leaves `out` empty (mesh still draws; GI skips it).
auto BakeMeshBricks(std::span<const MeshVertex> vertices,
                    std::span<const std::uint32_t> indices,
                    std::vector<SdfBrick>& out) noexcept -> void {
  out.clear();
  // Triangle cap FIRST: over budget the mesh keeps zero bricks (draws,
  // GI skips it). The check must precede the out.resize/pack below —
  // returning after resizing would report 256 zero bricks, which
  // gather_brick_rows would feed to the baker as phantom solid voxels
  // at the mesh origin (texel_size 0 collapses 131k texels onto one
  // point and stretches the scene AABB there).
  if (indices.size() / 3u > kGiBakeTriangleCap) {
    return;
  }
  std::vector<float> packed;
  packed.resize(vertices.size() * 3);
  for (std::size_t i = 0; i < vertices.size(); ++i) {
    packed[i * 3] = vertices[i].pos[0];
    packed[i * 3 + 1] = vertices[i].pos[1];
    packed[i * 3 + 2] = vertices[i].pos[2];
  }
  out.resize(kGiBrickCap);
  MeshSdf sdf{};
  sdf.bricks = out.data();
  sdf.capacity = kGiBrickCap;
  MeshSdfBuilder builder;
  SdfBuildParams params{};
  params.texel_size = kGiBrickTexel;
  params.brick_capacity = kGiBrickCap;
  const auto vc = static_cast<std::uint32_t>(vertices.size());
  const auto ic = static_cast<std::uint32_t>(indices.size());
  if (!builder.build(packed.data(), vc, indices.data(), ic, params, &sdf)
           .ok()) {
    out.clear();
    return;
  }
  out.resize(sdf.brick_count);
}

}  // namespace

GpuMesh::~GpuMesh() noexcept { destroy(); }
DepthBuffer::~DepthBuffer() noexcept { destroy(); }

#if defined(ENGINE_RENDERER_HAS_VULKAN)

auto GpuMesh::create(VulkanDevice& dev, const MeshVertex* vertices,
                     std::uint32_t vertex_count, const std::uint32_t* indices,
                     std::uint32_t index_count) noexcept -> Status {
  if (ready_) {
    return MakeFail(StatusCode::kAlreadyInitialized, "mesh built");
  }
  if (vertices == nullptr || indices == nullptr || vertex_count == 0 ||
      index_count == 0) {
    return MakeFail(StatusCode::kInvalidArgument, "mesh bad args");
  }
  if (const Status up = create_raw(dev, vertices, sizeof(MeshVertex),
                                        vertex_count, indices, index_count);
      !up.ok()) {
    return up;
  }
// GI bricks (P4-07 part 2 probe input): bake once from the upload verts
  // (mesh-local). Best-effort: a failed bake leaves zero bricks and the
  // mesh still draws (GI simply sees no occluder there). The bake is
  // unconditional by design, not gated on gi_enabled_: bricks describe the
  // mesh, and GI can be enabled after upload — gating would leave
  // late-enabled GI blind with no source geometry retained for a rebuild.
  // Upload-time cost is bounded by kGiBakeTriangleCap instead.
  BakeMeshBricks(std::span(vertices, vertex_count),
                 std::span(indices, index_count), gi_bricks_);
  return MakeOk();
}

auto GpuMesh::create_raw(VulkanDevice& dev, const void* vertices,
                         std::size_t vertex_stride,
                         std::uint32_t vertex_count,
                         const std::uint32_t* indices,
                         std::uint32_t index_count) noexcept -> Status {
  if (ready_) {
    return MakeFail(StatusCode::kAlreadyInitialized, "mesh built");
  }
  if (vertices == nullptr || indices == nullptr || vertex_stride == 0 ||
      vertex_count == 0 || index_count == 0) {
    return MakeFail(StatusCode::kInvalidArgument, "mesh bad args");
  }
  VkDevice device = dev.device();
  if (device == nullptr || !dev.vma_ready()) {
    return MakeFail(StatusCode::kInvalidArgument, "mesh null device");
  }
  auto* alloc = vma_bridge::g_alloc;
  if (alloc == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "mesh no vma");
  }
  vma_bridge::MeshBufferBundle vb{};
  if (vma_bridge::EngineVmaCreateMeshBuffer(
          alloc, static_cast<std::uint64_t>(vertex_count) * vertex_stride,
          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vb) != 0 ||
      vb.mapped == nullptr) {
    return MakeFail(StatusCode::kDeviceCreateFailed, "mesh vertex alloc");
  }
  vma_bridge::MeshBufferBundle ib{};
  if (vma_bridge::EngineVmaCreateMeshBuffer(
          alloc,
          static_cast<std::uint64_t>(index_count) * sizeof(std::uint32_t),
          VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &ib) != 0 ||
      ib.mapped == nullptr) {
    vma_bridge::EngineVmaDestroyBuffer(alloc, vb.buffer_bits, vb.alloc);
    return MakeFail(StatusCode::kDeviceCreateFailed, "mesh index alloc");
  }
  std::memcpy(vb.mapped, vertices,
              static_cast<std::size_t>(vertex_count) * vertex_stride);
  std::memcpy(ib.mapped, indices,
              static_cast<std::size_t>(index_count) * sizeof(std::uint32_t));
  device_ = device;
  vma_alloc_ = alloc;
  vert_mapped_ = vb.mapped;
  idx_mapped_ = ib.mapped;
  vert_bytes_ = static_cast<std::uint64_t>(vertex_count) * vertex_stride;
  idx_bytes_ = static_cast<std::uint64_t>(index_count) * sizeof(std::uint32_t);
  vert_stride_ = static_cast<std::uint64_t>(vertex_stride);
  vert_bits_ = vb.buffer_bits;
  vma_vert_alloc_ = vb.alloc;
  idx_bits_ = ib.buffer_bits;
  vma_idx_alloc_ = ib.alloc;
  vertex_count_ = vertex_count;
  index_count_ = index_count;
  vertex_stride_ = vertex_stride;
  ready_ = true;
  return MakeOk();
}

auto GpuMesh::write_raw(const void* vertices, std::size_t vertex_bytes,
                        const std::uint32_t* indices, std::size_t index_bytes,
                        std::uint32_t vert_count,
                        std::uint32_t idx_count) noexcept -> Status {
  if (!ready_) {
    return MakeFail(StatusCode::kNotInitialized, "mesh not built");
  }
  if (vert_count == 0 || idx_count == 0 || vert_mapped_ == nullptr ||
      idx_mapped_ == nullptr) {
    // Empty draw: no data needed, counts zeroed (mesh stays ready).
    // Null mappings can only happen post-destroy (ready_ false above),
    // but fail closed here rather than memcpy into null.
    vertex_count_ = 0;
    index_count_ = 0;
    return (vert_count == 0 && idx_count == 0)
               ? MakeOk()
               : MakeFail(StatusCode::kInvalidArgument, "mesh write no mapping");
  }
  if (vertex_bytes > vert_bytes_ || index_bytes > idx_bytes_) {
    return MakeFail(StatusCode::kInvalidArgument, "mesh write over capacity");
  }
  // Bound the draw counts the same way: record_draw reads up to
  // index_count() from the index buffer, so a count beyond capacity
  // would read past it (or read stale indices).
  if (static_cast<std::uint64_t>(idx_count) * sizeof(std::uint32_t) >
      idx_bytes_) {
    return MakeFail(StatusCode::kInvalidArgument, "mesh write count over capacity");
  }
  if (vert_stride_ != 0 &&
      static_cast<std::uint64_t>(vert_count) * vert_stride_ > vert_bytes_) {
    return MakeFail(StatusCode::kInvalidArgument, "mesh write count over capacity");
  }
  if ((vertices == nullptr && vertex_bytes != 0) ||
      (indices == nullptr && index_bytes != 0)) {
    return MakeFail(StatusCode::kInvalidArgument, "mesh write null data");
  }
  if (vertices != nullptr && vertex_bytes != 0) {
    std::memcpy(vert_mapped_, vertices, vertex_bytes);
  }
  if (indices != nullptr && index_bytes != 0) {
    std::memcpy(idx_mapped_, indices, index_bytes);
  }
  vertex_count_ = vert_count;
  index_count_ = idx_count;
  return MakeOk();
}

auto GpuMesh::gi_bricks(std::uint32_t* count_out) const noexcept
    -> const SdfBrick* {
  if (count_out != nullptr) {
    const std::size_t n = gi_bricks_.size();
    *count_out =
        static_cast<std::uint32_t>(n > 0xFFFFFFFFu ? 0xFFFFFFFFu : n);
  }
  return gi_bricks_.empty() ? nullptr : gi_bricks_.data();
}

auto GpuMesh::destroy() noexcept -> void {
  auto* alloc = static_cast<vma_bridge::Allocator_T*>(vma_alloc_);
  if (alloc != nullptr) {
    if (vert_bits_ != 0) {
      vma_bridge::EngineVmaDestroyBuffer(
          alloc, vert_bits_,
          static_cast<vma_bridge::Allocation_T*>(vma_vert_alloc_));
    }
    if (idx_bits_ != 0) {
      vma_bridge::EngineVmaDestroyBuffer(
          alloc, idx_bits_,
          static_cast<vma_bridge::Allocation_T*>(vma_idx_alloc_));
    }
  }
  device_ = nullptr;
  vma_alloc_ = nullptr;
  vma_vert_alloc_ = nullptr;
  vma_idx_alloc_ = nullptr;
  vert_mapped_ = nullptr;
  idx_mapped_ = nullptr;
  vert_bytes_ = 0;
  idx_bytes_ = 0;
  vert_stride_ = 0;
  vert_bits_ = 0;
  idx_bits_ = 0;
  vertex_count_ = 0;
  index_count_ = 0;
  gi_bricks_.clear();
  ready_ = false;
}

auto DepthBuffer::create(VulkanDevice& dev, VkExtent2D extent,
                         VkFormat format) noexcept -> Status {
  if (ready_) {
    return MakeFail(StatusCode::kAlreadyInitialized, "depth built");
  }
  if (extent.width == 0 || extent.height == 0) {
    return MakeFail(StatusCode::kInvalidArgument, "depth bad extent");
  }
  if (format != VK_FORMAT_D32_SFLOAT) {
    return MakeFail(StatusCode::kInvalidArgument, "depth bad format");
  }
  VkDevice device = dev.device();
  if (device == nullptr || !dev.vma_ready()) {
    return MakeFail(StatusCode::kInvalidArgument, "depth null device");
  }
  auto* alloc = vma_bridge::g_alloc;
  if (alloc == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "depth no vma");
  }
  auto p_vkCreateImageView = reinterpret_cast<PFN_vkCreateImageView>(
      dev.instance_proc("vkCreateImageView"));
  auto p_vkDestroyImageView = reinterpret_cast<PFN_vkDestroyImageView>(
      dev.instance_proc("vkDestroyImageView"));
  if (p_vkCreateImageView == nullptr || p_vkDestroyImageView == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "depth no entry");
  }
  vma_bridge::DepthImageBundle db{};
  if (vma_bridge::EngineVmaCreateDepthImage(alloc, extent.width, extent.height,
                                            &db) != 0) {
    return MakeFail(StatusCode::kDeviceCreateFailed, "depth image alloc");
  }
  VkImageView view = nullptr;
  {
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = reinterpret_cast<VkImage>(
        static_cast<std::uintptr_t>(db.image_bits));
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_D32_SFLOAT;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vi.subresourceRange.baseMipLevel = 0;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.baseArrayLayer = 0;
    vi.subresourceRange.layerCount = 1;
    if (p_vkCreateImageView(device, &vi, nullptr, &view) != VK_SUCCESS) {
      vma_bridge::EngineVmaDestroyImage(alloc, db.image_bits, db.alloc);
      return MakeFail(StatusCode::kDeviceCreateFailed, "depth view");
    }
  }
  device_ = device;
  vma_alloc_ = alloc;
  vma_img_alloc_ = db.alloc;
  image_bits_ = db.image_bits;
  view_ = view;
  proc_destroy_view_ = reinterpret_cast<void*>(p_vkDestroyImageView);
  width_ = extent.width;
  height_ = extent.height;
  ready_ = true;
  return MakeOk();
}

auto DepthBuffer::destroy() noexcept -> void {
  (void)proc_destroy_view_;
  auto* alloc = static_cast<vma_bridge::Allocator_T*>(vma_alloc_);
  // View destroy needs a live device; the image free is allocator-side
  // and safe whenever the allocator pointer is set.
  if (device_ != nullptr && view_ != nullptr &&
      proc_destroy_view_ != nullptr) {
    auto p_vkDestroyImageView =
        reinterpret_cast<PFN_vkDestroyImageView>(proc_destroy_view_);
    p_vkDestroyImageView(device_, view_, nullptr);
  }
  if (alloc != nullptr && image_bits_ != 0) {
    vma_bridge::EngineVmaDestroyImage(
        alloc, image_bits_,
        static_cast<vma_bridge::Allocation_T*>(vma_img_alloc_));
  }
  device_ = nullptr;
  vma_alloc_ = nullptr;
  vma_img_alloc_ = nullptr;
  image_bits_ = 0;
  view_ = nullptr;
  proc_destroy_view_ = nullptr;
  width_ = 0;
  height_ = 0;
  ready_ = false;
}

#else

auto GpuMesh::create(VulkanDevice& /*dev*/, const MeshVertex* /*vertices*/,
                     std::uint32_t /*vertex_count*/,
                     const std::uint32_t* /*indices*/,
                     std::uint32_t /*index_count*/) noexcept -> Status {
  return MakeFail(StatusCode::kVulkanLoaderFailed, "no Vulkan SDK");
}

auto GpuMesh::create_raw(VulkanDevice& /*dev*/, const void* /*vertices*/,
                         std::size_t /*vertex_stride*/,
                         std::uint32_t /*vertex_count*/,
                         const std::uint32_t* /*indices*/,
                         std::uint32_t /*index_count*/) noexcept -> Status {
  return MakeFail(StatusCode::kVulkanLoaderFailed, "no Vulkan SDK");
}

auto GpuMesh::write_raw(const void* /*vertices*/, std::size_t /*vertex_bytes*/,
                        const std::uint32_t* /*indices*/,
                        std::size_t /*index_bytes*/,
                        std::uint32_t /*vert_count*/,
                        std::uint32_t /*idx_count*/) noexcept -> Status {
  return MakeFail(StatusCode::kVulkanLoaderFailed, "no Vulkan SDK");
}

auto GpuMesh::destroy() noexcept -> void {
  vertex_count_ = 0;
  index_count_ = 0;
  vertex_stride_ = 0;
  ready_ = false;
}

auto DepthBuffer::create(VulkanDevice& /*dev*/, VkExtent2D /*extent*/,
                         VkFormat /*format*/) noexcept -> Status {
  return MakeFail(StatusCode::kVulkanLoaderFailed, "no Vulkan SDK");
}

auto DepthBuffer::destroy() noexcept -> void {
  width_ = 0;
  height_ = 0;
  ready_ = false;
}

#endif

}  // namespace engine::renderer

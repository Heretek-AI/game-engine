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

#if defined(ENGINE_RENDERER_HAS_VULKAN)
// volk-first include order (same rule as vulkan_device.cpp).
#include <volk.h>
#endif

#include "engine/renderer/vulkan_device.hpp"
#include "vma_bridge.hpp"

namespace engine::renderer {

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
          alloc, static_cast<std::uint64_t>(vertex_count) * sizeof(MeshVertex),
          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vb) != 0 ||
      vb.mapped == nullptr) {
    return MakeFail(StatusCode::kDeviceCreateFailed, "mesh vertex alloc");
  }
  vma_bridge::MeshBufferBundle ib{};
  if (vma_bridge::EngineVmaCreateMeshBuffer(
          alloc, static_cast<std::uint64_t>(index_count) * sizeof(std::uint32_t),
          VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &ib) != 0 ||
      ib.mapped == nullptr) {
    vma_bridge::EngineVmaDestroyBuffer(alloc, vb.buffer_bits, vb.alloc);
    return MakeFail(StatusCode::kDeviceCreateFailed, "mesh index alloc");
  }
  std::memcpy(vb.mapped, vertices,
              static_cast<std::size_t>(vertex_count) * sizeof(MeshVertex));
  std::memcpy(ib.mapped, indices,
              static_cast<std::size_t>(index_count) * sizeof(std::uint32_t));
  device_ = device;
  vma_alloc_ = alloc;
  vert_bits_ = vb.buffer_bits;
  vma_vert_alloc_ = vb.alloc;
  idx_bits_ = ib.buffer_bits;
  vma_idx_alloc_ = ib.alloc;
  vertex_count_ = vertex_count;
  index_count_ = index_count;
  ready_ = true;
  return MakeOk();
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
  vert_bits_ = 0;
  idx_bits_ = 0;
  vertex_count_ = 0;
  index_count_ = 0;
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
  auto vkCreateImageView = reinterpret_cast<PFN_vkCreateImageView>(
      dev.instance_proc("vkCreateImageView"));
  auto vkDestroyImageView = reinterpret_cast<PFN_vkDestroyImageView>(
      dev.instance_proc("vkDestroyImageView"));
  if (vkCreateImageView == nullptr || vkDestroyImageView == nullptr) {
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
    if (vkCreateImageView(device, &vi, nullptr, &view) != VK_SUCCESS) {
      vma_bridge::EngineVmaDestroyImage(alloc, db.image_bits, db.alloc);
      return MakeFail(StatusCode::kDeviceCreateFailed, "depth view");
    }
  }
  device_ = device;
  vma_alloc_ = alloc;
  vma_img_alloc_ = db.alloc;
  image_bits_ = db.image_bits;
  view_ = view;
  proc_destroy_view_ = reinterpret_cast<void*>(vkDestroyImageView);
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
    auto vkDestroyImageView =
        reinterpret_cast<PFN_vkDestroyImageView>(proc_destroy_view_);
    vkDestroyImageView(device_, view_, nullptr);
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

auto GpuMesh::destroy() noexcept -> void {
  vertex_count_ = 0;
  index_count_ = 0;
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

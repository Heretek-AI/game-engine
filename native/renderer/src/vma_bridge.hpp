// vma_bridge.hpp — M3-01: shared VMA bridge declarations (internal).
//
// Single source of truth for the extern "C" EngineVma* wrappers so
// vulkan_device.cpp and vma_impl.cpp use IDENTICAL types (CodeRabbit:
// separate hand declarations risk field-reorder/type drift that compiles
// cleanly and corrupts the ring bundle at runtime, plus LTO
// -Wlto-type-mismatch on the same C symbol with different signatures).
// Also declares RendererWindowCreateSurface (implemented in
// renderer_window.cpp) so both callers share one prototype.
//
// Hot-flagged TUs include this header only; vk_mem_alloc.h stays confined
// to vma_impl.cpp. The bundle mirrors the VMA upload-ring fields with
// opaque handles — no Vulkan/VMA headers required here.
#pragma once

#include <cstdint>

namespace engine::renderer::vma_bridge {

// Opaque VMA handles (real types only visible inside vma_impl.cpp).
struct Allocator_T;
struct Allocation_T;

// Upload-ring bundle: buffer handle bits + allocation + mapping.
struct BufferBundle {
  std::uint64_t buffer_bits{0};  // VkBuffer bits (opaque here)
  Allocation_T* alloc{nullptr};
  void* mapped{nullptr};
  std::uint64_t device_addr{0};
};
static_assert(sizeof(BufferBundle) == 32, "bridge bundle layout");
static_assert(alignof(BufferBundle) == 8, "bridge bundle align");

// P1-07: host-visible buffer backing (vertex/index) + D32_SFLOAT depth
// image. Mirrors the upload-ring flags (HOST_VISIBLE|HOST_COHERENT,
// prefer DEVICE_LOCAL) so mesh uploads memcpy directly into the
// mapping; the depth image is DEVICE_LOCAL-only (never mapped).
struct MeshBufferBundle {
  std::uint64_t buffer_bits{0};  // VkBuffer bits (opaque here)
  Allocation_T* alloc{nullptr};
  void* mapped{nullptr};
};
struct DepthImageBundle {
  std::uint64_t image_bits{0};  // VkImage bits (opaque here)
  Allocation_T* alloc{nullptr};
};

extern "C" {
auto EngineVmaCreateForDevice(void* physical, void* device, void* instance)
    -> Allocator_T*;
auto EngineVmaCreateUploadRing(Allocator_T* alloc, void* device,
                               std::uint64_t bytes, BufferBundle* out) -> int;
auto EngineVmaDestroyRing(Allocator_T* alloc, std::uint64_t buffer_bits,
                          Allocation_T* handle) -> void;
auto EngineVmaDestroy(Allocator_T* alloc) -> void;
auto EngineVmaCreateMeshBuffer(Allocator_T* alloc, std::uint64_t bytes,
                               std::uint32_t usage_bits,
                               MeshBufferBundle* out) -> int;
auto EngineVmaDestroyBuffer(Allocator_T* alloc, std::uint64_t buffer_bits,
                            Allocation_T* handle) -> void;
auto EngineVmaCreateDepthImage(Allocator_T* alloc, std::uint32_t width,
                               std::uint32_t height,
                               DepthImageBundle* out) -> int;
auto EngineVmaDestroyImage(Allocator_T* alloc, std::uint64_t image_bits,
                           Allocation_T* handle) -> void;
}  // extern "C"

// Process slots: one device per process in M3-01 (M3-02 moves these into
// VulkanDevice once the header can see VmaAllocator without leaking it).
inline Allocator_T* g_alloc = nullptr;
inline BufferBundle g_ring{};

}  // namespace engine::renderer::vma_bridge

namespace engine::renderer {
class RendererWindow;
}  // namespace engine::renderer

// Surface helper implemented in renderer_window.cpp (owns the SDL
// includes). VkInstance/VkSurfaceKHR degrade to void* when the Vulkan
// SDK is absent.
auto RendererWindowCreateSurface(const engine::renderer::RendererWindow& window,
                                 void* instance, void* surface_out) noexcept
    -> bool;

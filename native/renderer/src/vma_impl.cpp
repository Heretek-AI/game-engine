// vma_impl.cpp — VMA single-definition TU + extern "C" wrappers (M3-01).
//
// VulkanMemoryAllocator is header-only: exactly one TU defines
// VMA_IMPLEMENTATION. This TU also implements the EngineVma* wrappers
// consumed by vulkan_device.cpp (which is hot-flagged -fno-exceptions
// and must never include vk_mem_alloc.h). Compiled with BASE flags
// (exceptions/RTTI on): per-file -Wno-* relaxations for VMA's own
// warnings, plus -fno-lto-safe exception tables for VMA internals.
//
// NOTE: volk.h must come FIRST (it defines VK_NO_PROTOTYPES and
// hard-errors if <vulkan/vulkan.h> was already included).
#if defined(ENGINE_RENDERER_HAS_VULKAN)
#include <volk.h>

#include <cstdint>

#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wnullability-completeness"
#pragma GCC diagnostic ignored "-Wnullability-extension"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wtautological-unsigned-zero-compare"
#endif
#include "vk_mem_alloc.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <cstddef>

#include "vma_bridge.hpp"

// vma_impl.cpp MUST keep the SAME layout as vma_bridge::BufferBundle
// (shared internal header): field drift here corrupts the ring at runtime.
static_assert(sizeof(engine::renderer::vma_bridge::BufferBundle) == 32,
              "bridge bundle layout");
static_assert(alignof(engine::renderer::vma_bridge::BufferBundle) == 8,
              "bridge bundle align");

extern "C" {

using engine::renderer::vma_bridge::Allocation_T;
using engine::renderer::vma_bridge::Allocator_T;
using engine::renderer::vma_bridge::BufferBundle;

auto EngineVmaCreateForDevice(void* physical, void* device, void* instance)
    -> Allocator_T* {
  VmaVulkanFunctions fns{};
  fns.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
  fns.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
  VmaAllocatorCreateInfo va{};
  va.physicalDevice = static_cast<VkPhysicalDevice>(physical);
  va.device = static_cast<VkDevice>(device);
  va.instance = static_cast<VkInstance>(instance);
  va.vulkanApiVersion = VK_API_VERSION_1_3;
  // REQUIRED: the upload ring uses VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS;
  // VMA asserts (Debug) without this flag.
  va.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
  va.pVulkanFunctions = &fns;
  VmaAllocator alloc = nullptr;
  if (vmaCreateAllocator(&va, &alloc) != VK_SUCCESS) {
    return nullptr;
  }
  return reinterpret_cast<Allocator_T*>(alloc);
}

auto EngineVmaCreateUploadRing(Allocator_T* alloc_raw, void* device,
                               std::uint64_t bytes, BufferBundle* out)
    -> int {
  VmaAllocator alloc = reinterpret_cast<VmaAllocator>(alloc_raw);
  if (alloc == nullptr || out == nullptr || bytes == 0) {
    return -1;
  }
  VkBufferCreateInfo bc{};
  bc.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bc.size = static_cast<VkDeviceSize>(bytes);
  bc.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
             VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  bc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VmaAllocationCreateInfo ac{};
  // ReBAR-style: require host-visible + coherent; PREFER device-local
  // (resizable-BAR heap) but accept plain staging when the driver lacks it.
  ac.requiredFlags = static_cast<VkMemoryPropertyFlags>(
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  ac.preferredFlags = static_cast<VkMemoryPropertyFlags>(
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  ac.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
             VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
  VkBuffer buf = VK_NULL_HANDLE;
  VmaAllocation handle = nullptr;
  VmaAllocationInfo info{};
  if (vmaCreateBuffer(alloc, &bc, &ac, &buf, &handle, &info) != VK_SUCCESS) {
    return -1;
  }
  VkBufferDeviceAddressInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
  ai.buffer = buf;
  out->buffer_bits = static_cast<std::uint64_t>(
      reinterpret_cast<std::uintptr_t>(buf));
  out->alloc = reinterpret_cast<Allocation_T*>(handle);
  out->mapped = info.pMappedData;
  out->device_addr = static_cast<std::uint64_t>(
      vkGetBufferDeviceAddress(static_cast<VkDevice>(device), &ai));
  return (info.pMappedData != nullptr) ? 0 : -1;
}

auto EngineVmaDestroyRing(Allocator_T* alloc_raw, std::uint64_t buffer_bits,
                          Allocation_T* handle_raw) -> void {
  VmaAllocator alloc = reinterpret_cast<VmaAllocator>(alloc_raw);
  VmaAllocation handle = reinterpret_cast<VmaAllocation>(handle_raw);
  if (alloc == nullptr) {
    return;
  }
  vmaDestroyBuffer(alloc,
                   reinterpret_cast<VkBuffer>(
                       static_cast<std::uintptr_t>(buffer_bits)),
                   handle);
}

auto EngineVmaDestroy(Allocator_T* alloc_raw) -> void {
  VmaAllocator alloc = reinterpret_cast<VmaAllocator>(alloc_raw);
  if (alloc != nullptr) {
    vmaDestroyAllocator(alloc);
  }
}

}  // extern "C"
#endif

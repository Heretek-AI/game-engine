// vulkan_device.cpp — M3-01: Vulkan 1.3 bootstrap implementation.
//
// Two build modes (selected by native/renderer/CMakeLists.txt):
//   ENGINE_RENDERER_HAS_VULKAN — real path: volk + Vulkan SDK + VMA.
//     init(): volkInitialize -> vkCreateInstance (1.3) -> pick physical
//     device -> vkCreateDevice (dynamicRendering/sync2/BDA/indexing/
//     indirectCount) -> volkLoadDevice -> VMA allocator -> upload ring.
//   (undefined) — stub path: init() reports kVulkanLoaderFailed, ring
//     still works as a CPU-side bump allocator so RenderGraph/upload
//     tests run on machines without a Vulkan SDK.
//
// The upload ring prefers ReBAR-style VRAM: when VMA is available it
// allocates HOST_VISIBLE | HOST_COHERENT device-local memory
// (resizable-BAR heap when the driver exposes one) and returns real
// VkBufferDeviceAddress. Otherwise it falls back to 64-byte aligned
// CPU memory with device_address 0 (still valid for CPU-side tests).
#include "engine/renderer/vulkan_device.hpp"

#if defined(ENGINE_RENDERER_HAS_VULKAN)
// volk first: it defines VK_NO_PROTOTYPES and hard-errors if
// <vulkan/vulkan.h> was already included, so it must precede any header
// that might pull Vulkan headers (SDL_vulkan via renderer_window).
// NOTE: vk_mem_alloc.h is included ONLY in vma_impl.cpp (which carries
// per-file -Wno-* relaxations). This TU uses volk types + forward
// declarations only, so our -Werror hot flags stay clean.
#include <volk.h>
#endif

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#if defined(_WIN32)
#include <malloc.h>
#endif

namespace engine::renderer {
class RendererWindow;
}  // namespace engine::renderer

#include "engine/renderer/renderer_window.hpp"

// Surface helper implemented in renderer_window.cpp (owns the SDL
// includes). VkInstance/VkSurfaceKHR degrade to void* when the Vulkan
// SDK is absent.
auto RendererWindowCreateSurface(const engine::renderer::RendererWindow& window,
                                 void* instance, void* surface_out) noexcept
    -> bool;

namespace engine::renderer::vma_bridge {
// Declarations only; bodies in vma_impl.cpp.
struct Allocator_T;
struct Allocation_T;
struct BufferBundle {
  std::uint64_t buffer_bits{0};  // VkBuffer bits (opaque here)
  Allocation_T* alloc{nullptr};
  void* mapped{nullptr};
  std::uint64_t device_addr{0};
};
extern "C" {
auto EngineVmaCreateForDevice(void* physical, void* device, void* instance)
    -> Allocator_T*;
auto EngineVmaCreateUploadRing(Allocator_T* alloc, void* device,
                               std::uint64_t bytes, BufferBundle* out) -> int;
auto EngineVmaDestroyRing(Allocator_T* alloc, std::uint64_t buffer_bits,
                          Allocation_T* handle) -> void;
auto EngineVmaDestroy(Allocator_T* alloc) -> void;
}  // extern "C"
// Process slots: one device per process in M3-01 (M3-02 moves these into
// VulkanDevice once the header can see VmaAllocator without leaking it).
inline Allocator_T* g_alloc = nullptr;
inline BufferBundle g_ring{};
}  // namespace engine::renderer::vma_bridge

namespace engine::renderer {

#if defined(ENGINE_RENDERER_HAS_VULKAN)
// (VMA wrapper declarations live in engine::renderer::vma_bridge above;
// this TU's engine::renderer section only uses them.)
#endif

VulkanDevice::~VulkanDevice() noexcept { shutdown(); }

auto VulkanDevice::init(const DeviceCreateInfo& info) noexcept -> Status {
#if !defined(ENGINE_RENDERER_HAS_VULKAN)
  (void)info;
  return MakeFail(StatusCode::kVulkanLoaderFailed,
                  "Vulkan SDK unavailable (ENGINE_RENDERER_HAS_VULKAN undefined)");
#else
  if (device_ != nullptr) {
    return MakeFail(StatusCode::kAlreadyInitialized, "device already initialized");
  }
  if (volkInitialize() != VK_SUCCESS) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "volkInitialize failed");
  }
  std::uint32_t loader_version = 0;
  if (vkEnumerateInstanceVersion == nullptr ||
      vkEnumerateInstanceVersion(&loader_version) != VK_SUCCESS ||
      VK_VERSION_MAJOR(loader_version) < 1) {
    return MakeFail(StatusCode::kVulkanLoaderFailed,
                    "no Vulkan 1.x loader found");
  }

  // --- instance ---------------------------------------------------------
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = (info.app_name != nullptr) ? info.app_name : "GameEngine";
  app.applicationVersion = info.app_version;
  app.apiVersion = VK_API_VERSION_1_3;

  const char* layers[1] = {"VK_LAYER_KHRONOS_validation"};
  VkInstanceCreateInfo ic{};
  ic.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ic.pApplicationInfo = &app;
  ic.enabledExtensionCount = info.extra_instance_extension_count;
  ic.ppEnabledExtensionNames = info.extra_instance_extensions;
  if (info.enable_validation) {
    ic.enabledLayerCount = 1;
    ic.ppEnabledLayerNames = layers;
  }
  VkInstance inst = nullptr;
  if (vkCreateInstance(&ic, nullptr, &inst) != VK_SUCCESS) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "vkCreateInstance failed");
  }
  volkLoadInstance(inst);
  instance_ = inst;

  // --- physical device ---------------------------------------------------
  std::uint32_t gpu_count = 0;
  if (vkEnumeratePhysicalDevices(instance_, &gpu_count, nullptr) != VK_SUCCESS ||
      gpu_count == 0) {
    shutdown();
    return MakeFail(StatusCode::kNoPhysicalDevice, "no physical devices");
  }
  constexpr std::uint32_t kMaxGpus = 16;
  VkPhysicalDevice gpus[kMaxGpus];
  std::uint32_t fetch = (gpu_count < kMaxGpus) ? gpu_count : kMaxGpus;
  if (vkEnumeratePhysicalDevices(instance_, &fetch, gpus) != VK_SUCCESS) {
    shutdown();
    return MakeFail(StatusCode::kNoPhysicalDevice, "enumerate failed");
  }
  const char* hint = (info.device_hint != nullptr) ? info.device_hint : "";
  VkPhysicalDevice chosen = nullptr;
  VkPhysicalDeviceProperties chosen_props{};
  // Pass 1: honor hint substring match. Pass 2: discrete GPU. Pass 3: first.
  for (int pass = 0; pass < 3 && chosen == nullptr; ++pass) {
    for (std::uint32_t i = 0; i < fetch; ++i) {
      VkPhysicalDeviceProperties pr{};
      vkGetPhysicalDeviceProperties(gpus[i], &pr);
      if (pass == 0) {
        if (hint[0] == '\0') {
          continue;
        }
        std::string name(pr.deviceName);
        std::string h(hint);
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        std::transform(h.begin(), h.end(), h.begin(), ::tolower);
        if (name.find(h) == std::string::npos) {
          continue;
        }
      } else if (pass == 1) {
        if (pr.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
          continue;
        }
      }
      chosen = gpus[i];
      chosen_props = pr;
      break;
    }
  }
  if (chosen == nullptr) {
    shutdown();
    return MakeFail(StatusCode::kNoPhysicalDevice, "no suitable GPU");
  }
  physical_ = chosen;
  std::snprintf(device_name_, sizeof(device_name_), "%s",
                chosen_props.deviceName);
  api_version_ = chosen_props.apiVersion;

  // --- queue family -------------------------------------------------------
  constexpr std::uint32_t kMaxFamilies = 16;
  std::uint32_t fam_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_, &fam_count, nullptr);
  if (fam_count == 0 || fam_count > kMaxFamilies) {
    shutdown();
    return MakeFail(StatusCode::kNoPhysicalDevice, "no queue families");
  }
  VkQueueFamilyProperties fams[kMaxFamilies];
  vkGetPhysicalDeviceQueueFamilyProperties(physical_, &fam_count, fams);
  std::uint32_t gfx = 0xFFFFFFFFu;
  for (std::uint32_t i = 0; i < fam_count; ++i) {
    if ((fams[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u) {
      gfx = i;
      break;
    }
  }
  if (gfx == 0xFFFFFFFFu) {
    shutdown();
    return MakeFail(StatusCode::kNoPhysicalDevice, "no graphics queue");
  }
  graphics_family_ = gfx;

  // --- logical device (M3 feature set) ------------------------------------
  float prio = 1.0F;
  VkDeviceQueueCreateInfo qc{};
  qc.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qc.queueFamilyIndex = gfx;
  qc.queueCount = 1;
  qc.pQueuePriorities = &prio;

  VkPhysicalDeviceVulkan13Features f13{};
  f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  f13.dynamicRendering = VK_TRUE;
  f13.synchronization2 = VK_TRUE;

  VkPhysicalDeviceVulkan12Features f12{};
  f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  f12.pNext = &f13;
  f12.bufferDeviceAddress = VK_TRUE;
  f12.descriptorIndexing = VK_TRUE;
  f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
  f12.descriptorBindingPartiallyBound = VK_TRUE;
  f12.descriptorBindingVariableDescriptorCount = VK_TRUE;
  f12.runtimeDescriptorArray = VK_TRUE;
  f12.drawIndirectCount = VK_TRUE;

  VkPhysicalDeviceFeatures2 f2{};
  f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  f2.pNext = &f12;
  vkGetPhysicalDeviceFeatures2(physical_, &f2);
  features_.dynamic_rendering = f13.dynamicRendering == VK_TRUE;
  features_.synchronization2 = f13.synchronization2 == VK_TRUE;
  features_.buffer_device_address = f12.bufferDeviceAddress == VK_TRUE;
  features_.descriptor_indexing = f12.descriptorIndexing == VK_TRUE;
  features_.draw_indirect_count = f12.drawIndirectCount == VK_TRUE;

  VkDeviceCreateInfo dc{};
  dc.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  dc.pNext = &f12;
  dc.queueCreateInfoCount = 1;
  dc.pQueueCreateInfos = &qc;
  VkDevice dev = nullptr;
  if (vkCreateDevice(physical_, &dc, nullptr, &dev) != VK_SUCCESS) {
    shutdown();
    return MakeFail(StatusCode::kDeviceCreateFailed, "vkCreateDevice failed");
  }
  device_ = dev;
  volkLoadDevice(device_);
  vkGetDeviceQueue(device_, gfx, 0, &graphics_queue_);

  if (!features_.dynamic_rendering || !features_.synchronization2 ||
      !features_.buffer_device_address) {
    shutdown();
    return MakeFail(StatusCode::kDeviceCreateFailed,
                    "GPU lacks dynamicRendering/sync2/BDA");
  }

  // --- VMA allocator + ReBAR-style upload ring ------------------------------
  // VMA calls go through vma_impl.cpp wrappers (hot TU must not touch
  // vk_mem_alloc.h). ReBAR-style = HOST_VISIBLE|HOST_COHERENT preferred
  // DEVICE_LOCAL; plain staging when the driver lacks a BAR heap.
  auto* vma = vma_bridge::EngineVmaCreateForDevice(physical_, device_,
                                                   instance_);
  if (vma == nullptr) {
    shutdown();
    return MakeFail(StatusCode::kVmaInitFailed, "vmaCreateAllocator failed");
  }
  vma_bridge::g_alloc = vma;
  allocator_ready_ = true;
  use_vma_ = true;
  vma_bridge::BufferBundle ring{};
  if (vma_bridge::EngineVmaCreateUploadRing(vma, device_, kUploadRingBytes,
                                            &ring) != 0 ||
      ring.mapped == nullptr) {
    shutdown();
    return MakeFail(StatusCode::kVmaInitFailed, "upload ring alloc failed");
  }
  vma_bridge::g_ring = ring;
  ring_base_ = ring.mapped;
  ring_device_addr_ = ring.device_addr;
  ring_offset_ = 0;
  frame_index_ = 0;
  ring_owns_ = false;  // VMA owns the memory; ring_base_ is a mapping
  return MakeOk();
#endif
}

auto VulkanDevice::shutdown() noexcept -> void {
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  if (device_ != nullptr) {
    if (vma_bridge::g_alloc != nullptr) {
      vma_bridge::EngineVmaDestroyRing(vma_bridge::g_alloc,
                                       vma_bridge::g_ring.buffer_bits,
                                       vma_bridge::g_ring.alloc);
      vma_bridge::EngineVmaDestroy(vma_bridge::g_alloc);
      vma_bridge::g_alloc = nullptr;
      vma_bridge::g_ring = vma_bridge::BufferBundle{};
    }
    allocator_ready_ = false;
    use_vma_ = false;
    ring_base_ = nullptr;
    ring_device_addr_ = 0;
    graphics_queue_ = nullptr;
    vkDestroyDevice(device_, nullptr);
    device_ = nullptr;
  }
  if (instance_ != nullptr) {
    vkDestroyInstance(instance_, nullptr);
    instance_ = nullptr;
  }
  physical_ = nullptr;
  graphics_family_ = 0xFFFFFFFFu;
  features_ = DeviceFeatures{};
  device_name_[0] = '\0';
  api_version_ = 0;
#else
  if (ring_owns_) {
#if defined(_WIN32)
    _aligned_free(ring_base_);
#else
    free(ring_base_);
#endif
  }
  ring_base_ = nullptr;
  ring_owns_ = false;
#endif
  ring_offset_ = 0;
  frame_index_ = 0;
}

auto VulkanDevice::create_surface(const RendererWindow& window,
                                  VkSurfaceKHR* surface_out) noexcept
    -> Status {
  if (surface_out == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "surface_out null");
  }
  *surface_out = nullptr;
#if !defined(ENGINE_RENDERER_HAS_VULKAN)
  (void)window;
  return MakeFail(StatusCode::kVulkanLoaderFailed, "Vulkan SDK unavailable");
#elif !defined(ENGINE_RENDERER_HAS_SDL3)
  (void)window;
  return MakeFail(StatusCode::kSurfaceCreateFailed, "SDL3 unavailable");
#else
  if (device_ == nullptr || instance_ == nullptr) {
    return MakeFail(StatusCode::kNotInitialized, "device not initialized");
  }
  VkSurfaceKHR surf = nullptr;
  if (!::RendererWindowCreateSurface(window, instance_, &surf) ||
      surf == nullptr) {
    return MakeFail(StatusCode::kSurfaceCreateFailed, "surface create failed");
  }
  *surface_out = surf;
  return MakeOk();
#endif
}

auto VulkanDevice::destroy_surface(VkSurfaceKHR surface) noexcept -> void {
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  if (surface != nullptr && instance_ != nullptr) {
    vkDestroySurfaceKHR(instance_, surface, nullptr);
  }
#else
  (void)surface;
#endif
}

auto VulkanDevice::ring_alloc(std::uint64_t size, std::uint64_t align) noexcept
    -> RingAlloc {
  RingAlloc out{};
  if (size == 0 || size > kUploadRingBytes) {
    return out;
  }
  if (align == 0) {
    align = 1;
  }
  // Lazy CPU fallback so tests can exercise the ring without init().
  if (ring_base_ == nullptr && !allocator_ready_) {
#if defined(_WIN32)
    ring_base_ = _aligned_malloc(
        static_cast<std::size_t>(kUploadRingBytes), 64);
#else
    void* p = nullptr;
    if (posix_memalign(&p, 64, static_cast<std::size_t>(kUploadRingBytes)) != 0) {
      p = nullptr;
    }
    ring_base_ = p;
#endif
    if (ring_base_ == nullptr) {
      return out;
    }
    ring_owns_ = true;
  }
  if (ring_base_ == nullptr) {
    return out;
  }
  std::uint64_t aligned = (ring_offset_ + (align - 1)) & ~(align - 1);
  if (aligned + size > kUploadRingBytes) {
    aligned = 0;  // wrap; caller fences the in-flight frame (M3-02)
  }
  auto* base = static_cast<char*>(ring_base_);
  out.cpu_ptr = base + aligned;
  out.buffer_offset = aligned;
  out.size = size;
  out.device_address =
      (ring_device_addr_ != 0) ? (ring_device_addr_ + aligned) : 0;
  ring_offset_ = aligned + size;
  return out;
}

auto VulkanDevice::ring_reset_frame() noexcept -> void {
  ring_offset_ = 0;
  frame_index_ += 1;
}

auto VulkanDevice::ring_stats() const noexcept -> UploadRingStats {
  UploadRingStats s{};
  s.capacity = kUploadRingBytes;
  s.offset = ring_offset_;
  s.frame_index = frame_index_;
  return s;
}

#if defined(ENGINE_RENDERER_HAS_VULKAN)
// (VMA wrapper declarations live in engine::renderer::vma_bridge above;
// this TU's engine::renderer section only uses them.)
#endif

}  // namespace engine::renderer

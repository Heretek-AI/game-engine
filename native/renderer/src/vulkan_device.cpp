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
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#if defined(_WIN32)
#include <malloc.h>
#endif

namespace engine::renderer {
class RendererWindow;
}  // namespace engine::renderer

#include "engine/renderer/renderer_window.hpp"
#include "vma_bridge.hpp"

namespace engine::renderer {

namespace {
// Release the owned CPU fallback ring (posix_memalign/_aligned_malloc path
// from ring_alloc's lazy fallback). Called at the top of shutdown() and
// before init() hands ring_base_ to the VMA mapping so the fallback block
// is never leaked or orphaned by overwrite.
void release_cpu_ring(void*& base, bool& owns) noexcept {
  if (!owns) {
    return;
  }
#if defined(_WIN32)
  _aligned_free(base);  // pairs with _aligned_malloc (ring_alloc fallback)
#else
  // pairs with posix_memalign (ring_alloc fallback) — free is correct here
  std::free(base);  // NOSONAR(cpp:S1231) — posix_memalign/free pairing
#endif
  base = nullptr;
  owns = false;
}
}  // namespace

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
  if (vma_bridge::g_alloc != nullptr) {
    // One device per process: the VMA allocator/ring slots are process-wide
    // (M3-01). A second init() would leak the first allocator and hand the
    // first device's shutdown() the second device's allocator. Reject here
    // WITHOUT calling shutdown() — that would destroy the OTHER device's
    // state through the same globals.
    return MakeFail(StatusCode::kAlreadyInitialized,
                    "another VulkanDevice owns the VMA allocator");
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
#if defined(__APPLE__)
  // P7-02: MoltenVK advertises VK_KHR_portability_subset, so the loader
  // hides every physical device unless the instance requests
  // VK_KHR_portability_enumeration + the enumerate-portability flag.
  // Probe for the extension; fail closed (same message shape as other
  // instance failures) when the SDK predates it.
  // Stack arrays: init() is documented NOT thread-safe (one call at
  // startup), so frame-lifetime locals are safe here.
  constexpr std::uint32_t kMaxInstExts = 256;
  std::array<VkExtensionProperties, kMaxInstExts> avail{};
  constexpr std::uint32_t kMaxMergedExts = 64;
  std::array<const char*, kMaxMergedExts> merged{};
  {
    std::uint32_t avail_count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &avail_count,
                                               nullptr) != VK_SUCCESS) {
      return MakeFail(StatusCode::kVulkanLoaderFailed,
                      "instance extension query failed");
    }
    std::uint32_t avail_fetch =
        (avail_count < kMaxInstExts) ? avail_count : kMaxInstExts;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &avail_fetch,
                                               avail.data()) != VK_SUCCESS) {
      return MakeFail(StatusCode::kVulkanLoaderFailed,
                      "instance extension fetch failed");
    }
    constexpr std::string_view kWant =
        VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    bool have_portability = false;
    for (std::uint32_t i = 0; i < avail_fetch; ++i) {
      const char* name = avail[i].extensionName;
      std::size_t len = 0;
      while (len < VK_MAX_EXTENSION_NAME_SIZE && name[len] != '\0') {
        ++len;
      }
      if (len != kWant.size()) {
        continue;
      }
      bool same = true;
      for (std::size_t j = 0; j < len; ++j) {
        if (name[j] != kWant[j]) {
          same = false;
          break;
        }
      }
      if (same) {
        have_portability = true;
        break;
      }
    }
    if (!have_portability) {
      return MakeFail(StatusCode::kVulkanLoaderFailed,
                      "VK_KHR_portability_enumeration missing");
    }
  }
  // Bounded merge: caller extensions + portability enumeration.
  // Never truncate the caller's list: one slot is reserved for the
  // portability extension, anything that does not fit is rejected.
  const std::uint32_t caller_ext_count = info.extra_instance_extension_count;
  if (caller_ext_count > kMaxMergedExts - 1) {
    return MakeFail(StatusCode::kInvalidArgument,
                    "too many instance extensions for the portability merge");
  }
  for (std::uint32_t i = 0; i < caller_ext_count; ++i) {
    merged[i] = info.extra_instance_extensions[i];
  }
  merged[caller_ext_count] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
  const std::uint32_t apple_ext_count = caller_ext_count + 1;
  ic.enabledExtensionCount = apple_ext_count;
  ic.ppEnabledExtensionNames = merged.data();
  ic.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#else
  ic.enabledExtensionCount = info.extra_instance_extension_count;
  ic.ppEnabledExtensionNames = info.extra_instance_extensions;
#endif
  if (info.enable_validation) {
    ic.enabledLayerCount = 1;
    ic.ppEnabledLayerNames = layers;
  }
  VkInstance inst = nullptr;
  VkResult ic_res = vkCreateInstance(&ic, nullptr, &inst);
  if (ic_res == VK_ERROR_INCOMPATIBLE_DRIVER) {
    // Loader present, no usable ICD (headers+loader installed, driver
    // missing — e.g. libvulkan-dev without mesa-vulkan-drivers). This is
    // "no GPU", not "broken loader": report kNoPhysicalDevice so callers
    // take the headless-skip path instead of failing the test run.
    return MakeFail(StatusCode::kNoPhysicalDevice,
                    "no Vulkan driver (incompatible driver)");
  }
  if (ic_res != VK_SUCCESS) {
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
  // GPU selection also filters M3-required features per candidate (Vulkan
  // 1.3 + dynamicRendering + synchronization2 + bufferDeviceAddress): the
  // default path must not die on the first discrete GPU when a later GPU
  // in the list is the compatible one (multi-GPU hosts).
  auto gpu_has_m3_features = [](VkPhysicalDevice gpu) -> bool {
    VkPhysicalDeviceProperties pr{};
    vkGetPhysicalDeviceProperties(gpu, &pr);
    if (VK_VERSION_MAJOR(pr.apiVersion) < 1 ||
        (VK_VERSION_MAJOR(pr.apiVersion) == 1 &&
         VK_VERSION_MINOR(pr.apiVersion) < 3)) {
      return false;
    }
    VkPhysicalDeviceVulkan13Features q13{};
    q13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    VkPhysicalDeviceVulkan12Features q12{};
    q12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    q12.pNext = &q13;
    VkPhysicalDeviceFeatures2 q2{};
    q2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    q2.pNext = &q12;
    vkGetPhysicalDeviceFeatures2(gpu, &q2);
    // Every feature init() enables below must gate selection: enabling an
    // unsupported feature fails vkCreateDevice (FEATURE_NOT_PRESENT), and
    // the multi-GPU walk must skip such GPUs instead of dying on them.
    return q13.dynamicRendering == VK_TRUE &&
           q13.synchronization2 == VK_TRUE &&
           q12.bufferDeviceAddress == VK_TRUE &&
           q12.descriptorIndexing == VK_TRUE &&
           q12.shaderSampledImageArrayNonUniformIndexing == VK_TRUE &&
           q12.descriptorBindingPartiallyBound == VK_TRUE &&
           q12.descriptorBindingVariableDescriptorCount == VK_TRUE &&
           q12.runtimeDescriptorArray == VK_TRUE;
    // drawIndirectCount is optional (P7-02): MoltenVK on Apple's
    // paravirtual device lacks it, and no production path records a
    // counted draw yet. features().draw_indirect_count reports it.
  };
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
      if (!gpu_has_m3_features(gpus[i])) {
        continue;  // skip GPUs that cannot serve the M3 feature set
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

  // Query with ZEROED structs (never preset VK_TRUE): on a sub-1.3 driver
  // the query leaves unsupported fields false, so the report reflects the
  // GPU. Enablement uses separate creation structs below — reusing query
  // results in dc.pNext could request features the GPU lacks and fail
  // vkCreateDevice with VK_ERROR_FEATURE_NOT_PRESENT.
  VkPhysicalDeviceVulkan13Features f13{};
  f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;

  VkPhysicalDeviceVulkan12Features f12{};
  f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  f12.pNext = &f13;

  VkPhysicalDeviceFeatures2 f2{};
  f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  f2.pNext = &f12;
  vkGetPhysicalDeviceFeatures2(physical_, &f2);
  features_.dynamic_rendering = f13.dynamicRendering == VK_TRUE;
  features_.synchronization2 = f13.synchronization2 == VK_TRUE;
  features_.buffer_device_address = f12.bufferDeviceAddress == VK_TRUE;
  features_.descriptor_indexing = f12.descriptorIndexing == VK_TRUE;
  features_.draw_indirect_count = f12.drawIndirectCount == VK_TRUE;
  // All bindless + indirect-count features are hard requirements (the
  // enable structs below request every one): fail here with a named
  // feature instead of a generic FEATURE_NOT_PRESENT from vkCreateDevice.
  const bool bindless_ok =
      f12.descriptorIndexing == VK_TRUE &&
      f12.shaderSampledImageArrayNonUniformIndexing == VK_TRUE &&
      f12.descriptorBindingPartiallyBound == VK_TRUE &&
      f12.descriptorBindingVariableDescriptorCount == VK_TRUE &&
      f12.runtimeDescriptorArray == VK_TRUE;
  if (!features_.dynamic_rendering || !features_.synchronization2 ||
      !features_.buffer_device_address || !bindless_ok) {
    shutdown();
    return MakeFail(StatusCode::kDeviceCreateFailed,
                    "GPU lacks dynamicRendering/sync2/BDA/bindless");
  }

  // Enable ONLY the required features (never every queried feature).
  // M3-04 GI fullscreen triangles read gl_VertexIndex (SV_VertexID):
  // slang emits SPIR-V Capability DrawParameters, which needs
  // shaderDrawParameters (Vulkan 1.1 core struct, VUID-08740).
  VkPhysicalDeviceVulkan11Features e11{};
  e11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
  e11.shaderDrawParameters = VK_TRUE;

  VkPhysicalDeviceVulkan13Features e13{};
  e13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  e13.pNext = &e11;
  e13.dynamicRendering = VK_TRUE;
  e13.synchronization2 = VK_TRUE;

  VkPhysicalDeviceVulkan12Features e12{};
  e12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  e12.pNext = &e13;
  e12.bufferDeviceAddress = VK_TRUE;
  e12.descriptorIndexing = VK_TRUE;
  e12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
  e12.descriptorBindingPartiallyBound = VK_TRUE;
  e12.descriptorBindingVariableDescriptorCount = VK_TRUE;
  e12.runtimeDescriptorArray = VK_TRUE;
  // Optional: enable only when supported (see selection above).
  e12.drawIndirectCount = f12.drawIndirectCount;

  VkDeviceCreateInfo dc{};
  dc.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  dc.pNext = &e12;
  dc.queueCreateInfoCount = 1;
  dc.pQueueCreateInfos = &qc;
  // P1-03: opt-in swapchain. Presence is probed on the CHOSEN physical
  // device (not the first in the list) via vkEnumerateDeviceExtension-
  // Properties; absent -> kDeviceCreateFailed + exact spec message.
  // P7-02: MoltenVK devices also require VK_KHR_portability_subset on
  // the logical device. The string literal is used (not the macro: some
  // SDKs such as libvulkan-dev 1.4.309 ship the enumeration macro but
  // not the subset one); presence is probed before enabling.
#if !defined(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME)
#define ENGINE_PORTABILITY_SUBSET_NAME "VK_KHR_portability_subset"
#else
#define ENGINE_PORTABILITY_SUBSET_NAME \
  VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME
#endif
  // Probe the CHOSEN device's extensions once for both paths: the
  // swapchain path needs VK_KHR_swapchain, and every MoltenVK device
  // (headless or not) must enable VK_KHR_portability_subset when it is
  // advertised. Two-pass enumerate into a stack window; VK_INCOMPLETE
  // (more than the window holds) still yields the first entries.
  constexpr std::uint32_t kMaxDevExts = 256;
  std::array<VkExtensionProperties, kMaxDevExts> dev_exts{};
  std::uint32_t ext_count = 0;
  std::uint32_t fetch_exts = 0;
  if (vkEnumerateDeviceExtensionProperties(physical_, nullptr, &ext_count,
                                           nullptr) == VK_SUCCESS) {
    fetch_exts = (ext_count < kMaxDevExts) ? ext_count : kMaxDevExts;
    const VkResult er = vkEnumerateDeviceExtensionProperties(
        physical_, nullptr, &fetch_exts, dev_exts.data());
    if (er != VK_SUCCESS && er != VK_INCOMPLETE) {
      fetch_exts = 0;
    }
  }
  bool have_swap = false;
  bool have_subset = false;
  for (std::uint32_t i = 0; i < fetch_exts; ++i) {
    if (std::strcmp(dev_exts[i].extensionName,
                    VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
      have_swap = true;
    }
    if (std::strcmp(dev_exts[i].extensionName,
                    ENGINE_PORTABILITY_SUBSET_NAME) == 0) {
      have_subset = true;
    }
  }
  std::array<const char*, 2> dev_ext_names{};
  std::uint32_t dev_ext_count = 0;
  if (info.enable_swapchain) {
    // Absent (or past the probe window): fail closed with the spec
    // message, never a silent no-present device.
    if (!have_swap) {
      shutdown();
      return MakeFail(StatusCode::kDeviceCreateFailed,
                      "VK_KHR_swapchain missing");
    }
    dev_ext_names[dev_ext_count++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
  }
  // P7-02: enabled only where the device offers it (MoltenVK). Linux and
  // Windows drivers never advertise it, so their lists are unchanged.
  if (have_subset) {
    dev_ext_names[dev_ext_count++] = ENGINE_PORTABILITY_SUBSET_NAME;
  }
  dc.enabledExtensionCount = dev_ext_count;
  dc.ppEnabledExtensionNames = dev_ext_count > 0 ? dev_ext_names.data() : nullptr;
  VkDevice dev = nullptr;
  if (vkCreateDevice(physical_, &dc, nullptr, &dev) != VK_SUCCESS) {
    shutdown();
    return MakeFail(StatusCode::kDeviceCreateFailed, "vkCreateDevice failed");
  }
  device_ = dev;
  volkLoadDevice(device_);
  vkGetDeviceQueue(device_, gfx, 0, &graphics_queue_);

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
  release_cpu_ring(ring_base_, ring_owns_);  // drop CPU fallback if any
  ring_base_ = ring.mapped;
  ring_device_addr_ = ring.device_addr;
  ring_offset_ = 0;
  frame_index_ = 0;
  ring_owns_ = false;  // VMA owns the memory; ring_base_ is a mapping
  return MakeOk();
#endif
}

auto VulkanDevice::shutdown() noexcept -> void {
  release_cpu_ring(ring_base_, ring_owns_);  // owned fallback first
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

auto VulkanDevice::supports_present(VkSurfaceKHR surface) const noexcept
    -> bool {
#if !defined(ENGINE_RENDERER_HAS_VULKAN)
  (void)surface;
  return false;
#else
  if (surface == nullptr || device_ == nullptr || physical_ == nullptr ||
      graphics_family_ == 0xFFFFFFFFu) {
    return false;
  }
  VkBool32 ok = VK_FALSE;
  if (vkGetPhysicalDeviceSurfaceSupportKHR(physical_, graphics_family_,
                                           surface, &ok) != VK_SUCCESS) {
    return false;
  }
  return ok == VK_TRUE;
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

auto VulkanDevice::proc(const char* name) const noexcept -> void* {
#if !defined(ENGINE_RENDERER_HAS_VULKAN)
  (void)name;
  return nullptr;
#else
  if (name == nullptr || device_ == nullptr) {
    return nullptr;
  }
  // volk's vkGetDeviceProcAddr global is valid after volkLoadDevice in
  // init(); instance-level entry points resolve through the instance.
  auto* get_dev =
      reinterpret_cast<PFN_vkGetDeviceProcAddr>(vkGetDeviceProcAddr);
  if (get_dev != nullptr) {
    if (auto* p = get_dev(device_, name); p != nullptr) {
      return reinterpret_cast<void*>(p);
    }
  }
  if (instance_ != nullptr) {
    auto* get_inst =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(vkGetInstanceProcAddr);
    if (get_inst != nullptr) {
      if (auto* p = get_inst(instance_, name); p != nullptr) {
        return reinterpret_cast<void*>(p);
      }
    }
  }
  return nullptr;
#endif
}

auto VulkanDevice::instance_proc(const char* name) const noexcept -> void* {
#if !defined(ENGINE_RENDERER_HAS_VULKAN)
  (void)name;
  return nullptr;
#else
  if (name == nullptr || instance_ == nullptr) {
    return nullptr;
  }
  // Instance-level FIRST (physical-device queries live here), then device.
  auto* get_inst =
      reinterpret_cast<PFN_vkGetInstanceProcAddr>(vkGetInstanceProcAddr);
  if (get_inst != nullptr) {
    if (auto* p = get_inst(instance_, name); p != nullptr) {
      return reinterpret_cast<void*>(p);
    }
  }
  return proc(name);
#endif
}

#if defined(ENGINE_RENDERER_HAS_VULKAN)
// (VMA wrapper declarations live in engine::renderer::vma_bridge above;
// this TU's engine::renderer section only uses them.)
#endif

}  // namespace engine::renderer

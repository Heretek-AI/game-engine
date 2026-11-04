// viewport_share.cpp — P3-13: shareable viewport image implementation.
//
// Export path (Linux): VkImage created with
// VkExternalMemoryImageCreateInfo (OPAQUE_FD_BIT) chained into
// VkImageCreateInfo, memory allocated with VkExportMemoryAllocateInfo
// chained into VkMemoryAllocateInfo, handle exported per share() via
// vkGetMemoryFdKHR (VkMemoryGetFdInfoKHR, OPAQUE_FD_BIT_KHR).
// OPAQUE_FD is the type Avalonia imports as
// VulkanOpaquePosixFileDescriptor. Each share() exports a FRESH fd
// (the spec requires a new fd per vkGetMemoryFdKHR call); ownership
// passes to the caller.
//
// Entry points resolve through VulkanDevice::proc()/instance_proc()
// (gotcha 11: no second volk copy in consumers). vkGetMemoryFdKHR is a
// DEVICE-level entry point; the VkImageCreateInfo struct types come
// from the Vulkan headers (compile-time, no lookup needed).
//
// Failure modes (all fail closed, CPU path stays default):
//   no Vulkan SDK          -> kVulkanLoaderFailed
//   uninitialized device    -> kNotInitialized / kInvalidArgument
//   no external-memory ext -> kUnsupported
// Non-Linux OS builds compile the image-creation path but share()
// reports invalid: fd export is VK_KHR_external_memory_fd, a Linux
// (+Fuchsia) extension. Windows (NT handle) / macOS (Metal/MoltenVK)
// share paths are future work; the bridge falls back to CPU there.
#if defined(ENGINE_RENDERER_HAS_VULKAN)
#include <volk.h>
#endif
#include "engine/renderer/viewport_share.hpp"

#if defined(ENGINE_RENDERER_HAS_VULKAN) && (defined(__linux__) || defined(__Fuchsia__))
#include <unistd.h>
#endif

namespace engine::renderer {

ViewportShare::~ViewportShare() noexcept { destroy(); }

auto ViewportShare::create(VulkanDevice& dev, std::uint32_t width,
                           std::uint32_t height) noexcept -> Status {
#if !defined(ENGINE_RENDERER_HAS_VULKAN)
  (void)dev;
  (void)width;
  (void)height;
  return MakeFail(StatusCode::kVulkanLoaderFailed, "Vulkan SDK unavailable");
#else
  if (!dev.valid()) {
    return MakeFail(StatusCode::kNotInitialized, "viewport share: device not initialized");
  }
  // The export path needs VK_KHR_external_memory(+_fd) ENABLED on this
  // logical device, not merely advertised: vkGetMemoryFdKHR is a
  // device-level extension command, and calling it on a device created
  // without the extensions is undefined. Gate on the recorded outcome
  // before probing formats or touching images.
  if (!dev.external_memory_enabled()) {
    return MakeFail(StatusCode::kUnsupported,
                    "viewport share: external memory extensions not enabled on device");
  }
  if (width == 0 || height == 0) {
    return MakeFail(StatusCode::kInvalidArgument, "viewport share: extent must be non-zero");
  }
  if (width > 16384u || height > 16384u) {
    return MakeFail(StatusCode::kInvalidArgument, "viewport share: extent too large");
  }
  if (image_ != nullptr) {
    return MakeFail(StatusCode::kAlreadyInitialized, "viewport share: already created");
  }
  VkDevice device = dev.device();
  VkPhysicalDevice physical = dev.physical_device();

  // Probe the external-memory entry points first: fail closed with
  // kUnsupported (CPU fallback) instead of creating a non-exportable
  // image that a later share() would misreport.
  using GetImageReqs2 = PFN_vkGetImageMemoryRequirements2;
  auto get_reqs2 = reinterpret_cast<GetImageReqs2>(
      dev.instance_proc("vkGetImageMemoryRequirements2"));
  if (get_reqs2 == nullptr) {
    return MakeFail(StatusCode::kUnsupported,
                    "viewport share: external memory unsupported (no reqs2)");
  }

  // Probe the exportable format BEFORE creating the image: a driver that
  // rejects the handle type must surface as kUnsupported (fail closed,
  // CPU fallback stays default), never as kDeviceCreateFailed from a
  // vkCreateImage chained with VkExternalMemoryImageCreateInfo — and the
  // unsupported path must not create (then destroy) an image.
  VkPhysicalDeviceExternalImageFormatInfo ext_fmt{};
  ext_fmt.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO;
#if defined(__linux__) || defined(__Fuchsia__)
  ext_fmt.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
  VkPhysicalDeviceImageFormatInfo2 fmt_info{};
  fmt_info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
  fmt_info.pNext = &ext_fmt;
  fmt_info.format = VK_FORMAT_R8G8B8A8_UNORM;
  fmt_info.type = VK_IMAGE_TYPE_2D;
  fmt_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  fmt_info.usage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  fmt_info.flags = 0;
  VkExternalImageFormatProperties ext_props{};
  ext_props.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES;
  VkImageFormatProperties2 props2{};
  props2.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
  props2.pNext = &ext_props;
  using GetPhysFmt2 = PFN_vkGetPhysicalDeviceImageFormatProperties2;
  auto get_fmt2 = reinterpret_cast<GetPhysFmt2>(
      dev.instance_proc("vkGetPhysicalDeviceImageFormatProperties2"));
  bool external_ok = false;
  if (get_fmt2 != nullptr &&
      get_fmt2(physical, &fmt_info, &props2) == VK_SUCCESS) {
#if defined(__linux__) || defined(__Fuchsia__)
    const auto feats =
        ext_props.externalMemoryProperties.externalMemoryFeatures;
    external_ok = (feats & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT) != 0;
#else
    external_ok = false;
#endif
  }
  if (!external_ok) {
    return MakeFail(StatusCode::kUnsupported,
                    "viewport share: format not exportable on this device");
  }

  // External image info: OPAQUE_FD on Linux (Avalonia's
  // VulkanOpaquePosixFileDescriptor); plain image elsewhere (share()
  // reports invalid there — CPU fallback).
#if defined(__linux__) || defined(__Fuchsia__)
  VkExternalMemoryImageCreateInfo ext_img{};
  ext_img.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
  ext_img.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif

  VkImageCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
#if defined(__linux__) || defined(__Fuchsia__)
  ici.pNext = &ext_img;
#endif
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = VK_FORMAT_R8G8B8A8_UNORM;
  ici.extent.width = width;
  ici.extent.height = height;
  ici.extent.depth = 1;
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

  VkImage img = nullptr;
  if (vkCreateImage(device, &ici, nullptr, &img) != VK_SUCCESS || img == nullptr) {
    return MakeFail(StatusCode::kDeviceCreateFailed, "viewport share: image create failed");
  }

  VkImageMemoryRequirementsInfo2 req_info{};
  req_info.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2;
  req_info.image = img;
  VkMemoryRequirements2 req2{};
  req2.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
  get_reqs2(device, &req_info, &req2);

  VkPhysicalDeviceMemoryProperties mem_props{};
  using GetMemProps = PFN_vkGetPhysicalDeviceMemoryProperties;
  auto get_mem_props = reinterpret_cast<GetMemProps>(
      dev.instance_proc("vkGetPhysicalDeviceMemoryProperties"));
  if (get_mem_props == nullptr) {
    vkDestroyImage(device, img, nullptr);
    return MakeFail(StatusCode::kUnsupported,
                    "viewport share: memory properties unavailable");
  }
  get_mem_props(physical, &mem_props);
  std::uint32_t mem_type = 0xFFFFFFFFu;
  for (std::uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
    if ((req2.memoryRequirements.memoryTypeBits & (1u << i)) != 0u) {
      mem_type = i;  // first compatible type: device-local preferred below
      if ((mem_props.memoryTypes[i].propertyFlags &
           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
        break;
      }
    }
  }
  if (mem_type == 0xFFFFFFFFu) {
    vkDestroyImage(device, img, nullptr);
    return MakeFail(StatusCode::kOutOfMemory, "viewport share: no memory type");
  }

#if defined(__linux__) || defined(__Fuchsia__)
  VkExportMemoryAllocateInfo export_info{};
  export_info.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
  export_info.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
#if defined(__linux__) || defined(__Fuchsia__)
  mai.pNext = &export_info;
#endif
  mai.allocationSize = req2.memoryRequirements.size;
  mai.memoryTypeIndex = mem_type;
  VkDeviceMemory mem = nullptr;
  if (vkAllocateMemory(device, &mai, nullptr, &mem) != VK_SUCCESS || mem == nullptr) {
    vkDestroyImage(device, img, nullptr);
    return MakeFail(StatusCode::kOutOfMemory, "viewport share: memory alloc failed");
  }
  if (vkBindImageMemory(device, img, mem, 0) != VK_SUCCESS) {
    vkFreeMemory(device, mem, nullptr);
    vkDestroyImage(device, img, nullptr);
    return MakeFail(StatusCode::kDeviceCreateFailed, "viewport share: bind failed");
  }

  dev_ = &dev;
  width_ = width;
  height_ = height;
  image_ = img;
  memory_ = mem;
  memory_type_ = mem_type;
  memory_size_ = req2.memoryRequirements.size;
#if defined(__linux__) || defined(__Fuchsia__)
  external_ = true;
#else
  external_ = false;
#endif
  return MakeOk();
#endif
}

auto ViewportShare::destroy() noexcept -> void {
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  if (image_ != nullptr && dev_ != nullptr && dev_->valid()) {
    vkDeviceWaitIdle(dev_->device());
    vkDestroyImage(dev_->device(), image_, nullptr);
    vkFreeMemory(dev_->device(), memory_, nullptr);
  } else if (image_ != nullptr && dev_ != nullptr) {
    // Device already gone (shutdown before share destroy): the handles
    // are dead; drop without touching the device.
  }
  image_ = nullptr;
  memory_ = nullptr;
  dev_ = nullptr;
  width_ = 0;
  height_ = 0;
  memory_type_ = 0xFFFFFFFFu;
  memory_size_ = 0;
  external_ = false;
#else
  live_ = false;
  dev_ = nullptr;
  width_ = 0;
  height_ = 0;
#endif
}

auto ViewportShare::valid() const noexcept -> bool {
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  return image_ != nullptr;
#else
  return live_;
#endif
}

auto ViewportShare::share() noexcept -> ViewportShareHandle {
  ViewportShareHandle out{};
#if !defined(ENGINE_RENDERER_HAS_VULKAN)
  return out;
#else
  if (image_ == nullptr || dev_ == nullptr || !dev_->valid()) {
    return out;
  }
#if !(defined(__linux__) || defined(__Fuchsia__))
  // fd export is VK_KHR_external_memory_fd (Linux/Fuchsia only).
  // Windows (NT handle) / macOS (IOSurface) need their own export
  // path: CPU fallback there.
  (void)external_;
  return out;
#else
  if (!external_) {
    return out;
  }
  using GetFd = PFN_vkGetMemoryFdKHR;
  void* raw_fd = dev_->proc("vkGetMemoryFdKHR");
  if (raw_fd == nullptr) {
    return out;
  }
  auto get_fd = reinterpret_cast<GetFd>(raw_fd);
  VkMemoryGetFdInfoKHR fd_info{};
  fd_info.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
  fd_info.memory = memory_;
  fd_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  int fd = -1;
  if (get_fd(dev_->device(), &fd_info, &fd) != VK_SUCCESS || fd < 0) {
    return out;
  }
  out.fd = fd;
  return out;
#endif
#endif
}

}  // namespace engine::renderer

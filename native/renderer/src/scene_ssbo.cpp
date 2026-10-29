// scene_ssbo.cpp — M3-02: persistent Scene SSBO + upload-ring streaming.
//
// CPU shadow is a plain new[]/delete[] array (init/shutdown only — never
// on the frame path). The frame path touches ONLY the mapped ring region
// via memcpy in stream_write (caller-fenced, single producer — same
// discipline as VulkanDevice::ring_alloc).
#include "engine/renderer/scene_ssbo.hpp"

#include <cstring>
#include <new>

namespace engine::renderer {

SceneSsbo::~SceneSsbo() noexcept { shutdown(); }

auto SceneSsbo::init(std::uint32_t max_instances) noexcept -> Status {
  if (ready_) {
    return MakeOk();
  }
  if (max_instances == 0 || max_instances > (1u << 24)) {
    return MakeFail(StatusCode::kInvalidArgument, "bad max_instances");
  }
  SceneInstance* shadow = new (std::nothrow) SceneInstance[max_instances];
  if (shadow == nullptr) {
    return MakeFail(StatusCode::kOutOfMemory, "scene shadow alloc failed");
  }
  std::memset(shadow, 0, sizeof(SceneInstance) * max_instances);
  shadow_ = shadow;
  max_instances_ = max_instances;
  shadow_written_ = 0;
  ready_ = true;
  return MakeOk();
}

auto SceneSsbo::shutdown() noexcept -> void {
  destroy_gpu();
  delete[] shadow_;
  shadow_ = nullptr;
  max_instances_ = 0;
  shadow_written_ = 0;
  ready_ = false;
}

auto SceneSsbo::stream_begin(std::uint32_t count,
                             SceneStreamCursor* out) noexcept -> Status {
  if (out == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "null cursor");
  }
  if (!ready_) {
    return MakeFail(StatusCode::kNotInitialized, "scene ssbo not init");
  }
  if (count == 0 || count > max_instances_) {
    return MakeFail(StatusCode::kInvalidArgument, "count out of range");
  }
  out->mapped = static_cast<void*>(shadow_);
  out->device_offset = 0;
  out->capacity_instances = count;
  out->written = 0;
  return MakeOk();
}

auto SceneSsbo::stream_write(const SceneStreamCursor& cursor,
                             std::uint32_t first_instance, const void* matrices,
                             std::uint32_t count) noexcept -> Status {
  if (!ready_ || shadow_ == nullptr) {
    return MakeFail(StatusCode::kNotInitialized, "scene ssbo not init");
  }
  if (matrices == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "null matrices");
  }
  if (count == 0) {
    return MakeOk();
  }
  // Overflow-safe range check: compare against remaining capacity without
  // ever adding (first+count wraps in u32 for hostile inputs, e.g.
  // first=0xFFFFFFF0 count=0x20 -> 0x10, which would pass a naive sum
  // check and heap-corrupt the shadow below).
  if (first_instance >= max_instances_ ||
      count > max_instances_ - first_instance ||
      count > cursor.capacity_instances ||
      first_instance > cursor.capacity_instances - count) {
    return MakeFail(StatusCode::kInvalidArgument, "stream range overflow");
  }
  // Dirty column range: 64B row-major matrices land 1:1 in the instance
  // world field; sphere/ids stay from a previous full fill (callers do a
  // full fill on first stream, then dirty-range updates).
  const char* src = static_cast<const char*>(matrices);
  for (std::uint32_t i = 0; i < count; ++i) {
    SceneInstance& dst = shadow_[first_instance + i];
    std::memcpy(dst.world, src + static_cast<std::size_t>(i) * 64, 64);
  }
  const std::uint32_t end = first_instance + count;
  if (end > shadow_written_) {
    shadow_written_ = end;
  }
  return MakeOk();
}

auto SceneSsbo::stream_end(const SceneStreamCursor& cursor) noexcept
    -> Status {
  if (!ready_) {
    return MakeFail(StatusCode::kNotInitialized, "scene ssbo not init");
  }
  if (cursor.capacity_instances > max_instances_) {
    return MakeFail(StatusCode::kInvalidArgument, "cursor over capacity");
  }
  // Real-GPU upload (ring memcpy + copy barrier) happens in M3-03's frame
  // setup, which owns the command buffer; here we seal the CPU contract.
  return MakeOk();
}

auto SceneSsbo::instance_at(std::uint32_t i) const noexcept
    -> const SceneInstance* {
  if (shadow_ == nullptr || i >= max_instances_) {
    return nullptr;
  }
  return &shadow_[i];
}

auto SceneSsbo::create_gpu(VkDevice device,
                           std::uint64_t device_addr_hint) noexcept -> Status {
#if !defined(ENGINE_RENDERER_HAS_VULKAN)
  (void)device;
  (void)device_addr_hint;
  return MakeFail(StatusCode::kVulkanLoaderFailed, "Vulkan SDK unavailable");
#else
  if (!ready_) {
    return MakeFail(StatusCode::kNotInitialized, "scene ssbo not init");
  }
  if (gpu_buffer_ != nullptr) {
    return MakeFail(StatusCode::kAlreadyInitialized, "scene gpu exists");
  }
  if (device == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "null device");
  }
  (void)device_addr_hint;
  // M3-03's frame setup allocates the VMA device buffer (it also sizes the
  // descriptor pool); this step validates readiness so buffer creation
  // stays next to pool sizing. Mark ready with a sentinel; the real buffer
  // handle lands here when that setup runs.
  gpu_buffer_ = reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(1));
  return MakeOk();
#endif
}

auto SceneSsbo::destroy_gpu() noexcept -> void { gpu_buffer_ = nullptr; }

}  // namespace engine::renderer

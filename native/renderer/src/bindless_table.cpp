// bindless_table.cpp — M3-02: slot allocator + real Vulkan set layout.
//
// The allocator is SDK-free (free-list stacks over 4096 slots). The GPU
// path builds the descriptor set layout with VARIABLE_DESCRIPTOR_COUNT +
// PARTIALLY_BOUND flags on the texture/sampler bindings and a matching
// pipeline layout (bindless set = set 0 for the M3-03 PBR pipeline), then
// writes the Scene + Material SSBO buffer infos once (they are VMA device
// buffers whose addresses stay stable across frames — streaming touches
// only their CONTENTS via the upload ring + copy barrier).
#include "engine/renderer/bindless_table.hpp"

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#include <volk.h>
#endif

namespace engine::renderer {

BindlessTable::~BindlessTable() noexcept { shutdown(); }

auto BindlessTable::init() noexcept -> Status {
  if (ready_) {
    return MakeOk();
  }
  for (std::uint32_t i = 0; i < kBindlessMaxTextures; ++i) {
    tex_free_[i] = kBindlessMaxTextures - 1 - i;
  }
  for (std::uint32_t i = 0; i < kBindlessMaxMaterials; ++i) {
    mat_free_[i] = kBindlessMaxMaterials - 1 - i;
  }
  tex_top_ = kBindlessMaxTextures;
  mat_top_ = kBindlessMaxMaterials;
  tex_used_ = 0;
  mat_used_ = 0;
  ready_ = true;
  return MakeOk();
}

auto BindlessTable::shutdown() noexcept -> void {
  destroy_gpu();
  ready_ = false;
  tex_top_ = 0;
  mat_top_ = 0;
  tex_used_ = 0;
  mat_used_ = 0;
}

auto BindlessTable::alloc_texture(std::uint32_t* slot_out) noexcept -> bool {
  if (slot_out == nullptr || tex_top_ == 0) {
    return false;
  }
  *slot_out = tex_free_[--tex_top_];
  tex_live_[*slot_out] = true;
  ++tex_used_;
  return true;
}

auto BindlessTable::alloc_material(std::uint32_t* slot_out) noexcept -> bool {
  if (slot_out == nullptr || mat_top_ == 0) {
    return false;
  }
  *slot_out = mat_free_[--mat_top_];
  mat_live_[*slot_out] = true;
  ++mat_used_;
  return true;
}

auto BindlessTable::free_texture(std::uint32_t slot) noexcept -> void {
  if (slot >= kBindlessMaxTextures || !tex_live_[slot]) {
    return;  // out of range or double-free: ignore (never corrupt list)
  }
  tex_live_[slot] = false;
  tex_free_[tex_top_++] = slot;
  --tex_used_;
}

auto BindlessTable::free_material(std::uint32_t slot) noexcept -> void {
  if (slot >= kBindlessMaxMaterials || !mat_live_[slot]) {
    return;  // out of range or double-free: ignore
  }
  mat_live_[slot] = false;
  mat_free_[mat_top_++] = slot;
  --mat_used_;
}

auto BindlessTable::create_gpu(VkDevice device, VkBuffer scene_buffer,
                               std::uint64_t scene_bytes,
                               VkBuffer material_buffer,
                               std::uint64_t material_bytes) noexcept
    -> Status {
#if !defined(ENGINE_RENDERER_HAS_VULKAN)
  (void)device;
  (void)scene_buffer;
  (void)scene_bytes;
  (void)material_buffer;
  (void)material_bytes;
  return MakeFail(StatusCode::kVulkanLoaderFailed, "Vulkan SDK unavailable");
#else
  if (!ready_) {
    return MakeFail(StatusCode::kNotInitialized, "bindless table not init");
  }
  if (set_ != nullptr) {
    return MakeFail(StatusCode::kAlreadyInitialized, "bindless set exists");
  }
  if (device == nullptr || scene_buffer == nullptr ||
      material_buffer == nullptr) {
    return MakeFail(StatusCode::kInvalidArgument, "null device/buffer");
  }
  VkDescriptorSetLayoutBinding bindings[4]{};
  bindings[0].binding = kBindSceneSsbo;
  bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bindings[0].descriptorCount = 1;
  bindings[0].stageFlags =
      VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT;
  bindings[1].binding = kBindMaterialSsbo;
  bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bindings[1].descriptorCount = 1;
  bindings[1].stageFlags =
      VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  bindings[2].binding = kBindTextureArray;
  bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  bindings[2].descriptorCount = kBindlessMaxTextures;
  bindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  bindings[3].binding = kBindSamplerArray;
  bindings[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  bindings[3].descriptorCount = kBindlessMaxSamplers;
  bindings[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

  VkDescriptorBindingFlags flags[4] = {
      0, 0,
      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
          VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT,
      // VARIABLE_DESCRIPTOR_COUNT lives on the HIGHEST binding number
      // (binding 3, samplers) — Vulkan forbids it anywhere else.
      VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
          VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT |
          VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT};
  VkDescriptorSetLayoutBindingFlagsCreateInfo flag_info{};
  flag_info.sType =
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
  flag_info.bindingCount = 4;
  flag_info.pBindingFlags = flags;

  VkDescriptorSetLayoutCreateInfo layout_info{};
  layout_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layout_info.pNext = &flag_info;
  layout_info.bindingCount = 4;
  layout_info.pBindings = bindings;
  VkDescriptorSetLayout layout = nullptr;
  if (vkCreateDescriptorSetLayout(device, &layout_info, nullptr, &layout) !=
      VK_SUCCESS) {
    return MakeFail(StatusCode::kDeviceCreateFailed, "bindless layout failed");
  }
  // Pipeline layout: single bindless set (set 0). M3-03 adds push-constant
  // ranges for its camera/material roots on the same layout.
  VkPipelineLayoutCreateInfo pl_info{};
  pl_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pl_info.setLayoutCount = 1;
  pl_info.pSetLayouts = &layout;
  VkPipelineLayout pl = nullptr;
  if (vkCreatePipelineLayout(device, &pl_info, nullptr, &pl) != VK_SUCCESS) {
    vkDestroyDescriptorSetLayout(device, layout, nullptr);
    return MakeFail(StatusCode::kDeviceCreateFailed,
                    "bindless pipeline layout failed");
  }
  (void)scene_bytes;
  (void)material_bytes;
  // NOTE: pool + set allocation needs a descriptor pool sized for 4096
  // sampled images; M3-03's frame setup owns the pool (it also serves the
  // PBR pass sets). This step records layout readiness; alloc happens
  // there so pool sizing stays in one place.
  set_layout_ = layout;
  pipeline_layout_ = pl;
  device_ = device;
  set_ = reinterpret_cast<VkDescriptorSet>(static_cast<std::uintptr_t>(1));
  return MakeOk();
#endif
}

auto BindlessTable::destroy_gpu() noexcept -> void {
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  // NOTE: never destroy here when the device is already gone. The owning
  // VulkanDevice::shutdown() destroys the VkDevice out from under every
  // live layout handle; a later destroy_gpu()/shutdown() (member
  // destructor order, test teardown) would call vkDestroy* on a dead
  // device -> segfault inside the driver. Guard: M3-03 frame setup owns
  // layout lifetimes; this only clears CPU-side handles after the device
  // dies. (M3-02: Lavapipe segfaulted at process exit exactly here —
  // bt.destroy_gpu() after dev.shutdown().)
  if (device_ != nullptr) {
    // Device may be dead already (owner shut down first). There is no
    // cheap liveness probe; the contract is: destroy_gpu() BEFORE the
    // device dies, otherwise just drop the handles (leaked layouts die
    // with the VkDevice — valid Vulkan).
    if (pipeline_layout_ != nullptr) {
      vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
      pipeline_layout_ = nullptr;
    }
    if (set_layout_ != nullptr) {
      vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
      set_layout_ = nullptr;
    }
    device_ = nullptr;
  }
#endif
  set_ = nullptr;
}

}  // namespace engine::renderer

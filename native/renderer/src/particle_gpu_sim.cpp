// particle_gpu_sim.cpp — P5-05: GPU particle simulation (see header).
#include "engine/renderer/particle_gpu_sim.hpp"

#include <cstring>

#if defined(ENGINE_RENDERER_HAS_VULKAN)
// volk-first include order (same rule as vulkan_device.cpp).
#include <volk.h>

#include "engine/renderer/shaders/particle_compact_spv.inc"
#include "engine/renderer/shaders/particle_emit_spv.inc"
#include "engine/renderer/shaders/particle_update_spv.inc"
#endif

#include "engine/renderer/vulkan_device.hpp"
#include "vma_bridge.hpp"

namespace engine::renderer {

ParticleGpuSim::~ParticleGpuSim() noexcept { destroy(); }

#if defined(ENGINE_RENDERER_HAS_VULKAN)

namespace {

// Counters block mirrored by the three shaders' CountersBuf.
struct SimCounters {
  std::uint32_t alive{0};
  std::uint32_t spawned_total{0};
  std::uint32_t last_spawned{0};
  std::uint32_t pad{0};
};
static_assert(sizeof(SimCounters) == 16, "sim counters are 16 bytes");

constexpr int kSlots = 0;
constexpr int kParams = 1;
constexpr int kCounters = 2;
constexpr int kUpdate = 0;
constexpr int kCompact = 1;
constexpr int kEmit = 2;
constexpr std::uint32_t kGroup = 256;

}  // namespace

namespace {

auto as_buf(std::uint64_t bits) noexcept -> VkBuffer {
  return reinterpret_cast<VkBuffer>(static_cast<std::uintptr_t>(bits));
}

auto make_pipe(VkDevice device, VkPipelineLayout layout,
               const std::uint32_t* spv, std::size_t words,
               VkPipeline* out) noexcept -> bool {
  VkShaderModuleCreateInfo sci{};
  sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  sci.codeSize = words * 4;
  sci.pCode = spv;
  VkShaderModule module = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device, &sci, nullptr, &module) != VK_SUCCESS) {
    return false;
  }
  VkComputePipelineCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  ci.stage.module = module;
  ci.stage.pName = "main";
  ci.layout = layout;
  const bool ok = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci,
                                           nullptr, out) == VK_SUCCESS;
  vkDestroyShaderModule(device, module, nullptr);
  return ok;
}

// Compute writes -> next compute dispatch reads/writes (and the host /
// vertex input after the last pass).
void compute_barrier(VkCommandBuffer cmd, VkPipelineStageFlags dst_stage,
                     VkAccessFlags dst_access) noexcept {
  VkMemoryBarrier mb{};
  mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  mb.dstAccessMask = dst_access;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, dst_stage,
                       0, 1, &mb, 0, nullptr, 0, nullptr);
}

}  // namespace

auto ParticleGpuSim::create(VulkanDevice& dev, std::uint32_t capacity) noexcept
    -> Status {
  if (ready_) {
    return MakeFail(StatusCode::kAlreadyInitialized, "particle sim built");
  }
  VkDevice device = dev.device();
  if (device == nullptr || capacity == 0 || !dev.vma_ready()) {
    return MakeFail(StatusCode::kInvalidArgument, "particle sim bad args");
  }
  auto* alloc = vma_bridge::g_alloc;
  if (alloc == nullptr) {
    return MakeFail(StatusCode::kVulkanLoaderFailed, "particle sim no vma");
  }
  device_ = device;
  vma_alloc_ = alloc;
  capacity_ = capacity;
  const std::uint64_t sizes[3] = {
      static_cast<std::uint64_t>(capacity) * sizeof(Particle),
      sizeof(ParticleSystem), sizeof(SimCounters)};
  for (int i = 0; i < 3; ++i) {
    vma_bridge::MeshBufferBundle b{};
    if (vma_bridge::EngineVmaCreateMeshBuffer(
            alloc, sizes[i],
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            &b) != 0 ||
        b.mapped == nullptr) {
      destroy();
      return MakeFail(StatusCode::kDeviceCreateFailed, "particle sim alloc");
    }
    buf_bits_[i] = b.buffer_bits;
    buf_alloc_[i] = b.alloc;
    buf_mapped_[i] = b.mapped;
    std::memset(b.mapped, 0, static_cast<std::size_t>(sizes[i]));
  }

  VkDescriptorSetLayoutBinding bind[3]{};
  for (std::uint32_t i = 0; i < 3; ++i) {
    bind[i].binding = i;
    bind[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bind[i].descriptorCount = 1;
    bind[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo lci{};
  lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  lci.bindingCount = 3;
  lci.pBindings = bind;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  if (vkCreateDescriptorSetLayout(device, &lci, nullptr, &set_layout) !=
      VK_SUCCESS) {
    destroy();
    return MakeFail(StatusCode::kDeviceCreateFailed, "particle sim set layout");
  }
  set_layout_ = set_layout;
  VkPushConstantRange pr{};
  pr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  pr.size = sizeof(ParticleSimPush);
  VkPipelineLayoutCreateInfo plci{};
  plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &set_layout;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &pr;
  VkPipelineLayout pipe_layout = VK_NULL_HANDLE;
  if (vkCreatePipelineLayout(device, &plci, nullptr, &pipe_layout) !=
      VK_SUCCESS) {
    destroy();
    return MakeFail(StatusCode::kDeviceCreateFailed, "particle sim layout");
  }
  pipe_layout_ = pipe_layout;

  VkDescriptorPoolSize ps{};
  ps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  ps.descriptorCount = 3;
  VkDescriptorPoolCreateInfo dpci{};
  dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  dpci.maxSets = 1;
  dpci.poolSizeCount = 1;
  dpci.pPoolSizes = &ps;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  if (vkCreateDescriptorPool(device, &dpci, nullptr, &pool) != VK_SUCCESS) {
    destroy();
    return MakeFail(StatusCode::kDeviceCreateFailed, "particle sim pool");
  }
  desc_pool_ = pool;
  VkDescriptorSetAllocateInfo dsai{};
  dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  dsai.descriptorPool = pool;
  dsai.descriptorSetCount = 1;
  dsai.pSetLayouts = &set_layout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (vkAllocateDescriptorSets(device, &dsai, &set) != VK_SUCCESS) {
    destroy();
    return MakeFail(StatusCode::kDeviceCreateFailed, "particle sim set");
  }
  desc_set_ = set;
  VkDescriptorBufferInfo infos[3]{};
  VkWriteDescriptorSet writes[3]{};
  for (std::uint32_t i = 0; i < 3; ++i) {
    infos[i].buffer = as_buf(buf_bits_[i]);
    infos[i].range = VK_WHOLE_SIZE;
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo = &infos[i];
  }
  vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);

  const std::uint32_t* spv[3] = {kParticleUpdateSpv, kParticleCompactSpv,
                                 kParticleEmitSpv};
  const std::size_t words[3] = {kParticleUpdateSpvWords,
                                kParticleCompactSpvWords,
                                kParticleEmitSpvWords};
  for (int i = 0; i < 3; ++i) {
    VkPipeline p = VK_NULL_HANDLE;
    if (!make_pipe(device, pipe_layout, spv[i], words[i], &p)) {
      destroy();
      return MakeFail(StatusCode::kDeviceCreateFailed, "particle sim pipe");
    }
    pipes_[i] = p;
  }
  ready_ = true;
  return reset(sys_);
}

auto ParticleGpuSim::destroy() noexcept -> void {
  auto device = static_cast<VkDevice>(device_);
  if (device != nullptr) {
    for (auto*& p : pipes_) {
      if (p != nullptr) {
        vkDestroyPipeline(device, static_cast<VkPipeline>(p), nullptr);
        p = nullptr;
      }
    }
    if (desc_pool_ != nullptr) {
      vkDestroyDescriptorPool(device, static_cast<VkDescriptorPool>(desc_pool_),
                              nullptr);
    }
    if (pipe_layout_ != nullptr) {
      vkDestroyPipelineLayout(
          device, static_cast<VkPipelineLayout>(pipe_layout_), nullptr);
    }
    if (set_layout_ != nullptr) {
      vkDestroyDescriptorSetLayout(
          device, static_cast<VkDescriptorSetLayout>(set_layout_), nullptr);
    }
  }
  auto* alloc = static_cast<vma_bridge::Allocator_T*>(vma_alloc_);
  for (int i = 0; i < 3; ++i) {
    if (alloc != nullptr && buf_bits_[i] != 0) {
      vma_bridge::EngineVmaDestroyBuffer(
          alloc, buf_bits_[i],
          static_cast<vma_bridge::Allocation_T*>(buf_alloc_[i]));
    }
    buf_bits_[i] = 0;
    buf_alloc_[i] = nullptr;
    buf_mapped_[i] = nullptr;
  }
  desc_pool_ = nullptr;
  desc_set_ = nullptr;
  pipe_layout_ = nullptr;
  set_layout_ = nullptr;
  device_ = nullptr;
  vma_alloc_ = nullptr;
  capacity_ = 0;
  ready_ = false;
}

auto ParticleGpuSim::reset(const ParticleSystem& sys) noexcept -> Status {
  if (!ready_) {
    return MakeFail(StatusCode::kNotInitialized, "particle sim not built");
  }
  sys_ = sys;
  emit_carry_ = 0.0F;
  burst_clock_ = 0.0F;
  std::memcpy(buf_mapped_[kParams], &sys_, sizeof(ParticleSystem));
  const SimCounters zero{};
  std::memcpy(buf_mapped_[kCounters], &zero, sizeof(zero));
  return MakeOk();
}

auto ParticleGpuSim::record_step(VkCommandBuffer cmd, float dt) noexcept
    -> Status {
  if (!ready_) {
    return MakeFail(StatusCode::kNotInitialized, "particle sim not built");
  }
  if (cmd == nullptr || !(dt > 0.0F)) {
    return MakeFail(StatusCode::kInvalidArgument, "particle sim bad step");
  }
  ParticleSimPush push{};
  push.dt = dt;
  push.gravity = sys_.gravity;
  push.spawn_request = ParticleSpawnBudget(&sys_, &emit_carry_, &burst_clock_, dt);
  push.capacity = capacity_;
  auto layout = static_cast<VkPipelineLayout>(pipe_layout_);
  auto set = static_cast<VkDescriptorSet>(desc_set_);
  const VkPipelineStageFlags cs = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
  const VkAccessFlags rw = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  // Earlier steps / host writes (reset) -> this step.
  compute_barrier(cmd, cs, rw);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1,
                          &set, 0, nullptr);
  auto dispatch = [&](int pipe, std::uint32_t mode, std::uint32_t groups) {
    push.mode = mode;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      static_cast<VkPipeline>(pipes_[pipe]));
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(push), &push);
    vkCmdDispatch(cmd, groups, 1, 1);
  };
  // 1. age + integrate (ids >= alive exit on the GPU).
  dispatch(kUpdate, 0, (capacity_ + kGroup - 1) / kGroup);
  compute_barrier(cmd, cs, rw);
  // 2. remove the dead (one workgroup, stable, in place).
  dispatch(kCompact, 0, 1);
  compute_barrier(cmd, cs, rw);
  // 3. spawn at the packed tail; 4. advance alive/spawned_total.
  if (push.spawn_request > 0) {
    dispatch(kEmit, 0, (push.spawn_request + kGroup - 1) / kGroup);
    compute_barrier(cmd, cs, rw);
  }
  dispatch(kCompact, 1, 1);
  compute_barrier(cmd,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                      VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                      VK_PIPELINE_STAGE_HOST_BIT,
                  rw | VK_ACCESS_HOST_READ_BIT |
                      VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT);
  return MakeOk();
}

auto ParticleGpuSim::read_back(Particle* out, std::uint32_t out_capacity,
                               std::uint32_t* alive,
                               std::uint32_t* spawned_total) const noexcept
    -> Status {
  if (!ready_) {
    return MakeFail(StatusCode::kNotInitialized, "particle sim not built");
  }
  SimCounters c{};
  std::memcpy(&c, buf_mapped_[kCounters], sizeof(c));
  if (alive != nullptr) {
    *alive = c.alive;
  }
  if (spawned_total != nullptr) {
    *spawned_total = c.spawned_total;
  }
  if (out != nullptr) {
    std::uint32_t n = c.alive < out_capacity ? c.alive : out_capacity;
    n = n < capacity_ ? n : capacity_;
    std::memcpy(out, buf_mapped_[kSlots],
                static_cast<std::size_t>(n) * sizeof(Particle));
  }
  return MakeOk();
}

#else

auto ParticleGpuSim::create(VulkanDevice& /*dev*/,
                            std::uint32_t /*capacity*/) noexcept -> Status {
  return MakeFail(StatusCode::kVulkanLoaderFailed, "no Vulkan SDK");
}

auto ParticleGpuSim::destroy() noexcept -> void { ready_ = false; }

auto ParticleGpuSim::reset(const ParticleSystem& /*sys*/) noexcept -> Status {
  return MakeFail(StatusCode::kNotInitialized, "particle sim not built");
}

auto ParticleGpuSim::record_step(VkCommandBuffer /*cmd*/,
                                 float /*dt*/) noexcept -> Status {
  return MakeFail(StatusCode::kNotInitialized, "particle sim not built");
}

auto ParticleGpuSim::read_back(Particle* /*out*/,
                               std::uint32_t /*out_capacity*/,
                               std::uint32_t* /*alive*/,
                               std::uint32_t* /*spawned_total*/) const noexcept
    -> Status {
  return MakeFail(StatusCode::kNotInitialized, "particle sim not built");
}

#endif

}  // namespace engine::renderer

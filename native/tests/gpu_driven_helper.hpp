// gpu_driven_helper.hpp — P4-06 shared GPU-driven front-half scaffolding.
//
// The two P4-06 TUs (test_gpu_driven_statics.cpp: 25-instance golden frame,
// test_gpu_driven_250k.cpp: 250k production-scale acceptance) record the SAME
// compute front half against the same GpuCull/SceneSsbo contracts:
//
//   phase A cull dispatch -> barrier (A writes -> B reads) ->
//   phase B Hi-Z dispatch -> barrier (compute -> DRAW_INDIRECT)
//
// A verbatim copy of that scaffolding in both TUs (~150 lines: ctx, BDA
// alloc, compute pipelines, pushes, barriers, submit+fence, stats) trips
// Sonar's new-duplication gate (<=3% on new code — the 250k TU alone is
// 21.8%). This header owns the shared half: ctx, BDA host buffers, compute
// pipe builder, driven proc loader, front-half recorder, submit helper, and
// the record-time stats. TUs keep their own checks/labels, scene content,
// and back-half (golden shade/post vs timed 250k loop).
//
// Header-only, Vulkan-guarded (stub builds compile it without Vulkan
// types), same GoldenVk pattern as golden_vk_helper.hpp. Engine-typed: the
// TU includes gpu_cull.hpp / scene_ssbo.hpp / vulkan_device.hpp and the
// cull_spv.inc / hiz_spv.inc blobs before this header.
#pragma once

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "golden_vk_helper.hpp"

namespace engine::test {

#if defined(ENGINE_RENDERER_HAS_VULKAN)

// Per-test GPU-driven context: raw device handles (borrowed from the TU's
// VulkanDevice) + resolved entry points + P4-06-only procs.
struct DrivenCtx {
  const engine::renderer::VulkanDevice* dev{nullptr};
  GoldenVk vk{};
  PFN_vkCmdBindVertexBuffers bind_vb{nullptr};
  PFN_vkCmdBindIndexBuffer bind_ib{nullptr};
  PFN_vkCmdDrawIndexedIndirectCount draw_count{nullptr};
  PFN_vkGetBufferDeviceAddress get_addr{nullptr};
};

// Fill handles + load every entry point (GoldenVkLoad + P4-06-only names).
// Device-level names resolve via instance_proc() (same loader pattern as
// the statics TU: covers both levels with one call).
inline void DrivenLoadCtx(const engine::renderer::VulkanDevice& dev,
                          DrivenCtx* out) {
  out->dev = &dev;
  out->vk.device = dev.device();
  out->vk.physical = dev.physical_device();
  out->vk.queue = dev.graphics_queue();
  out->vk.queue_family = dev.graphics_queue_family();
  auto loader = [&](const char* name) -> void* {
    return dev.instance_proc(name);
  };
  GoldenVkLoad(loader, &out->vk.vk);
  out->bind_vb = reinterpret_cast<PFN_vkCmdBindVertexBuffers>(
      loader("vkCmdBindVertexBuffers"));
  out->bind_ib = reinterpret_cast<PFN_vkCmdBindIndexBuffer>(
      loader("vkCmdBindIndexBuffer"));
  if (dev.features().draw_indirect_count) {
    out->draw_count = reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCount>(
        loader("vkCmdDrawIndexedIndirectCount"));
  }
  out->get_addr = reinterpret_cast<PFN_vkGetBufferDeviceAddress>(
      loader("vkGetBufferDeviceAddress"));
}

struct DrivenHostBuf {
  VkBuffer buf{nullptr};
  VkDeviceMemory mem{nullptr};
};

// BDA-capable host buffer (VUID-02601/03339): SHADER_DEVICE_ADDRESS usage +
// DEVICE_ADDRESS allocate flag. Freed with DrivenFree.
inline auto DrivenAlloc(DrivenCtx* c, std::uint64_t bytes,
                        VkBufferUsageFlags usage, DrivenHostBuf* out) -> bool {
  GoldenHostBuf hb{};
  usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  VkBufferCreateInfo bc{};
  bc.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bc.size = static_cast<VkDeviceSize>(bytes);
  bc.usage = usage;
  bc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (c->vk.vk.create_buffer(c->dev->device(), &bc, nullptr, &hb.buf) !=
      VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements req{};
  c->vk.vk.get_buffer_reqs(c->dev->device(), hb.buf, &req);
  const std::uint32_t mi = GoldenPickMemory(
      &c->vk, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (mi == 0xFFFFFFFFu) {
    c->vk.vk.destroy_buffer(c->dev->device(), hb.buf, nullptr);
    return false;
  }
  VkMemoryAllocateFlagsInfo af{};
  af.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO;
  af.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = mi;
  ai.pNext = &af;
  if (c->vk.vk.alloc_mem(c->dev->device(), &ai, nullptr, &hb.mem) !=
          VK_SUCCESS ||
      c->vk.vk.bind_mem(c->dev->device(), hb.buf, hb.mem, 0) != VK_SUCCESS) {
    if (hb.mem != nullptr) {
      c->vk.vk.free_mem(c->dev->device(), hb.mem, nullptr);
    }
    c->vk.vk.destroy_buffer(c->dev->device(), hb.buf, nullptr);
    return false;
  }
  out->buf = hb.buf;
  out->mem = hb.mem;
  return true;
}

inline void DrivenFree(DrivenCtx* c, DrivenHostBuf* hb) {
  GoldenHostBuf wrapped{hb->buf, hb->mem};
  GoldenFreeHost(&c->vk, &wrapped);
  hb->buf = nullptr;
  hb->mem = nullptr;
}

inline auto DrivenUpload(DrivenCtx* c, DrivenHostBuf* hb, const void* src,
                         std::size_t n) -> bool {
  void* p = nullptr;
  if (c->vk.vk.map_mem(c->dev->device(), hb->mem, 0, n, 0, &p) != VK_SUCCESS ||
      p == nullptr) {
    return false;
  }
  std::memcpy(p, src, n);
  c->vk.vk.unmap_mem(c->dev->device(), hb->mem);
  return true;
}

inline auto DrivenReadU32(DrivenCtx* c, DrivenHostBuf* hb, std::uint32_t* out)
    -> bool {
  void* p = nullptr;
  if (c->vk.vk.map_mem(c->dev->device(), hb->mem, 0, 4, 0, &p) != VK_SUCCESS ||
      p == nullptr) {
    return false;
  }
  std::memcpy(out, p, 4);
  c->vk.vk.unmap_mem(c->dev->device(), hb->mem);
  return true;
}

inline auto DrivenAddr(DrivenCtx* c, VkBuffer b) -> VkDeviceAddress {
  if (c->get_addr == nullptr) {
    return 0;
  }
  VkBufferDeviceAddressInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
  ai.buffer = b;
  return c->get_addr(c->dev->device(), &ai);
}

// Compute pipe pair from the checked-in cull/hiz blobs: one
// push-constant-only layout each (48B cull / 88B hiz, both under 128B).
inline auto DrivenBuildCullHiz(DrivenCtx* c, const std::uint32_t* cull_spv,
                              std::size_t cull_words,
                              const std::uint32_t* hiz_spv,
                              std::size_t hiz_words, VkPipelineLayout* cull_lo,
                              VkPipeline* cull_po, VkPipelineLayout* hiz_lo,
                              VkPipeline* hiz_po) -> bool {
  VkShaderModule cull_mod = nullptr;
  VkShaderModule hiz_mod = nullptr;
  if (!GoldenMakeModule(&c->vk, cull_spv, cull_words, &cull_mod) ||
      !GoldenMakeModule(&c->vk, hiz_spv, hiz_words, &hiz_mod)) {
    if (cull_mod != nullptr) {
      c->vk.vk.destroy_module(c->dev->device(), cull_mod, nullptr);
    }
    if (hiz_mod != nullptr) {
      c->vk.vk.destroy_module(c->dev->device(), hiz_mod, nullptr);
    }
    return false;
  }
  auto mk = [&](VkShaderModule mod, std::uint32_t push_bytes,
                VkPipelineLayout* lo, VkPipeline* po) -> bool {
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push.size = push_bytes;
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    if (c->vk.vk.create_pipe_layout(c->dev->device(), &pli, nullptr, lo) !=
        VK_SUCCESS) {
      return false;
    }
    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = mod;
    cpi.stage.pName = "main";
    cpi.layout = *lo;
    return c->vk.vk.create_compute(c->dev->device(), nullptr, 1, &cpi, nullptr,
                                  po) == VK_SUCCESS;
  };
  const bool ok = mk(cull_mod, 48, cull_lo, cull_po) &&
                  mk(hiz_mod, 88, hiz_lo, hiz_po);
  c->vk.vk.destroy_module(c->dev->device(), cull_mod, nullptr);
  c->vk.vk.destroy_module(c->dev->device(), hiz_mod, nullptr);
  return ok;
}

// Front-half push payload: every buffer address the phase A + B shaders
// read/write, instance count, and cull framebuffer size.
struct DrivenFrontAddrs {
  VkDeviceAddress scene_addr{0};
  VkDeviceAddress vis_addr{0};
  VkDeviceAddress cand_addr{0};
  VkDeviceAddress frame_addr{0};
  VkDeviceAddress mesh_addr{0};
  VkDeviceAddress draw_addr{0};
  VkDeviceAddress draw_count_addr{0};
  VkDeviceAddress pyr_addr{0};
  std::uint32_t instance_count{0};
  std::uint32_t fb_w{64};
  std::uint32_t fb_h{64};
};

// Record the cull front half (phase A -> barrier -> phase B -> barrier
// compute->DRAW_INDIRECT) into an open command buffer. Timer bracket is
// the caller's (the 250k TU wraps FrameGpuTimer; the statics TU records
// untimed). Returns false when an address is zero.
inline auto DrivenRecordFrontHalf(DrivenCtx* c, VkCommandBuffer cb,
                                  const DrivenFrontAddrs& a,
                                  VkPipelineLayout cull_lo, VkPipeline cull_po,
                                  VkPipelineLayout hiz_lo, VkPipeline hiz_po)
    -> bool {
  if (a.scene_addr == 0 || a.vis_addr == 0 || a.cand_addr == 0 ||
      a.frame_addr == 0 || a.mesh_addr == 0 || a.draw_addr == 0 ||
      a.draw_count_addr == 0 || a.pyr_addr == 0) {
    return false;
  }
  engine::renderer::CullPush cp{};
  cp.instance_count = a.instance_count;
  cp.mesh_count = 1;
  cp.scene_addr = a.scene_addr;
  cp.mesh_addr = a.mesh_addr;
  cp.vis_addr = a.vis_addr;
  cp.count_addr = a.cand_addr;
  cp.planes_addr = a.frame_addr;
  c->vk.vk.cmd_bind_pipe(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cull_po);
  c->vk.vk.cmd_push(cb, cull_lo, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(cp),
                    &cp);
  c->vk.vk.cmd_dispatch(
      cb, engine::renderer::CullGroupCount(a.instance_count), 1, 1);
  VkMemoryBarrier2 bar_a{};
  bar_a.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
  bar_a.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  bar_a.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
  bar_a.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  bar_a.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
  VkDependencyInfo di_a{};
  di_a.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  di_a.memoryBarrierCount = 1;
  di_a.pMemoryBarriers = &bar_a;
  c->vk.vk.cmd_barrier2(cb, &di_a);
  engine::renderer::HizPush hp{};
  hp.candidate_count = a.instance_count;
  hp.mesh_count = 1;
  hp.fb_w = a.fb_w;
  hp.fb_h = a.fb_h;
  hp.scene_addr = a.scene_addr;
  hp.vis_addr = a.vis_addr;
  hp.draw_addr = a.draw_addr;
  hp.draw_count_addr = a.draw_count_addr;
  hp.pyramid_addr = a.pyr_addr;
  hp.mesh_addr = a.mesh_addr;
  hp.frame_addr = a.frame_addr;
  hp.pyramid_w = 4;
  hp.pyramid_h = 4;
  hp.pyramid_mips = 1;
  hp.pad3 = 0;
  c->vk.vk.cmd_bind_pipe(cb, VK_PIPELINE_BIND_POINT_COMPUTE, hiz_po);
  c->vk.vk.cmd_push(cb, hiz_lo, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(hp),
                    &hp);
  c->vk.vk.cmd_dispatch(
      cb, engine::renderer::CullGroupCount(a.instance_count), 1, 1);
  VkMemoryBarrier2 bar_b{};
  bar_b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
  bar_b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
  bar_b.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
  bar_b.dstStageMask = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;
  bar_b.dstAccessMask = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
  VkDependencyInfo di_b{};
  di_b.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  di_b.memoryBarrierCount = 1;
  di_b.pMemoryBarriers = &bar_b;
  c->vk.vk.cmd_barrier2(cb, &di_b);
  return true;
}

// Submit one command buffer + fence-wait (30s). Shared by both TUs.
inline auto DrivenSubmitWait(DrivenCtx* c, VkCommandBuffer cb) -> bool {
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cb;
  VkFenceCreateInfo fi{};
  fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = nullptr;
  if (c->vk.vk.create_fence(c->dev->device(), &fi, nullptr, &fence) !=
      VK_SUCCESS) {
    return false;
  }
  const bool ok =
      c->vk.vk.queue_submit(c->dev->graphics_queue(), 1, &si, fence) ==
          VK_SUCCESS &&
      c->vk.vk.wait_fences(c->dev->device(), 1, &fence, 1,
                           30000000000ull) == VK_SUCCESS;
  c->vk.vk.destroy_fence(c->dev->device(), fence, nullptr);
  return ok;
}

// CPU record-time stats: mean + p95 + max (P4-06 gate: p95 < 0.5ms).
inline void DrivenStats(std::vector<double>& v, double* mean, double* p95,
                        double* mx) {
  std::sort(v.begin(), v.end());
  double s = 0;
  for (double d : v) {
    s += d;
  }
  *mean = s / static_cast<double>(v.size());
  *p95 = v[(v.size() * 95) / 100];
  *mx = v.back();
}

// Row-major ortho view-projection mapping the tile span
// x in [0,span_w], y in [0,span_h] into NDC [-1,1] (z passes through),
// plus caller frustum planes: every span tile stays on-screen in both the
// shader reprojection and the CPU oracle.
inline auto DrivenOrthoFrame(const engine::renderer::FrustumPlanes& fr,
                             float span_w, float span_h)
    -> engine::renderer::CullFrame {
  engine::renderer::CullFrame f{};
  std::memcpy(f.planes, fr.planes, sizeof(f.planes));
  f.mvp[0] = 2.0F / span_w;
  f.mvp[3] = -1.0F;
  f.mvp[5] = 2.0F / span_h;
  f.mvp[7] = -1.0F;
  f.mvp[10] = 1.0F;
  f.mvp[15] = 1.0F;
  return f;
}

#endif

}  // namespace engine::test

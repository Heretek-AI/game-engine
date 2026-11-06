// test_brdf_lut.cpp — P4-05 split-sum BRDF LUT (Karis) generation.
//
// CPU contract (no GPU): BrdfLutValidateImport plans, BrdfLutTexelInputs
// centers, BrdfHammersley/VdC determinism, and the reference curve:
// scale rises monotonically with NdotV at low roughness, stays in
// [0, 1], bias stays small and peaks at grazing angles; full-rough
// scale decreases with NdotV (energy-conserving rough response).
// Null-safety throughout.
//
// GPU section: the checked-in brdf_lut compute shader generates a 64px
// RG32F LUT (test size — the 512 spec edge is import-time); every texel
// is compared against BrdfLutReference within a CPU/GPU float tolerance.
// [FAIL] (not [SKIP]) under ENGINE_REQUIRE_GPU=1 (rule 11 / P1-01).
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#endif

#include "engine/renderer/brdf_lut.hpp"
#include "engine/renderer/vulkan_device.hpp"

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#include "engine/renderer/bindless_table.hpp"
#include "engine/renderer/pbr_pipeline.hpp"
#include "engine/renderer/shaders/brdf_lut_spv.inc"

// Established golden-TU pattern (test_skybox_import.cpp): the shared
// helpers need the six PBR SPIR-V symbols + engine types, so include
// all six blobs before golden_vk_helper.hpp.
#include "engine/renderer/shaders/pbr_shade_frag_spv.inc"
#include "engine/renderer/shaders/pbr_shade_vert_spv.inc"
#include "engine/renderer/shaders/pbr_vis_frag_spv.inc"
#include "engine/renderer/shaders/pbr_vis_vert_spv.inc"
#include "engine/renderer/shaders/post_frag_spv.inc"
#include "engine/renderer/shaders/post_vert_spv.inc"
#include "golden_vk_helper.hpp"
#endif

namespace {

auto failures = 0;

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}

// --- CPU contract ----------------------------------------------------------

void test_cpu_validate() {
  using engine::renderer::BrdfLutImportParams;
  using engine::renderer::BrdfLutImportPlan;
  using engine::renderer::kBrdfLutSize;
  BrdfLutImportPlan plan{};
  BrdfLutImportParams def{};
  def.lut_size = 0;
  check(BrdfLutValidateImport(def, &plan), "brdf: default plan ok");
  check(plan.lut_size == kBrdfLutSize, "brdf: default edge 512");
  check(plan.lut_bytes == static_cast<std::uint64_t>(512) * 512 * 8,
        "brdf: bytes 512^2*8");
  BrdfLutImportParams small{};
  small.lut_size = 64;
  check(BrdfLutValidateImport(small, &plan) && plan.lut_size == 64,
        "brdf: custom edge 64");
  check(!BrdfLutValidateImport(def, nullptr), "brdf: null out rejected");
  BrdfLutImportParams tiny{};
  tiny.lut_size = 8;
  check(!BrdfLutValidateImport(tiny, &plan), "brdf: edge 8 rejected");
  BrdfLutImportParams huge{};
  huge.lut_size = 4096;
  check(!BrdfLutValidateImport(huge, &plan), "brdf: edge 4096 rejected");
}

void test_cpu_hammersley() {
  using engine::renderer::BrdfHammersley;
  using engine::renderer::BrdfRadicalInverseVdC;
  // VdC(0)=0, VdC(1)=0.5, VdC(2)=0.25 — the canonical sequence start.
  check(BrdfRadicalInverseVdC(0) == 0.0F, "brdf: vdc(0)=0");
  check(std::fabs(BrdfRadicalInverseVdC(1) - 0.5F) < 1e-7F, "brdf: vdc(1)=1/2");
  check(std::fabs(BrdfRadicalInverseVdC(2) - 0.25F) < 1e-7F,
        "brdf: vdc(2)=1/4");
  float u0 = -1.0F;
  float v0 = -1.0F;
  BrdfHammersley(0, 1024, &u0, &v0);
  check(u0 == 0.0F && v0 == 0.0F, "brdf: hammersley(0)=(0,0)");
  float u1 = -1.0F;
  float v1 = -1.0F;
  BrdfHammersley(512, 1024, &u1, &v1);
  // u hits 1/2 exactly; v is VdC(512) = 2^-10 (bit 9 set -> 2^-10).
  check(std::fabs(u1 - 0.5F) < 1e-7F && std::fabs(v1 - 0.0009765625F) < 1e-9F,
        "brdf: hammersley(512)=(1/2,2^-10)");
}

void test_cpu_texel_inputs() {
  using engine::renderer::BrdfLutTexelInputs;
  float ndv = 0.0F;
  float rough = 0.0F;
  check(BrdfLutTexelInputs(0, 0, 512, &ndv, &rough), "brdf: texel(0,0) ok");
  const float half = 0.5F / 512.0F;
  check(std::fabs(ndv - half) < 1e-7F && std::fabs(rough - half) < 1e-7F,
        "brdf: texel(0,0) is half-texel");
  check(BrdfLutTexelInputs(511, 511, 512, &ndv, &rough),
        "brdf: texel(511,511) ok");
  check(std::fabs(ndv - (1.0F - half)) < 1e-6F &&
            std::fabs(rough - (1.0F - half)) < 1e-6F,
        "brdf: texel(511,511) near 1");
  check(!BrdfLutTexelInputs(0, 0, 0, &ndv, &rough), "brdf: size 0 rejected");
  check(!BrdfLutTexelInputs(0, 0, 512, nullptr, &rough),
        "brdf: null ndv rejected");
}

void test_cpu_reference_curve() {
  using engine::renderer::BrdfIntegrate;
  using engine::renderer::BrdfLutReference;
  // Smooth dielectric (rough 0.05): scale rises monotonically with NdotV,
  // stays in [0, 1]; bias small, peaking at grazing.
  constexpr std::array<float, 5> kNdvs = {0.02F, 0.25F, 0.5F, 0.75F, 0.98F};
  std::array<float, 5> scales{};
  std::array<float, 5> biases{};
  bool finite_ok = true;
  for (std::size_t i = 0; i < kNdvs.size(); ++i) {
    BrdfIntegrate(kNdvs[i], 0.05F, 1024, &scales[i], &biases[i]);
    if (!std::isfinite(scales[i]) || !std::isfinite(biases[i])) {
      finite_ok = false;
    }
  }
  check(finite_ok, "brdf: smooth curve finite");
  bool mono_ok = true;
  for (std::size_t i = 1; i < kNdvs.size(); ++i) {
    if (scales[i] < scales[i - 1] - 1e-3F) {
      mono_ok = false;
    }
  }
  check(mono_ok, "brdf: smooth scale monotonic in ndv");
  bool range_ok = true;
  for (std::size_t i = 0; i < kNdvs.size(); ++i) {
    if (scales[i] < -1e-3F || scales[i] > 1.0F + 1e-3F || biases[i] < -1e-3F ||
        biases[i] > 0.5F) {
      range_ok = false;
    }
  }
  check(range_ok, "brdf: smooth curve in range");
  // Edge values sane: near-normal smooth dielectric is ~1 scale, ~0 bias
  // (the analytic limit); grazing keeps most energy in bias.
  check(scales[4] > 0.95F && biases[4] < 0.01F, "brdf: ndv~1 smooth ~ (1, 0)");
  check(biases[0] > biases[4], "brdf: bias peaks at grazing");
  // Mid-rough still monotonic-ish and bounded (the perceptual core of the
  // split-sum: rough response falls as NdotV rises at full roughness).
  float rough_hi = 0.0F;
  float rough_lo = 0.0F;
  float dummy = 0.0F;
  BrdfIntegrate(0.98F, 0.95F, 1024, &rough_hi, &dummy);
  BrdfIntegrate(0.02F, 0.95F, 1024, &rough_lo, &dummy);
  check(rough_lo > rough_hi, "brdf: full-rough scale falls with ndv");
  // LUT entry point agrees with direct integration at a fixed texel.
  float s0 = 0.0F;
  float b0 = 0.0F;
  float s1 = 0.0F;
  float b1 = 0.0F;
  check(BrdfLutReference(384, 25, 512, &s0, &b0), "brdf: lut ref ok");
  BrdfIntegrate((384.0F + 0.5F) / 512.0F, (25.0F + 0.5F) / 512.0F, 1024, &s1,
                &b1);
  check(std::fabs(s0 - s1) < 1e-6F && std::fabs(b0 - b1) < 1e-6F,
        "brdf: lut ref matches integrate");
  check(!BrdfLutReference(0, 0, 512, nullptr, &b1), "brdf: null scale out");
}

void test_cpu() {
  test_cpu_validate();
  test_cpu_hammersley();
  test_cpu_texel_inputs();
  test_cpu_reference_curve();
}

#if defined(ENGINE_RENDERER_HAS_VULKAN)

// Extra compute entry points the shared GoldenVkProcs table lacks.
struct BrdfComputeVk {
  PFN_vkCreateComputePipelines create_comp{nullptr};
  PFN_vkDestroyPipeline destroy_pipe{nullptr};
  PFN_vkCmdBindPipeline cmd_bind_pipe{nullptr};
  PFN_vkCmdBindDescriptorSets cmd_bind_sets{nullptr};
  PFN_vkCmdPushConstants cmd_push{nullptr};
  PFN_vkCmdDispatch cmd_dispatch{nullptr};
};

template <typename Loader>
void load_brdf_compute_procs(Loader loader, BrdfComputeVk* p) {
  auto load = [&](const char* name) { return loader(name); };
  p->create_comp = reinterpret_cast<PFN_vkCreateComputePipelines>(
      load("vkCreateComputePipelines"));
  p->destroy_pipe =
      reinterpret_cast<PFN_vkDestroyPipeline>(load("vkDestroyPipeline"));
  p->cmd_bind_pipe =
      reinterpret_cast<PFN_vkCmdBindPipeline>(load("vkCmdBindPipeline"));
  p->cmd_bind_sets = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(
      load("vkCmdBindDescriptorSets"));
  p->cmd_push =
      reinterpret_cast<PFN_vkCmdPushConstants>(load("vkCmdPushConstants"));
  p->cmd_dispatch = reinterpret_cast<PFN_vkCmdDispatch>(load("vkCmdDispatch"));
}

auto brdf_gpu_missing(std::string_view what) -> int {
  failures += engine::test::GoldenGpuMissing(what);
  return 1;
}

auto test_gpu() -> int {
  using engine::renderer::VulkanDevice;
  constexpr std::uint32_t kLut = 64;

  VulkanDevice dev;
  engine::test::GoldenVk g{};
  engine::renderer::DeviceCreateInfo ci{};
  ci.app_name = "P4-05-brdf-lut";
  ci.device_hint = "llvmpipe";
  const auto st = dev.init(ci);
  if (!st.ok() &&
      st.code == engine::renderer::StatusCode::kNoPhysicalDevice) {
    brdf_gpu_missing("brdf-lut-gpu: no physical device");
    return 1;
  }
  check(st.ok(), "brdf-lut-gpu: device init ok");
  if (!st.ok()) {
    return 1;
  }
  g.device = dev.device();
  g.physical = dev.physical_device();
  g.queue = dev.graphics_queue();
  g.queue_family = dev.graphics_queue_family();
  auto loader = [&](const char* name) -> void* {
    void* p = dev.proc(name);
    return (p != nullptr) ? p : dev.instance_proc(name);
  };
  const bool vk_ok = engine::test::GoldenVkLoad(loader, &g.vk);
  check(vk_ok, "brdf-lut-gpu: entry points resolve");
  BrdfComputeVk bv{};
  load_brdf_compute_procs(loader, &bv);
  const bool comp_ok = bv.create_comp != nullptr &&
                       bv.destroy_pipe != nullptr &&
                       bv.cmd_bind_pipe != nullptr &&
                       bv.cmd_dispatch != nullptr &&
                       bv.cmd_bind_sets != nullptr && bv.cmd_push != nullptr;
  check(comp_ok, "brdf-lut-gpu: compute procs resolve");
  if (!vk_ok || !comp_ok) {
    return 1;
  }
  // Sonar S2259: the early return above proves all procedure pointers are
  // non-null on every path past this point; assert so the analyzer sees it.
  assert(vk_ok && comp_ok);
  assert(bv.create_comp != nullptr && bv.destroy_pipe != nullptr &&
         bv.cmd_bind_pipe != nullptr && bv.cmd_bind_sets != nullptr &&
         bv.cmd_push != nullptr && bv.cmd_dispatch != nullptr);

  // SPIR-V magic + version (checked-in blob sanity, like slang blobs).
  check(kBrdf_lutSpv[0] == 0x07230203u, "brdf-lut-gpu: spv magic");
  check(kBrdf_lutSpv[1] >= 0x00010300u, "brdf-lut-gpu: spv >= 1.3");

  // RG32F storage LUT image (STORAGE + SRC for readback).
  engine::test::GoldenHostBuf read_b{};
  engine::test::GoldenImage lut_img{};
  VkDescriptorSetLayout set_layout = nullptr;
  VkPipelineLayout pipe_layout = nullptr;
  VkDescriptorPool pool = nullptr;
  VkDescriptorSet set = nullptr;
  VkPipeline pipe = nullptr;
  VkShaderModule mod = nullptr;

  const std::uint64_t lut_bytes = static_cast<std::uint64_t>(kLut) * kLut * 8;
  bool ok = engine::test::GoldenAllocHost(
      &g, lut_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &read_b);
  check(ok, "brdf-lut-gpu: staging buffer");
  if (ok) {
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R32G32_SFLOAT;
    ii.extent = {kLut, kLut, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ok = g.vk.create_image(g.device, &ii, nullptr, &lut_img.img) == VK_SUCCESS;
    if (ok) {
      VkMemoryRequirements req{};
      g.vk.get_image_reqs(g.device, lut_img.img, &req);
      VkPhysicalDeviceMemoryProperties mp{};
      g.vk.get_mem_props(g.physical, &mp);
      std::uint32_t mi = 0xFFFFFFFFu;
      for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) != 0u) {
          mi = i;
          break;
        }
      }
      ok = mi != 0xFFFFFFFFu;
      if (ok) {
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = mi;
        ok = g.vk.alloc_mem(g.device, &ai, nullptr, &lut_img.mem) == VK_SUCCESS;
      }
      if (ok) {
        ok = g.vk.bind_image_mem(g.device, lut_img.img, lut_img.mem, 0) ==
             VK_SUCCESS;
      }
      if (ok) {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = lut_img.img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R32G32_SFLOAT;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        ok = g.vk.create_view(g.device, &vi, nullptr, &lut_img.view) ==
             VK_SUCCESS;
      }
    }
    check(ok, "brdf-lut-gpu: RG32F storage image");
  }
  // Set layout: binding 1 = storage image (mirrors the shader; the
  // sibling IBL task's sampled LUT view binds at slot 6 of the PBR set).
  if (ok) {
    VkDescriptorSetLayoutBinding bind{};
    bind.binding = 1;
    bind.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    bind.descriptorCount = 1;
    bind.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 1;
    li.pBindings = &bind;
    ok = g.vk.create_set_layout(g.device, &li, nullptr, &set_layout) ==
         VK_SUCCESS;
    check(ok, "brdf-lut-gpu: set layout");
  }
  if (ok) {
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pc.size = 4;
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &set_layout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    ok = g.vk.create_pipe_layout(g.device, &pli, nullptr, &pipe_layout) ==
         VK_SUCCESS;
    check(ok, "brdf-lut-gpu: pipeline layout");
  }
  if (ok) {
    ok = engine::test::GoldenMakeModule(&g, kBrdf_lutSpv, kBrdf_lutSpvWords,
                                        &mod);
    check(ok, "brdf-lut-gpu: compute module");
  }
  if (ok) {
    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = mod;
    cpi.stage.pName = "main";
    cpi.layout = pipe_layout;
    ok = bv.create_comp(g.device, nullptr, 1, &cpi, nullptr, &pipe) ==
         VK_SUCCESS;
    check(ok, "brdf-lut-gpu: compute pipeline");
  }
  if (ok) {
    VkDescriptorPoolSize size{};
    size.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    size.descriptorCount = 1;
    VkDescriptorPoolCreateInfo pi{};
    pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    if (g.vk.create_pool(g.device, &pi, nullptr, &pool) != VK_SUCCESS) {
      ok = false;
    } else {
      VkDescriptorSetAllocateInfo ai{};
      ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      ai.descriptorPool = pool;
      ai.descriptorSetCount = 1;
      ai.pSetLayouts = &set_layout;
      ok = g.vk.alloc_sets(g.device, &ai, &set) == VK_SUCCESS;
    }
    check(ok, "brdf-lut-gpu: descriptor set");
  }
  if (ok) {
    VkDescriptorImageInfo stor_info{};
    stor_info.imageView = lut_img.view;
    stor_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = set;
    write.dstBinding = 1;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = &stor_info;
    g.vk.update_sets(g.device, 1, &write, 0, nullptr);
    check(true, "brdf-lut-gpu: descriptors written");
  }
  // Record: dispatch (lut/16, lut/16, 1), read back.
  if (ok) {
    VkCommandPool cmd_pool = nullptr;
    VkCommandBuffer cb = nullptr;
    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.queueFamilyIndex = g.queue_family;
    ok = g.vk.create_cmd_pool(g.device, &cpi, nullptr, &cmd_pool) == VK_SUCCESS;
    if (ok) {
      VkCommandBufferAllocateInfo ab{};
      ab.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
      ab.commandPool = cmd_pool;
      ab.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      ab.commandBufferCount = 1;
      ok = g.vk.alloc_cmds(g.device, &ab, &cb) == VK_SUCCESS;
    }
    if (ok) {
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      ok = g.vk.begin_cmd(cb, &bi) == VK_SUCCESS;
    }
    auto barrier_img = [&](VkImage img, VkImageLayout old_l, VkImageLayout new_l,
                           VkPipelineStageFlags2 src_s, VkAccessFlags2 src_a,
                           VkPipelineStageFlags2 dst_s, VkAccessFlags2 dst_a) {
      VkImageMemoryBarrier2 b{};
      b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
      b.srcStageMask = src_s;
      b.srcAccessMask = src_a;
      b.dstStageMask = dst_s;
      b.dstAccessMask = dst_a;
      b.oldLayout = old_l;
      b.newLayout = new_l;
      b.image = img;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkDependencyInfo d{};
      d.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
      d.imageMemoryBarrierCount = 1;
      d.pImageMemoryBarriers = &b;
      g.vk.cmd_barrier2(cb, &d);
    };
    if (ok) {
      barrier_img(lut_img.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                  0, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
      bv.cmd_bind_pipe(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
      bv.cmd_bind_sets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe_layout, 0, 1,
                       &set, 0, nullptr);
      bv.cmd_push(cb, pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &kLut);
      bv.cmd_dispatch(cb, kLut / 16, kLut / 16, 1);
      barrier_img(lut_img.img, VK_IMAGE_LAYOUT_GENERAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
      VkBufferImageCopy rl{};
      rl.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      rl.imageExtent = {kLut, kLut, 1};
      g.vk.cmd_copy_img_buf(cb, lut_img.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            read_b.buf, 1, &rl);
      VkMemoryBarrier2 hb{};
      hb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
      hb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      hb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      hb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
      hb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
      VkDependencyInfo dh{};
      dh.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
      dh.memoryBarrierCount = 1;
      dh.pMemoryBarriers = &hb;
      g.vk.cmd_barrier2(cb, &dh);
      ok = g.vk.end_cmd(cb) == VK_SUCCESS;
    }
    check(ok, "brdf-lut-gpu: frame recorded");
    if (ok) {
      ok = engine::test::GoldenSubmitWait(&g, cb);
      check(ok, "brdf-lut-gpu: submit + fence wait");
    }
    if (cmd_pool != nullptr) {
      g.vk.destroy_cmd_pool(g.device, cmd_pool, nullptr);
    }
  }
  // Readback: every texel vs CPU reference (CPU/GPU float tolerance).
  if (ok) {
    void* p = nullptr;
    ok = g.vk.map_mem(g.device, read_b.mem, 0, lut_bytes, 0, &p) == VK_SUCCESS &&
         p != nullptr;
    check(ok, "brdf-lut-gpu: readback mapped");
    if (ok) {
      const auto* f = static_cast<const float*>(p);
      std::uint32_t bad = 0;
      std::uint32_t nan_ct = 0;
      float worst = 0.0F;
      std::uint32_t worst_x = 0;
      std::uint32_t worst_y = 0;
      int worst_c = 0;
      for (std::uint32_t y = 0; y < kLut; ++y) {
        for (std::uint32_t x = 0; x < kLut; ++x) {
          float want_s = 0.0F;
          float want_b = 0.0F;
          if (!engine::renderer::BrdfLutReference(x, y, kLut, &want_s, &want_b)) {
            ++bad;
            continue;
          }
          const std::size_t i =
              (static_cast<std::size_t>(y) * kLut + x) * 2;
          const float want[2] = {want_s, want_b};
          for (int c = 0; c < 2; ++c) {
            const float got = f[i + static_cast<std::size_t>(c)];
            if (!std::isfinite(got)) {
              ++nan_ct;
              continue;
            }
            const float dd = std::fabs(got - want[c]);
            if (dd > worst) {
              worst = dd;
              worst_x = x;
              worst_y = y;
              worst_c = c;
            }
            if (dd > 2e-3F) {
              ++bad;
              if (bad <= 5) {
                std::printf(
                    "[INFO] brdf-lut-gpu: mismatch x=%u y=%u c=%d "
                    "got=%.4f want=%.4f\n",
                    x, y, c, static_cast<double>(got),
                    static_cast<double>(want[c]));
              }
            }
          }
        }
      }
      std::printf("[INFO] brdf-lut-gpu: worst abs diff %.6f, bad=%u nan=%u\n",
                  static_cast<double>(worst), bad, nan_ct);
      std::printf("[INFO] brdf-lut-gpu: worst at x=%u y=%u c=%d\n", worst_x,
                  worst_y, worst_c);
      check(bad == 0 && nan_ct == 0, "brdf-lut-gpu: lut matches CPU ref");
      // Spot edge value: near-normal smooth corner texel (x=62, y=0:
      // ndv~0.977, rough~0.008) is ~(0.994, ~0) — proves the GPU path
      // reproduces the analytic smooth-normal limit (scale~1, bias~0).
      const std::size_t ei =
          (static_cast<std::size_t>(0) * kLut + 62) * 2;
      check(f[ei] > 0.98F && f[ei + 1] < 0.01F,
            "brdf-lut-gpu: smooth normal edge sane");
      g.vk.unmap_mem(g.device, read_b.mem);
    }
  }
  if (pipe != nullptr) {
    bv.destroy_pipe(g.device, pipe, nullptr);
  }
  if (mod != nullptr) {
    g.vk.destroy_module(g.device, mod, nullptr);
  }
  if (set_layout != nullptr) {
    g.vk.destroy_set_layout(g.device, set_layout, nullptr);
  }
  if (pipe_layout != nullptr) {
    g.vk.destroy_pipe_layout(g.device, pipe_layout, nullptr);
  }
  if (pool != nullptr) {
    g.vk.destroy_pool(g.device, pool, nullptr);
  }
  engine::test::GoldenFreeImage(&g, &lut_img);
  engine::test::GoldenFreeHost(&g, &read_b);
  dev.shutdown();
  return 0;
}

#endif

}  // namespace

auto main() -> int {
  test_cpu();
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  test_gpu();
#else
  std::printf("[SKIP] brdf-lut-gpu: SDK absent (stub build)\n");
#endif
  std::printf(failures == 0 ? "test_brdf_lut: all checks passed\n"
                            : "test_brdf_lut: %d FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}

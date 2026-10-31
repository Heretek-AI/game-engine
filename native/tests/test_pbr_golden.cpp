// test_pbr_golden.cpp — P4-01 sphere-grid golden (own TU, own main).
//
// Renders the 5x5 material grid (metallic x, roughness y) through the
// REAL shade + post pipelines into an offscreen RGBA8 image and compares
// against golden p4_01_spheres_320x240 via golden_compare.hpp (P1-05
// tolerance). No visibility rasterization: the visibility image uploads
// from the CPU (each 64x48 tile -> its instance id), so the shade pass
// reads exactly what a rasterizer would have produced. One submit,
// fence-waited; [FAIL] (not [SKIP]) under ENGINE_REQUIRE_GPU=1.
//
// Check naming mirrors test_slang_pbr.cpp (own TU, no shared test lib).
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>
#include <vector>

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#endif

#include "engine/renderer/bindless_table.hpp"
#include "engine/renderer/pbr_material.hpp"
#include "engine/renderer/pbr_pipeline.hpp"
#include "engine/renderer/vulkan_device.hpp"

#include "engine/renderer/shaders/pbr_shade_frag_spv.inc"
#include "engine/renderer/shaders/pbr_shade_vert_spv.inc"
#include "engine/renderer/shaders/pbr_vis_frag_spv.inc"
#include "engine/renderer/shaders/pbr_vis_vert_spv.inc"
#include "engine/renderer/shaders/post_frag_spv.inc"
#include "engine/renderer/shaders/post_vert_spv.inc"

#include "golden_compare.hpp"

namespace {

auto failures = 0;


#if defined(ENGINE_RENDERER_HAS_VULKAN)

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}

// [FAIL]-not-[SKIP] under ENGINE_REQUIRE_GPU=1 (rule 11 / P1-01 contract).
// MSVC treats getenv as a C4996 error under /WX: _dupenv_s on _WIN32.
auto gpu_missing(std::string_view what) -> int {
#if defined(_WIN32)
  char* dup = nullptr;
  std::size_t dup_len = 0;
  const bool required =
      _dupenv_s(&dup, &dup_len, "ENGINE_REQUIRE_GPU") == 0 && dup != nullptr &&
      std::string_view(dup) == "1";
  std::free(dup);
#else
  const char* req = std::getenv("ENGINE_REQUIRE_GPU");
  const bool required = req != nullptr && std::string_view(req) == "1";
#endif
  std::printf("[%s] %.*s\n", required ? "FAIL" : "SKIP",
              static_cast<int>(what.size()), what.data());
  return required ? 1 : 0;
}

constexpr std::uint32_t kW = 320;
constexpr std::uint32_t kH = 240;
constexpr std::uint32_t kGrid = 5;  // 5x5 materials
constexpr std::uint32_t kInst = kGrid * kGrid;

struct Procs {
  PFN_vkCreateBuffer create_buffer{nullptr};
  PFN_vkDestroyBuffer destroy_buffer{nullptr};
  PFN_vkGetBufferMemoryRequirements get_buffer_reqs{nullptr};
  PFN_vkGetPhysicalDeviceMemoryProperties get_mem_props{nullptr};
  PFN_vkAllocateMemory alloc_mem{nullptr};
  PFN_vkFreeMemory free_mem{nullptr};
  PFN_vkBindBufferMemory bind_mem{nullptr};
  PFN_vkMapMemory map_mem{nullptr};
  PFN_vkUnmapMemory unmap_mem{nullptr};
  PFN_vkCreateImage create_image{nullptr};
  PFN_vkDestroyImage destroy_image{nullptr};
  PFN_vkGetImageMemoryRequirements get_image_reqs{nullptr};
  PFN_vkBindImageMemory bind_image_mem{nullptr};
  PFN_vkCreateImageView create_view{nullptr};
  PFN_vkDestroyImageView destroy_view{nullptr};
  PFN_vkCreateSampler create_sampler{nullptr};
  PFN_vkDestroySampler destroy_sampler{nullptr};
  PFN_vkCreateDescriptorPool create_pool{nullptr};
  PFN_vkDestroyDescriptorPool destroy_pool{nullptr};
  PFN_vkAllocateDescriptorSets alloc_sets{nullptr};
  PFN_vkUpdateDescriptorSets update_sets{nullptr};
  PFN_vkCreateDescriptorSetLayout create_set_layout{nullptr};
  PFN_vkDestroyDescriptorSetLayout destroy_set_layout{nullptr};
  PFN_vkCreatePipelineLayout create_pipe_layout{nullptr};
  PFN_vkDestroyPipelineLayout destroy_pipe_layout{nullptr};
  PFN_vkCreateShaderModule create_module{nullptr};
  PFN_vkDestroyShaderModule destroy_module{nullptr};
  PFN_vkCreateGraphicsPipelines create_pipes{nullptr};
  PFN_vkDestroyPipeline destroy_pipe{nullptr};
  PFN_vkCreateCommandPool create_cmd_pool{nullptr};
  PFN_vkDestroyCommandPool destroy_cmd_pool{nullptr};
  PFN_vkAllocateCommandBuffers alloc_cmds{nullptr};
  PFN_vkBeginCommandBuffer begin_cmd{nullptr};
  PFN_vkEndCommandBuffer end_cmd{nullptr};
  PFN_vkCmdBeginRendering cmd_begin_render{nullptr};
  PFN_vkCmdEndRendering cmd_end_render{nullptr};
  PFN_vkCmdBindPipeline cmd_bind_pipe{nullptr};
  PFN_vkCmdPushConstants cmd_push{nullptr};
  PFN_vkCmdDraw cmd_draw{nullptr};
  PFN_vkCmdBindDescriptorSets cmd_bind_sets{nullptr};
  PFN_vkCmdPipelineBarrier2 cmd_barrier2{nullptr};
  PFN_vkCmdCopyBufferToImage cmd_copy_buf_img{nullptr};
  PFN_vkCmdCopyImageToBuffer cmd_copy_img_buf{nullptr};
  PFN_vkCmdSetViewport cmd_viewport{nullptr};
  PFN_vkCmdSetScissor cmd_scissor{nullptr};
  PFN_vkCreateFence create_fence{nullptr};
  PFN_vkDestroyFence destroy_fence{nullptr};
  PFN_vkQueueSubmit queue_submit{nullptr};
  PFN_vkWaitForFences wait_fences{nullptr};
};

void load_procs(const engine::renderer::VulkanDevice& dev, Procs* p) {
  p->create_buffer = reinterpret_cast<PFN_vkCreateBuffer>(
      dev.instance_proc("vkCreateBuffer"));
  p->destroy_buffer = reinterpret_cast<PFN_vkDestroyBuffer>(
      dev.instance_proc("vkDestroyBuffer"));
  p->get_buffer_reqs =
      reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(
          dev.instance_proc("vkGetBufferMemoryRequirements"));
  p->get_mem_props =
      reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
          dev.instance_proc("vkGetPhysicalDeviceMemoryProperties"));
  p->alloc_mem = reinterpret_cast<PFN_vkAllocateMemory>(
      dev.instance_proc("vkAllocateMemory"));
  p->free_mem = reinterpret_cast<PFN_vkFreeMemory>(
      dev.instance_proc("vkFreeMemory"));
  p->bind_mem = reinterpret_cast<PFN_vkBindBufferMemory>(
      dev.instance_proc("vkBindBufferMemory"));
  p->map_mem =
      reinterpret_cast<PFN_vkMapMemory>(dev.instance_proc("vkMapMemory"));
  p->unmap_mem = reinterpret_cast<PFN_vkUnmapMemory>(
      dev.instance_proc("vkUnmapMemory"));
  p->create_image = reinterpret_cast<PFN_vkCreateImage>(
      dev.instance_proc("vkCreateImage"));
  p->destroy_image = reinterpret_cast<PFN_vkDestroyImage>(
      dev.instance_proc("vkDestroyImage"));
  p->get_image_reqs =
      reinterpret_cast<PFN_vkGetImageMemoryRequirements>(
          dev.instance_proc("vkGetImageMemoryRequirements"));
  p->bind_image_mem = reinterpret_cast<PFN_vkBindImageMemory>(
      dev.instance_proc("vkBindImageMemory"));
  p->create_view = reinterpret_cast<PFN_vkCreateImageView>(
      dev.instance_proc("vkCreateImageView"));
  p->destroy_view = reinterpret_cast<PFN_vkDestroyImageView>(
      dev.instance_proc("vkDestroyImageView"));
  p->create_sampler = reinterpret_cast<PFN_vkCreateSampler>(
      dev.instance_proc("vkCreateSampler"));
  p->destroy_sampler = reinterpret_cast<PFN_vkDestroySampler>(
      dev.instance_proc("vkDestroySampler"));
  p->create_pool = reinterpret_cast<PFN_vkCreateDescriptorPool>(
      dev.instance_proc("vkCreateDescriptorPool"));
  p->destroy_pool = reinterpret_cast<PFN_vkDestroyDescriptorPool>(
      dev.instance_proc("vkDestroyDescriptorPool"));
  p->alloc_sets = reinterpret_cast<PFN_vkAllocateDescriptorSets>(
      dev.instance_proc("vkAllocateDescriptorSets"));
  p->update_sets = reinterpret_cast<PFN_vkUpdateDescriptorSets>(
      dev.instance_proc("vkUpdateDescriptorSets"));
  p->create_set_layout =
      reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(
          dev.instance_proc("vkCreateDescriptorSetLayout"));
  p->destroy_set_layout =
      reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(
          dev.instance_proc("vkDestroyDescriptorSetLayout"));
  p->create_pipe_layout = reinterpret_cast<PFN_vkCreatePipelineLayout>(
      dev.instance_proc("vkCreatePipelineLayout"));
  p->destroy_pipe_layout = reinterpret_cast<PFN_vkDestroyPipelineLayout>(
      dev.instance_proc("vkDestroyPipelineLayout"));
  p->create_module = reinterpret_cast<PFN_vkCreateShaderModule>(
      dev.instance_proc("vkCreateShaderModule"));
  p->destroy_module = reinterpret_cast<PFN_vkDestroyShaderModule>(
      dev.instance_proc("vkDestroyShaderModule"));
  p->create_pipes = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(
      dev.proc("vkCreateGraphicsPipelines"));
  p->destroy_pipe =
      reinterpret_cast<PFN_vkDestroyPipeline>(dev.proc("vkDestroyPipeline"));
  p->create_cmd_pool = reinterpret_cast<PFN_vkCreateCommandPool>(
      dev.instance_proc("vkCreateCommandPool"));
  p->destroy_cmd_pool = reinterpret_cast<PFN_vkDestroyCommandPool>(
      dev.instance_proc("vkDestroyCommandPool"));
  p->alloc_cmds = reinterpret_cast<PFN_vkAllocateCommandBuffers>(
      dev.instance_proc("vkAllocateCommandBuffers"));
  p->begin_cmd = reinterpret_cast<PFN_vkBeginCommandBuffer>(
      dev.instance_proc("vkBeginCommandBuffer"));
  p->end_cmd = reinterpret_cast<PFN_vkEndCommandBuffer>(
      dev.instance_proc("vkEndCommandBuffer"));
  p->cmd_begin_render = reinterpret_cast<PFN_vkCmdBeginRendering>(
      dev.proc("vkCmdBeginRendering"));
  p->cmd_end_render =
      reinterpret_cast<PFN_vkCmdEndRendering>(dev.proc("vkCmdEndRendering"));
  p->cmd_bind_pipe = reinterpret_cast<PFN_vkCmdBindPipeline>(
      dev.proc("vkCmdBindPipeline"));
  p->cmd_push = reinterpret_cast<PFN_vkCmdPushConstants>(
      dev.proc("vkCmdPushConstants"));
  p->cmd_draw = reinterpret_cast<PFN_vkCmdDraw>(dev.proc("vkCmdDraw"));
  p->cmd_bind_sets = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(
      dev.proc("vkCmdBindDescriptorSets"));
  p->cmd_barrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(
      dev.instance_proc("vkCmdPipelineBarrier2"));
  p->cmd_copy_buf_img = reinterpret_cast<PFN_vkCmdCopyBufferToImage>(
      dev.proc("vkCmdCopyBufferToImage"));
  p->cmd_copy_img_buf = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(
      dev.proc("vkCmdCopyImageToBuffer"));
  p->cmd_viewport =
      reinterpret_cast<PFN_vkCmdSetViewport>(dev.proc("vkCmdSetViewport"));
  p->cmd_scissor =
      reinterpret_cast<PFN_vkCmdSetScissor>(dev.proc("vkCmdSetScissor"));
  p->create_fence = reinterpret_cast<PFN_vkCreateFence>(
      dev.instance_proc("vkCreateFence"));
  p->destroy_fence = reinterpret_cast<PFN_vkDestroyFence>(
      dev.instance_proc("vkDestroyFence"));
  p->queue_submit = reinterpret_cast<PFN_vkQueueSubmit>(
      dev.instance_proc("vkQueueSubmit"));
  p->wait_fences = reinterpret_cast<PFN_vkWaitForFences>(
      dev.instance_proc("vkWaitForFences"));
}

struct HostBuf {
  VkBuffer buf{nullptr};
  VkDeviceMemory mem{nullptr};
};

struct Img {
  VkImage img{nullptr};
  VkDeviceMemory mem{nullptr};
  VkImageView view{nullptr};
};

struct GoldenCtx {
  const engine::renderer::VulkanDevice* dev{nullptr};
  Procs vk{};
};

auto alloc_host(GoldenCtx* g, std::uint64_t bytes, VkBufferUsageFlags usage,
                HostBuf* out) -> bool {
  VkBufferCreateInfo bc{};
  bc.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bc.size = static_cast<VkDeviceSize>(bytes);
  bc.usage = usage;
  bc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (g->vk.create_buffer(g->dev->device(), &bc, nullptr, &out->buf) !=
      VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements req{};
  g->vk.get_buffer_reqs(g->dev->device(), out->buf, &req);
  VkPhysicalDeviceMemoryProperties mp{};
  g->vk.get_mem_props(g->dev->physical_device(), &mp);
  std::uint32_t mi = 0xFFFFFFFFu;
  for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((req.memoryTypeBits & (1u << i)) != 0u &&
        (mp.memoryTypes[i].propertyFlags &
         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0u &&
        (mp.memoryTypes[i].propertyFlags &
         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0u) {
      mi = i;
      break;
    }
  }
  if (mi == 0xFFFFFFFFu) {
    g->vk.destroy_buffer(g->dev->device(), out->buf, nullptr);
    out->buf = nullptr;
    return false;
  }
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = mi;
  if (g->vk.alloc_mem(g->dev->device(), &ai, nullptr, &out->mem) !=
      VK_SUCCESS) {
    g->vk.destroy_buffer(g->dev->device(), out->buf, nullptr);
    out->buf = nullptr;
    return false;
  }
  return g->vk.bind_mem(g->dev->device(), out->buf, out->mem, 0) == VK_SUCCESS;
}

void free_host(GoldenCtx* g, HostBuf* hb) {
  if (hb->buf != nullptr) {
    g->vk.destroy_buffer(g->dev->device(), hb->buf, nullptr);
    hb->buf = nullptr;
  }
  if (hb->mem != nullptr) {
    g->vk.free_mem(g->dev->device(), hb->mem, nullptr);
    hb->mem = nullptr;
  }
}

auto upload_bytes(GoldenCtx* g, HostBuf* hb, const void* src, std::size_t n)
    -> bool {
  void* p = nullptr;
  if (g->vk.map_mem(g->dev->device(), hb->mem, 0, n, 0, &p) != VK_SUCCESS ||
      p == nullptr) {
    return false;
  }
  const auto* b = static_cast<const std::uint8_t*>(src);
  auto* d = static_cast<std::uint8_t*>(p);
  for (std::size_t i = 0; i < n; ++i) {
    d[i] = b[i];
  }
  g->vk.unmap_mem(g->dev->device(), hb->mem);
  return true;
}

auto make_image(GoldenCtx* g, VkFormat fmt, VkImageUsageFlags usage, Img* out)
    -> bool {
  VkImageCreateInfo ii{};
  ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = fmt;
  ii.extent = {kW, kH, 1};
  ii.mipLevels = 1;
  ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = usage;
  ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (g->vk.create_image(g->dev->device(), &ii, nullptr, &out->img) !=
      VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements req{};
  g->vk.get_image_reqs(g->dev->device(), out->img, &req);
  VkPhysicalDeviceMemoryProperties mp{};
  g->vk.get_mem_props(g->dev->physical_device(), &mp);
  std::uint32_t mi = 0xFFFFFFFFu;
  for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if ((req.memoryTypeBits & (1u << i)) != 0u &&
        (mp.memoryTypes[i].propertyFlags &
         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0u) {
      mi = i;
      break;
    }
  }
  if (mi == 0xFFFFFFFFu) {
    for (std::uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
      if ((req.memoryTypeBits & (1u << i)) != 0u) {
        mi = i;
        break;
      }
    }
  }
  if (mi == 0xFFFFFFFFu) {
    g->vk.destroy_image(g->dev->device(), out->img, nullptr);
    out->img = nullptr;
    return false;
  }
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = mi;
  if (g->vk.alloc_mem(g->dev->device(), &ai, nullptr, &out->mem) !=
      VK_SUCCESS) {
    g->vk.destroy_image(g->dev->device(), out->img, nullptr);
    out->img = nullptr;
    return false;
  }
  if (g->vk.bind_image_mem(g->dev->device(), out->img, out->mem, 0) !=
      VK_SUCCESS) {
    g->vk.free_mem(g->dev->device(), out->mem, nullptr);
    g->vk.destroy_image(g->dev->device(), out->img, nullptr);
    out->img = nullptr;
    out->mem = nullptr;
    return false;
  }
  VkImageViewCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vi.image = out->img;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = fmt;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  return g->vk.create_view(g->dev->device(), &vi, nullptr, &out->view) ==
         VK_SUCCESS;
}

void free_image(GoldenCtx* g, Img* im) {
  if (im->view != nullptr) {
    g->vk.destroy_view(g->dev->device(), im->view, nullptr);
    im->view = nullptr;
  }
  if (im->img != nullptr) {
    g->vk.destroy_image(g->dev->device(), im->img, nullptr);
    im->img = nullptr;
  }
  if (im->mem != nullptr) {
    g->vk.free_mem(g->dev->device(), im->mem, nullptr);
    im->mem = nullptr;
  }
}

auto build_pipe(GoldenCtx* g, VkShaderModule vert, VkShaderModule frag,
                VkFormat color_fmt, VkPipelineLayout layout, VkPipeline* out)
    -> bool {
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag;
  stages[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo vertex_input{};
  vertex_input.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VkPipelineInputAssemblyStateCreateInfo input_assembly{};
  input_assembly.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo viewport{};
  viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo raster{};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo msaa{};
  msaa.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  msaa.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo depth{};
  depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  VkPipelineColorBlendAttachmentState blend_att{};
  blend_att.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend{};
  blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blend.attachmentCount = 1;
  blend.pAttachments = &blend_att;
  const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                        VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn{};
  dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dyn.dynamicStateCount = 2;
  dyn.pDynamicStates = dyn_states;
  VkPipelineRenderingCreateInfo rendering{};
  rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &color_fmt;
  VkGraphicsPipelineCreateInfo pci{};
  pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pci.pNext = &rendering;
  pci.stageCount = 2;
  pci.pStages = stages;
  pci.pVertexInputState = &vertex_input;
  pci.pInputAssemblyState = &input_assembly;
  pci.pViewportState = &viewport;
  pci.pRasterizationState = &raster;
  pci.pMultisampleState = &msaa;
  pci.pDepthStencilState = &depth;
  pci.pColorBlendState = &blend;
  pci.pDynamicState = &dyn;
  pci.layout = layout;
  return g->vk.create_pipes(g->dev->device(), nullptr, 1, &pci, nullptr,
                            out) == VK_SUCCESS;
}

void test_golden() {
  using engine::renderer::BindlessTable;
  using engine::renderer::DeviceCreateInfo;
  using engine::renderer::PbrMaterialLibrary;
  using engine::renderer::PbrPipeline;
  using engine::renderer::StatusCode;
  using engine::renderer::VulkanDevice;
  VulkanDevice dev;
  DeviceCreateInfo ci{};
  ci.app_name = "P4-01-golden";
  ci.device_hint = "llvmpipe";
  const auto st = dev.init(ci);
  if (!st.ok() && st.code == StatusCode::kNoPhysicalDevice) {
    failures += gpu_missing("pbr-golden: no physical device");
    return;
  }
  check(st.ok(), "pbr-golden: device init ok");
  if (!st.ok()) {
    return;
  }
  GoldenCtx g{};
  g.dev = &dev;
  load_procs(dev, &g.vk);
  check(g.vk.create_buffer != nullptr && g.vk.create_pipes != nullptr &&
            g.vk.queue_submit != nullptr,
        "pbr-golden: entry points resolve");
  if (g.vk.create_buffer == nullptr || g.vk.create_pipes == nullptr ||
      g.vk.queue_submit == nullptr) {
    dev.shutdown();
    return;
  }

  // PBR plan (blob validation) + bindless layout.
  PbrPipeline pbr;
  check(pbr
            .init(kPbr_vis_vertSpv, kPbr_vis_vertSpvWords, kPbr_vis_fragSpv,
                  kPbr_vis_fragSpvWords, kPbr_shade_vertSpv,
                  kPbr_shade_vertSpvWords, kPbr_shade_fragSpv,
                  kPbr_shade_fragSpvWords, kPost_vertSpv, kPost_vertSpvWords,
                  kPost_fragSpv, kPost_fragSpvWords)
            .ok(),
        "pbr-golden: plan init");
  BindlessTable bt;
  check(bt.init().ok(), "pbr-golden: bindless init");
  VkBuffer sentinel = reinterpret_cast<VkBuffer>(0x1000);
  check(bt.create_gpu(dev.device(), sentinel, 1 << 20, sentinel, 1 << 16)
            .ok(),
        "pbr-golden: bindless layout builds");
  if (!pbr.ready() || !bt.gpu_ready()) {
    bt.destroy_gpu();
    dev.shutdown();
    return;
  }

  // Shader modules (shade + post only — the golden records no geometry).
  VkShaderModule shade_v = nullptr;
  VkShaderModule shade_f = nullptr;
  VkShaderModule post_v = nullptr;
  VkShaderModule post_f = nullptr;
  auto make_mod = [&](const std::uint32_t* w, std::size_t n,
                      VkShaderModule* m) -> bool {
    VkShaderModuleCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sci.codeSize = n * 4;
    sci.pCode = w;
    return g.vk.create_module(dev.device(), &sci, nullptr, m) == VK_SUCCESS;
  };
  const bool mods_ok =
      make_mod(kPbr_shade_vertSpv, kPbr_shade_vertSpvWords, &shade_v) &&
      make_mod(kPbr_shade_fragSpv, kPbr_shade_fragSpvWords, &shade_f) &&
      make_mod(kPost_vertSpv, kPost_vertSpvWords, &post_v) &&
      make_mod(kPost_fragSpv, kPost_fragSpvWords, &post_f);
  check(mods_ok, "pbr-golden: modules created");
  if (!mods_ok) {
    for (auto* m : {&shade_v, &shade_f, &post_v, &post_f}) {
      if (*m != nullptr) {
        g.vk.destroy_module(dev.device(), *m, nullptr);
      }
    }
    bt.destroy_gpu();
    dev.shutdown();
    return;
  }

  // Set-1 layouts: shade (vis image[0] + albedo combined[2]) + post
  // (HDR image[0] + sampler[1]). Visibility rides shade set-1 binding 0.
  VkDescriptorSetLayout set1_shade = nullptr;
  VkDescriptorSetLayout set1_post = nullptr;
  bool layouts_ok = true;
  {
    // Shade set-1: binding 0 = visibility SAMPLED_IMAGE (texel-fetch),
    // binding 2 = albedo COMBINED_IMAGE_SAMPLER array. Dummies bound
    // (untextured golden).
    VkDescriptorSetLayoutBinding b[5]{};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1].binding = 2;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[2].binding = 3;
    b[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[2].descriptorCount = 1;
    b[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[3].binding = 4;
    b[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[3].descriptorCount = 1;
    b[3].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[4].binding = 5;
    b[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[4].descriptorCount = 1;
    b[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 5;
    li.pBindings = b;
    layouts_ok = g.vk.create_set_layout(dev.device(), &li, nullptr,
                                        &set1_shade) == VK_SUCCESS;
    VkDescriptorSetLayoutBinding pb[2]{};
    pb[0].binding = 0;
    pb[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    pb[0].descriptorCount = 1;
    pb[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pb[1].binding = 1;
    pb[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    pb[1].descriptorCount = 1;
    pb[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    pli.bindingCount = 2;
    pli.pBindings = pb;
    layouts_ok = layouts_ok && g.vk.create_set_layout(dev.device(), &pli,
                                                      nullptr,
                                                      &set1_post) == VK_SUCCESS;
  }
  check(layouts_ok, "pbr-golden: set-1 layouts");
  // Pipeline layouts: bindless set-0 + set-1, with push ranges.
  VkPipelineLayout shade_layout = nullptr;
  VkPipelineLayout post_layout = nullptr;
  bool plo_ok = layouts_ok;
  if (plo_ok) {
    VkDescriptorSetLayout sets[2] = {bt.set_layout(), set1_shade};
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset = 0;
    push.size = 64;  // PbrShadePush
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 2;
    pli.pSetLayouts = sets;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &push;
    plo_ok = g.vk.create_pipe_layout(dev.device(), &pli, nullptr,
                                     &shade_layout) == VK_SUCCESS;
    VkDescriptorSetLayout psets[2] = {bt.set_layout(), set1_post};
    VkPushConstantRange ppush{};
    ppush.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    ppush.offset = 0;
    ppush.size = 20;  // PostPush
    VkPipelineLayoutCreateInfo ppli{};
    ppli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    ppli.setLayoutCount = 2;
    ppli.pSetLayouts = psets;
    ppli.pushConstantRangeCount = 1;
    ppli.pPushConstantRanges = &ppush;
    plo_ok = plo_ok && g.vk.create_pipe_layout(dev.device(), &ppli, nullptr,
                                               &post_layout) == VK_SUCCESS;
  }
  check(plo_ok, "pbr-golden: pipeline layouts");
  VkPipeline shade_pipe = nullptr;
  VkPipeline post_pipe = nullptr;
  const bool pipes_ok =
      plo_ok &&
      build_pipe(&g, shade_v, shade_f, VK_FORMAT_R16G16B16A16_SFLOAT,
                 shade_layout, &shade_pipe) &&
      build_pipe(&g, post_v, post_f, VK_FORMAT_R8G8B8A8_UNORM, post_layout,
                 &post_pipe);
  check(pipes_ok, "pbr-golden: pipelines build");
  for (auto* m : {&shade_v, &shade_f, &post_v, &post_f}) {
    g.vk.destroy_module(dev.device(), *m, nullptr);
    *m = nullptr;
  }
  if (!pipes_ok) {
    if (shade_pipe != nullptr) {
      g.vk.destroy_pipe(dev.device(), shade_pipe, nullptr);
    }
    if (post_pipe != nullptr) {
      g.vk.destroy_pipe(dev.device(), post_pipe, nullptr);
    }
    if (shade_layout != nullptr) {
      g.vk.destroy_pipe_layout(dev.device(), shade_layout, nullptr);
    }
    if (post_layout != nullptr) {
      g.vk.destroy_pipe_layout(dev.device(), post_layout, nullptr);
    }
    g.vk.destroy_set_layout(dev.device(), set1_shade, nullptr);
    g.vk.destroy_set_layout(dev.device(), set1_post, nullptr);
    bt.destroy_gpu();
    dev.shutdown();
    return;
  }

  // -- Frame content (lambda for early return; cleanup runs once below) --
  // Handles live outside the lambda so cleanup sees them (all null until set).
  HostBuf scene_b{};
  HostBuf mat_b{};
  HostBuf light_b{};
  HostBuf grid_b{};
  HostBuf index_b{};
  HostBuf vis_b{};
  HostBuf read_b{};
  Img vis_img{};
  Img hdr_img{};
  Img ldr_img{};
  VkSampler sampler = nullptr;
  VkDescriptorPool pool = nullptr;
  VkDescriptorSet set0 = nullptr;
  VkDescriptorSet set_shade = nullptr;
  VkDescriptorSet set_post = nullptr;
  const int frame_rc = [&]() -> int {
  // Resource handles live at function scope so every failure path jumps to
  // the single golden_cleanup label without crossing initializations.
  constexpr std::uint64_t kSceneBytes = kInst * 96;
  constexpr std::uint64_t kMatBytes = kInst * 64;
  std::vector<std::uint8_t> scene(kSceneBytes, 0);
  std::vector<std::uint8_t> mats(kMatBytes, 0);
  PbrMaterialLibrary pack_lib;
  pack_lib.init();
  PbrMaterialLibrary& lib = pack_lib;
  bool pack_ok = true;
  float light_data[8] = {160.0f, 120.0f, 200.0f, 500.0f,
                         1.0f,   1.0f,   1.0f,   40000.0f};
  constexpr std::uint32_t kTilesX = kW / 16;
  constexpr std::uint32_t kTilesY = kH / 16;
  std::vector<std::uint32_t> grid(kTilesX * kTilesY * 2, 0);
  for (std::size_t t = 0; t < kTilesX * kTilesY; ++t) {
    grid[t * 2 + 0] = 0;
    grid[t * 2 + 1] = 1;
  }
  std::uint32_t index_list[1] = {0};
  std::vector<std::uint32_t> vis(kW * kH * 2, 0);
  for (std::uint32_t y = 0; y < kH; ++y) {
    for (std::uint32_t x = 0; x < kW; ++x) {
      const std::uint32_t gx = x / 64;
      const std::uint32_t gy = y / 48;
      vis[(y * kW + x) * 2 + 0] = gy * kGrid + gx;
      vis[(y * kW + x) * 2 + 1] = 0;
    }
  }
  bool bufs_ok = false;
  bool imgs_ok = false;
  bool sets_ok = false;
  {
    for (std::uint32_t gy = 0; gy < kGrid && pack_ok; ++gy) {
      for (std::uint32_t gx = 0; gx < kGrid && pack_ok; ++gx) {
        const std::uint32_t inst = gy * kGrid + gx;
        float* m = reinterpret_cast<float*>(&scene[inst * 96]);
        for (int d = 0; d < 16; ++d) {
          m[d] = (d % 5 == 0) ? 1.0f : 0.0f;
        }
        float* sph = reinterpret_cast<float*>(&scene[inst * 96 + 64]);
        sph[0] = (static_cast<float>(gx) + 0.5f) * 64.0f;
        sph[1] = (static_cast<float>(gy) + 0.5f) * 48.0f;
        sph[2] = 0.0f;
        sph[3] = 20.0f;
        std::uint32_t* ids =
            reinterpret_cast<std::uint32_t*>(&scene[inst * 96 + 80]);
        ids[0] = 0;
        ids[1] = inst;  // material_slot = instance index
        ids[2] = 1;     // flags: visible
        ids[3] = 0;
        engine::renderer::PbrMaterialAsset a{};
        a.base_color = {0.8f, 0.8f, 0.8f, 1.0f};
        a.metallic_roughness[0] = static_cast<float>(gx) / 4.0f;
        a.metallic_roughness[1] =
            0.04f + 0.96f * (static_cast<float>(gy) / 4.0f);
        std::uint32_t slot = 0xFFFFFFFFu;
        if (!lib.add(a, &slot).ok() || slot != inst) {
          check(false, "pbr-golden: material add");
          pack_ok = false;
          break;
        }
        float rec[16]{};
        if (!PbrMaterialLibrary::to_scene_record(a, rec).ok()) {
          check(false, "pbr-golden: record pack");
          pack_ok = false;
          break;
        }
        for (int f = 0; f < 16; ++f) {
          reinterpret_cast<float*>(&mats[inst * 64])[f] = rec[f];
        }
      }
    }
  }
  check(pack_ok, "pbr-golden: 25 materials packed");
  if (!pack_ok) {
    return 1;
  }
  bufs_ok =
      alloc_host(&g, kSceneBytes,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 &scene_b) &&
      alloc_host(&g, kMatBytes,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 &mat_b) &&
      alloc_host(&g, sizeof(light_data),
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 &light_b) &&
      alloc_host(&g, grid.size() * 4,
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 &grid_b) &&
      alloc_host(&g, sizeof(index_list),
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 &index_b) &&
      alloc_host(&g, static_cast<std::uint64_t>(kW) * kH * 8,
                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &vis_b) &&
      alloc_host(&g, static_cast<std::uint64_t>(kW) * kH * 4,
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT, &read_b);
  check(bufs_ok, "pbr-golden: upload buffers");
  if (!bufs_ok) {
    return 1;
  }
  if (!upload_bytes(&g, &scene_b, scene.data(), scene.size()) ||
      !upload_bytes(&g, &mat_b, mats.data(), mats.size()) ||
      !upload_bytes(&g, &light_b, light_data, sizeof(light_data)) ||
      !upload_bytes(&g, &grid_b, grid.data(), grid.size() * 4) ||
      !upload_bytes(&g, &index_b, index_list, sizeof(index_list)) ||
      !upload_bytes(&g, &vis_b, vis.data(), vis.size() * 4)) {
    check(false, "pbr-golden: uploads mapped");
    return 1;
  }
  check(true, "pbr-golden: uploads mapped");

  imgs_ok =
    make_image(&g, VK_FORMAT_R32G32_UINT,
               VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
               &vis_img) &&
    make_image(&g, VK_FORMAT_R16G16B16A16_SFLOAT,
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                   VK_IMAGE_USAGE_SAMPLED_BIT,
               &hdr_img) &&
    make_image(&g, VK_FORMAT_R8G8B8A8_UNORM,
               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
               &ldr_img);
  check(imgs_ok, "pbr-golden: images allocated");
  if (!imgs_ok) {
  return 1;
  }

  {
  VkSamplerCreateInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  si.magFilter = VK_FILTER_LINEAR;
  si.minFilter = VK_FILTER_LINEAR;
  si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (g.vk.create_sampler(dev.device(), &si, nullptr, &sampler) !=
      VK_SUCCESS) {
    check(false, "pbr-golden: sampler");
    return 1;
  }
  }

  {
  VkDescriptorPoolSize sizes[5]{};
  sizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  sizes[0].descriptorCount = 5;
  sizes[1].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  sizes[1].descriptorCount = 1;
  sizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLER;
  sizes[2].descriptorCount = 2;
  sizes[3].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  sizes[3].descriptorCount = 1;
  sizes[4].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  sizes[4].descriptorCount = 1;
  VkDescriptorPoolCreateInfo pi{};
  pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pi.maxSets = 3;
  pi.poolSizeCount = 5;
  pi.pPoolSizes = sizes;
  sets_ok = false;
  if (g.vk.create_pool(dev.device(), &pi, nullptr, &pool) == VK_SUCCESS) {
    VkDescriptorSetLayout layouts[3] = {bt.set_layout(), set1_shade,
                                        set1_post};
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = pool;
    ai.descriptorSetCount = 3;
    ai.pSetLayouts = layouts;
    VkDescriptorSet sets[3]{nullptr, nullptr, nullptr};
    if (g.vk.alloc_sets(dev.device(), &ai, sets) == VK_SUCCESS) {
      set0 = sets[0];
      set_shade = sets[1];
      set_post = sets[2];
      sets_ok = true;
    }
  }
  check(sets_ok, "pbr-golden: descriptor sets");
  if (!sets_ok) {
    return 1;
  }
  }

  {
  VkDescriptorBufferInfo scene_info{scene_b.buf, 0, kSceneBytes};
  VkDescriptorBufferInfo mat_info{mat_b.buf, 0, kMatBytes};
  VkDescriptorBufferInfo light_info{light_b.buf, 0, sizeof(light_data)};
  VkDescriptorBufferInfo grid_info{grid_b.buf, 0, grid.size() * 4};
  VkDescriptorBufferInfo index_info{index_b.buf, 0, sizeof(index_list)};
  // Visibility rides shade set-1 binding 0 (STORAGE_IMAGE readonly;
  // no sampler). The image uploads TRANSFER_DST -> GENERAL for imageLoad.
  // Set-0 binding 2 stays the bindless COMBINED array, unwritten by this
  // golden (P4-06 frame wiring owns it).
  VkDescriptorImageInfo vis_info{};
  vis_info.imageView = vis_img.view;
  vis_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  VkDescriptorImageInfo hdr_info{};
  hdr_info.imageView = hdr_img.view;
  hdr_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkDescriptorImageInfo post_smp{};
  post_smp.sampler = sampler;
  VkWriteDescriptorSet w[10]{};
  for (int i = 0; i < 10; ++i) {
    w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  }
  w[0].dstSet = set0;
  w[0].dstBinding = 0;
  w[0].descriptorCount = 1;
  w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  w[0].pBufferInfo = &scene_info;
  w[1].dstSet = set0;
  w[1].dstBinding = 1;
  w[1].descriptorCount = 1;
  w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  w[1].pBufferInfo = &mat_info;
  w[2].dstSet = set_shade;
  w[2].dstBinding = 0;
  w[2].descriptorCount = 1;
  w[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  w[2].pImageInfo = &vis_info;
  w[3].dstSet = set_shade;
  w[3].dstBinding = 3;
  w[3].descriptorCount = 1;
  w[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  w[3].pBufferInfo = &light_info;
  w[4].dstSet = set_shade;
  w[4].dstBinding = 4;
  w[4].descriptorCount = 1;
  w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  w[4].pBufferInfo = &grid_info;
  w[5].dstSet = set_shade;
  w[5].dstBinding = 5;
  w[5].descriptorCount = 1;
  w[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  w[5].pBufferInfo = &index_info;
  // Untextured golden: shade set-1 albedo dummy (combined image+sampler).
  VkDescriptorImageInfo shade_dummy_comb{};
  shade_dummy_comb.imageView = hdr_img.view;
  shade_dummy_comb.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  shade_dummy_comb.sampler = sampler;
  w[6].dstSet = set_shade;
  w[6].dstBinding = 2;
  w[6].descriptorCount = 1;
  w[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  w[6].pImageInfo = &shade_dummy_comb;
  w[7].dstSet = set_post;
  w[7].dstBinding = 0;
  w[7].descriptorCount = 1;
  w[7].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  w[7].pImageInfo = &hdr_info;
  w[8].dstSet = set_post;
  w[8].dstBinding = 1;
  w[8].descriptorCount = 1;
  w[8].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
  w[8].pImageInfo = &post_smp;
  g.vk.update_sets(dev.device(), 9, w, 0, nullptr);
  }
  check(true, "pbr-golden: descriptors written");

  // -- Record, submit, read back ------------------------------------------------
  bool submit_ok = false;
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kW) * kH * 4, 0);
  VkCommandPool cmd_pool = nullptr;
  {
    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.queueFamilyIndex = dev.graphics_queue_family();
    if (g.vk.create_cmd_pool(dev.device(), &cpi, nullptr, &cmd_pool) !=
        VK_SUCCESS) {
      check(false, "pbr-golden: cmd pool");
      return 1;
    }
  }
  {
    VkCommandBufferAllocateInfo ab{};
    ab.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ab.commandPool = cmd_pool;
    ab.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ab.commandBufferCount = 1;
    VkCommandBuffer cb = nullptr;
    if (g.vk.alloc_cmds(dev.device(), &ab, &cb) != VK_SUCCESS) {
      check(false, "pbr-golden: cmd alloc");
      return 1;
    }
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    bool rec_ok = g.vk.begin_cmd(cb, &bi) == VK_SUCCESS;
    if (rec_ok) {
      // 1. Visibility upload: buffer -> image, then to SHADER_READ.
      VkBufferImageCopy region{};
      region.bufferOffset = 0;
      region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.imageExtent = {kW, kH, 1};
      VkImageMemoryBarrier2 b0{};
      b0.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
      b0.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      b0.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      b0.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      b0.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      b0.image = vis_img.img;
      b0.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkDependencyInfo d0{};
      d0.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
      d0.imageMemoryBarrierCount = 1;
      d0.pImageMemoryBarriers = &b0;
      g.vk.cmd_barrier2(cb, &d0);
      g.vk.cmd_copy_buf_img(cb, vis_b.buf, vis_img.img,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
      VkImageMemoryBarrier2 b1 = b0;
      b1.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      b1.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      b1.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      b1.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
      b1.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      b1.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      VkDependencyInfo d1{};
      d1.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
      d1.imageMemoryBarrierCount = 1;
      d1.pImageMemoryBarriers = &b1;
      g.vk.cmd_barrier2(cb, &d1);
      // 2. HDR/LDR layouts: UNDEFINED -> COLOR_ATTACHMENT.
      VkImageMemoryBarrier2 batt[2]{};
      for (int i = 0; i < 2; ++i) {
        batt[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        batt[i].dstStageMask =
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        batt[i].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        batt[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        batt[i].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        batt[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      }
      batt[0].image = hdr_img.img;
      batt[1].image = ldr_img.img;
      VkDependencyInfo datt{};
      datt.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
      datt.imageMemoryBarrierCount = 2;
      datt.pImageMemoryBarriers = batt;
      g.vk.cmd_barrier2(cb, &datt);
      // 3. Shade pass -> HDR.
      VkRenderingAttachmentInfo shade_att{};
      shade_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      shade_att.imageView = hdr_img.view;
      shade_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      shade_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      shade_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      shade_att.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
      VkRenderingInfo shade_ri{};
      shade_ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
      shade_ri.renderArea = {{0, 0}, {kW, kH}};
      shade_ri.layerCount = 1;
      shade_ri.colorAttachmentCount = 1;
      shade_ri.pColorAttachments = &shade_att;
      g.vk.cmd_begin_render(cb, &shade_ri);
      VkViewport vp{0, 0, static_cast<float>(kW), static_cast<float>(kH),
                    0.0f, 1.0f};
      g.vk.cmd_viewport(cb, 0, 1, &vp);
      VkRect2D sc{{0, 0}, {kW, kH}};
      g.vk.cmd_scissor(cb, 0, 1, &sc);
      g.vk.cmd_bind_pipe(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, shade_pipe);
      VkDescriptorSet shade_sets[2] = {set0, set_shade};
      g.vk.cmd_bind_sets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, shade_layout,
                         0, 2, shade_sets, 0, nullptr);
      struct ShadePush {
        float cam[4];
        float frame[4];
        float inv[4];
        std::uint32_t lc;
        std::uint32_t flags;
        std::uint32_t max_tile;
        std::uint32_t pad;
      };
      ShadePush sp{};
      sp.cam[0] = 160.0f;
      sp.cam[1] = 120.0f;
      sp.cam[2] = 80.0f;
      sp.cam[3] = 1.0f;
      sp.frame[0] = static_cast<float>(kW);
      sp.frame[1] = static_cast<float>(kH);
      sp.frame[2] = 1.0f;
      sp.frame[3] = 1.0f;
      sp.inv[0] = 1.0f / static_cast<float>(kW);
      sp.inv[1] = 1.0f / static_cast<float>(kH);
      sp.inv[2] = 1.0f;
      sp.inv[3] = 1.0f;
      sp.lc = 1;
      sp.max_tile = 64;
      g.vk.cmd_push(cb, shade_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                    sizeof(sp), &sp);
      g.vk.cmd_draw(cb, 3, 1, 0, 0);
      g.vk.cmd_end_render(cb);
      // 4. HDR -> SHADER_READ for the post pass.
      VkImageMemoryBarrier2 b2{};
      b2.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
      b2.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
      b2.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
      b2.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
      b2.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
      b2.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      b2.image = hdr_img.img;
      b2.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkDependencyInfo d2{};
      d2.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
      d2.imageMemoryBarrierCount = 1;
      d2.pImageMemoryBarriers = &b2;
      g.vk.cmd_barrier2(cb, &d2);
      // 5. Post pass -> LDR.
      VkRenderingAttachmentInfo post_att{};
      post_att.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
      post_att.imageView = ldr_img.view;
      post_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      post_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      post_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      post_att.clearValue.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
      VkRenderingInfo post_ri{};
      post_ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
      post_ri.renderArea = {{0, 0}, {kW, kH}};
      post_ri.layerCount = 1;
      post_ri.colorAttachmentCount = 1;
      post_ri.pColorAttachments = &post_att;
      g.vk.cmd_begin_render(cb, &post_ri);
      g.vk.cmd_viewport(cb, 0, 1, &vp);
      g.vk.cmd_scissor(cb, 0, 1, &sc);
      g.vk.cmd_bind_pipe(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, post_pipe);
      VkDescriptorSet post_sets[2] = {set0, set_post};
      g.vk.cmd_bind_sets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, post_layout,
                         0, 2, post_sets, 0, nullptr);
      struct PostPush {
        float exposure;
        std::uint32_t flags;
        std::uint32_t p0;
        std::uint32_t p1;
        std::uint32_t p2;
      };
      PostPush pp{};
      pp.exposure = 1.0f;
      g.vk.cmd_push(cb, post_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                    sizeof(pp), &pp);
      g.vk.cmd_draw(cb, 3, 1, 0, 0);
      g.vk.cmd_end_render(cb);
      // 6. LDR -> TRANSFER_SRC + copy to readback.
      VkImageMemoryBarrier2 b3 = b2;
      b3.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
      b3.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
      b3.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      b3.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
      b3.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      b3.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      b3.image = ldr_img.img;
      VkDependencyInfo d3{};
      d3.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
      d3.imageMemoryBarrierCount = 1;
      d3.pImageMemoryBarriers = &b3;
      g.vk.cmd_barrier2(cb, &d3);
      VkBufferImageCopy rc{};
      rc.bufferOffset = 0;
      rc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      rc.imageExtent = {kW, kH, 1};
      g.vk.cmd_copy_img_buf(cb, ldr_img.img,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, read_b.buf,
                            1, &rc);
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
      rec_ok = g.vk.end_cmd(cb) == VK_SUCCESS;
    }
    if (rec_ok) {
      VkSubmitInfo si{};
      si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      si.commandBufferCount = 1;
      si.pCommandBuffers = &cb;
      VkFenceCreateInfo fi{};
      fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
      VkFence fence = nullptr;
      if (g.vk.create_fence(dev.device(), &fi, nullptr, &fence) ==
          VK_SUCCESS) {
        if (g.vk.queue_submit(dev.graphics_queue(), 1, &si, fence) ==
                VK_SUCCESS &&
            g.vk.wait_fences(dev.device(), 1, &fence, 1, 10000000000ull) ==
                VK_SUCCESS) {
          submit_ok = true;
        }
        g.vk.destroy_fence(dev.device(), fence, nullptr);
      }
    }
  }
  check(submit_ok, "pbr-golden: submit + fence wait");
  check(submit_ok, "pbr-golden: submit + fence wait");
  {
    // TEMP-DEBUG: verify the scene/material upload bytes landed.
    void* sp2 = nullptr;
    if (g.vk.map_mem(dev.device(), scene_b.mem, 0, kSceneBytes, 0, &sp2) ==
            VK_SUCCESS &&
        sp2 != nullptr) {
      const auto* su = static_cast<const std::uint32_t*>(sp2);
      const auto* sf = static_cast<const float*>(sp2);
      std::printf("scenebuf i0: ids=%u %u %u %u sph=%.1f\n", su[20], su[21],
                  su[22], su[23], sf[16]);
      std::printf("scenebuf i24: ids=%u %u %u %u sph=%.1f\n", su[24 * 24 + 20],
                  su[24 * 24 + 21], su[24 * 24 + 22], su[24 * 24 + 23],
                  sf[24 * 24 + 16]);
      g.vk.unmap_mem(dev.device(), scene_b.mem);
    }
    // TEMP-DEBUG: verify the material upload bytes landed in mat_b.
    void* mp = nullptr;
    if (g.vk.map_mem(dev.device(), mat_b.mem, 0, kMatBytes, 0, &mp) ==
            VK_SUCCESS &&
        mp != nullptr) {
      const auto* f = static_cast<const float*>(mp);
      std::printf("matbuf m0: base=%.2f metal=%.2f rough=%.2f\n", f[0], f[4],
                  f[5]);
      std::printf("matbuf m24: base=%.2f metal=%.2f rough=%.2f\n", f[24 * 16],
                  f[24 * 16 + 4], f[24 * 16 + 5]);
      const auto* u = static_cast<const std::uint32_t*>(mp);
      std::printf("matbuf m24 lanes: %u %u %u %u\n", u[24 * 16 + 8],
                  u[24 * 16 + 9], u[24 * 16 + 10], u[24 * 16 + 11]);
      g.vk.unmap_mem(dev.device(), mat_b.mem);
    }
  }
  if (submit_ok) {
    void* p = nullptr;
    const std::size_t n = static_cast<std::size_t>(kW) * kH * 4;
    if (g.vk.map_mem(dev.device(), read_b.mem, 0, n, 0, &p) == VK_SUCCESS &&
        p != nullptr) {
      const auto* src = static_cast<const std::uint8_t*>(p);
      for (std::size_t i = 0; i < n; ++i) {
        pixels[i] = src[i];
      }
      g.vk.unmap_mem(dev.device(), read_b.mem);
      check(true, "pbr-golden: readback mapped");
      failures += engine::test::golden_check(
          "p4_01_spheres_320x240",
          std::span<const std::uint8_t>(pixels.data(), pixels.size()), kW,
          kH);
      // Sanity: metallic varies along x — column 0 (metal 0, diffuse
      // kd~0.235) is brighter than column 1 (metal 0.25, kd~0.147) on the
      // bottom (rough) row. The single centered light makes the image
      // x-symmetric, so left-half vs right-half would compare equal.
      std::uint64_t col0 = 0;
      std::uint64_t col1 = 0;
      for (std::uint32_t y = 4 * 48; y < 5 * 48; ++y) {
        for (std::uint32_t x = 0; x < 64; ++x) {
          const std::size_t i = (y * kW + x) * 4;
          col0 += pixels[i] + pixels[i + 1] + pixels[i + 2];
          const std::size_t j = (y * kW + x + 64) * 4;
          col1 += pixels[j] + pixels[j + 1] + pixels[j + 2];
        }
      }
      check(col0 > col1, "pbr-golden: metallic varies along x");
    } else {
      check(false, "pbr-golden: readback mapped");
    }
  }
  g.vk.destroy_cmd_pool(dev.device(), cmd_pool, nullptr);

  return 0;
  }();
  (void)frame_rc;
  g.vk.destroy_pool(dev.device(), pool, nullptr);
  g.vk.destroy_sampler(dev.device(), sampler, nullptr);
  free_image(&g, &vis_img);
  free_image(&g, &hdr_img);
  free_image(&g, &ldr_img);
  free_host(&g, &scene_b);
  free_host(&g, &mat_b);
  free_host(&g, &light_b);
  free_host(&g, &grid_b);
  free_host(&g, &index_b);
  free_host(&g, &vis_b);
  free_host(&g, &read_b);
  g.vk.destroy_pipe(dev.device(), shade_pipe, nullptr);
  g.vk.destroy_pipe(dev.device(), post_pipe, nullptr);
  g.vk.destroy_pipe_layout(dev.device(), shade_layout, nullptr);
  g.vk.destroy_pipe_layout(dev.device(), post_layout, nullptr);
  g.vk.destroy_set_layout(dev.device(), set1_shade, nullptr);
  g.vk.destroy_set_layout(dev.device(), set1_post, nullptr);
  bt.destroy_gpu();
  dev.shutdown();
}

#endif

}  // namespace

auto main() -> int {
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  test_golden();
#else
  std::printf("[SKIP] pbr-golden: SDK absent (stub build)\n");
#endif
  std::printf(failures == 0 ? "test_pbr_golden: all checks passed\n"
                            : "test_pbr_golden: %d FAILED\n",
              failures);
  return failures == 0 ? 0 : 1;
}

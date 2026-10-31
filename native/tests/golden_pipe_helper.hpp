// golden_pipe_helper.hpp — P4-01 shared test helper (NOT a golden contract).
//
// Builds one dynamic-rendering graphics pipeline for a fullscreen-triangle
// pair (vertex + fragment): empty vertex input, triangle-list assembly,
// FILL/NONE/CCW raster, 1-sample MSAA, no depth, all-channels blend,
// viewport+scissor dynamic state. Both P4-01 GPU test TUs (test_pbr_materials
// build-proof + test_pbr_golden offscreen render) share this so Sonar does
// not count the ~60-line Vulkan struct-fill as duplicated new code.
//
// Header-only, Vulkan-guarded (stub builds skip it), own TU via `#pragma
// once` + inline. The create_pipes callable is a template so each TU can
// pass its own proc-loader type (raw PFN or struct member).
#pragma once

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

namespace engine::test {

// Fill a dynamic-rendering graphics pipeline for one fullscreen-triangle
// (vert, frag) pair writing color_fmt through layout into *out.
template <typename CreatePipesFn>
inline auto GoldenBuildPipe(CreatePipesFn create_pipes, VkDevice device,
                            VkShaderModule vert, VkShaderModule frag,
                            VkFormat color_fmt, VkPipelineLayout layout,
                            VkPipeline* out) -> bool {
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
  return create_pipes(device, nullptr, 1, &pci, nullptr, out) == VK_SUCCESS;
}

// P4-01 shade/post set-1 layout descriptors (mirrors pbr_shade.frag +
// post_composite.frag): shade binding 0 = visibility STORAGE_IMAGE
// (rg32ui uimage2D), 2 = albedo COMBINED_IMAGE_SAMPLER array, 3/4/5 =
// lights/grid/index STORAGE_BUFFERs; post binding 0 = HDR SAMPLED_IMAGE,
// 1 = SAMPLER. Templated on the create-layout callable so both TU
// proc-loader styles work; each TU still owns its VkDescriptorSetLayout
// handles (destroyed by the TU, like its pipelines).
template <typename CreateSetLayoutFn>
inline auto GoldenBuildSet1(CreateSetLayoutFn create_set_layout,
                            VkDevice device, VkDescriptorSetLayout* shade_out,
                            VkDescriptorSetLayout* post_out) -> bool {
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
  bool ok = create_set_layout(device, &li, nullptr, shade_out) == VK_SUCCESS;
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
  return ok && create_set_layout(device, &pli, nullptr, post_out) == VK_SUCCESS;
}

}  // namespace engine::test

#endif

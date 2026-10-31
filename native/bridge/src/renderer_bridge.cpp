// renderer_bridge.cpp — P1-09: native EngineRenderer behind the ABI tail.
//
// EngineRenderer owns one SDL3 window + Vulkan device + swapchain +
// frame context + ForwardRenderer mesh pipeline + depth buffer + the
// uploaded GpuMeshes (mesh_id = owned vector index + 1; 0 is reserved).
// The Flecs world is BORROWED (owned by the C# caller — destroy the
// renderer before the world, or frames stop).
//
// Frame loop mirrors the P1-08 test exactly: begin_frame -> color/depth
// barrier -> begin_mesh_pass -> render_world (aspect from the swapchain
// extent) -> end_mesh_pass -> barrier to TRANSFER_SRC -> end_frame
// (present) -> capture_rgba8 for screenshot. render_world skips cleanly
// (0, no draw) when the world has no is_main camera yet.
//
// All entries are noexcept (an exception crossing the C ABI is UB) and
// fail closed: null args, zero extents, upload errors, and frame errors
// all return null/0/negative codes — never abort.
#include "engine/bridge/engine_abi.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#endif

#include "engine/ecs/ecs_render_components.hpp"
#include "engine/renderer/forward_renderer.hpp"
#include "engine/renderer/gpu_mesh.hpp"
#include "engine/renderer/mesh_registry.hpp"
#include "engine/renderer/renderer_window.hpp"
#include "engine/renderer/swapchain.hpp"
#include "engine/renderer/vulkan_device.hpp"
#include "flecs.h"

namespace engine::bridge {

// The ABI EngineWorld* is an EcsWorld-backed handle owned by the caller.
// The renderer borrows the raw Flecs pointer: BridgeRawWorld (defined in
// engine_abi.cpp next to the TU-local ImplOf) unwraps it. flecs.h above
// already declares ::ecs_world_t — no local forward declaration (it would
// shadow the real type inside this namespace and break overloads).
auto BridgeRawWorld(EngineWorld* world) noexcept -> ::ecs_world_t*;

using ::engine::renderer::DepthBuffer;
using ::engine::renderer::ForwardRenderer;
using ::engine::renderer::FrameContext;
using ::engine::renderer::GpuMesh;
using ::engine::renderer::MeshRegistry;
using ::engine::renderer::MeshVertex;
using ::engine::renderer::RendererWindow;
using ::engine::renderer::Swapchain;
using ::engine::renderer::VulkanDevice;
using ::engine::renderer::WindowConfig;

// Concrete renderer: the opaque EngineRenderer* the ABI hands to C#.
struct RendererImpl {
  ::ecs_world_t* world{nullptr};  // borrowed, never owned
  RendererWindow window;
  VulkanDevice device;
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  VkSurfaceKHR surface{nullptr};
#endif
  Swapchain swapchain;
  FrameContext frames;
  ForwardRenderer forward;
  DepthBuffer depth;
  MeshRegistry registry;
  // Owned meshes: registry borrows these, so they must outlive it.
  std::vector<std::unique_ptr<GpuMesh>> meshes;
  std::uint32_t width{0};
  std::uint32_t height{0};
  bool depth_live{false};
  std::vector<std::uint8_t> last_frame;
};

auto ImplOf(EngineRenderer* r) noexcept -> RendererImpl* {
  return reinterpret_cast<RendererImpl*>(r);  // NOLINT
}

#if defined(ENGINE_RENDERER_HAS_VULKAN)

// One frame: pump events, render the borrowed world, present, capture.
// Returns 0 on success, 1 when the window was closed (SDL_EVENT_QUIT),
// negative errno-style codes on failure. `px_out` (optional) receives
// the captured RGBA8 frame for screenshot.
auto RenderOne(RendererImpl* impl,
               ForwardRenderer::RenderWorldStats* stats_out,
               std::vector<std::uint8_t>* px_out) noexcept -> int {
  std::uint32_t quit = 0;
  if (!impl->window.pump_events(&quit).ok()) {
    return ENGINE_ABI_ERR_INTERNAL;
  }
  if (quit != 0) {
    return 1;
  }
  auto begin = impl->frames.begin_frame();
  if (!begin.ok || begin.cmd == nullptr) {
    return ENGINE_ABI_ERR_INTERNAL;
  }
  VkImage img = impl->swapchain.image(begin.image_index);
  VkImageView view = impl->swapchain.view(begin.image_index);
  if (img == nullptr || view == nullptr) {
    return ENGINE_ABI_ERR_INTERNAL;
  }
  auto* barrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(
      impl->device.proc("vkCmdPipelineBarrier2"));
  if (barrier2 == nullptr) {
    return ENGINE_ABI_ERR_INTERNAL;
  }
  // Color: UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL.
  {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = 1;
    VkDependencyInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    barrier2(begin.cmd, &di);
  }
  // Depth: first frame UNDEFINED -> ATTACHMENT, then ATTACHMENT -> self.
  {
    VkImage depth_img = reinterpret_cast<VkImage>(
        static_cast<std::uintptr_t>(impl->depth.image_bits()));
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    const bool first_use = !impl->depth_live;
    b.srcStageMask = first_use ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT
                               : (VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT);
    b.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    b.srcAccessMask =
        first_use ? 0 : VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.oldLayout = first_use ? VK_IMAGE_LAYOUT_UNDEFINED
                            : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    b.image = depth_img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = 1;
    VkDependencyInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    barrier2(begin.cmd, &di);
  }
  impl->depth_live = true;
  if (!impl->forward
           .begin_mesh_pass(begin.cmd, view, impl->depth.view(),
                            impl->swapchain.extent())
           .ok()) {
    return ENGINE_ABI_ERR_INTERNAL;
  }
  // Skip the draw when the world has no is_main camera yet: present a
  // cleared frame (the begin pass cleared color+depth).
  bool has_main = false;
  const ecs_entity_t cam_id = ecs_lookup_path_w_sep(
      impl->world, 0, "Engine.Runtime.Camera", "", "", false);
  if (cam_id != 0) {
    ecs_query_desc_t qd{};
    qd.terms[0].id = cam_id;
    qd.terms[0].src.id = EcsSelf;
    qd.cache_kind = EcsQueryCacheAuto;
    ecs_query_t* q = ecs_query_init(impl->world, &qd);
    if (q != nullptr) {
      ecs_iter_t it = ecs_query_iter(impl->world, q);
      while (ecs_query_next(&it)) {
        auto* col = static_cast<engine::ecs::Camera*>(
            ecs_field_w_size(&it, sizeof(engine::ecs::Camera), 0));
        if (col == nullptr) {
          break;
        }
        for (int r = 0; r < it.count; ++r) {
          if (col[r].is_main != 0) {
            has_main = true;
            break;
          }
        }
        if (has_main) {
          break;
        }
      }
      ecs_query_fini(q);
    }
  }
  ForwardRenderer::RenderWorldStats stats{};
  if (has_main) {
    const float aspect =
        impl->height > 0
            ? static_cast<float>(impl->width) / static_cast<float>(impl->height)
            : 16.0F / 9.0F;
    const auto st = impl->forward.render_world(impl->world, impl->registry,
                                               begin.cmd, nullptr, &stats,
                                               aspect);
    if (!st.ok()) {
      return ENGINE_ABI_ERR_INTERNAL;
    }
  }
  if (!impl->forward.end_mesh_pass(begin.cmd).ok()) {
    return ENGINE_ABI_ERR_INTERNAL;
  }
  // To TRANSFER_SRC for capture + present.
  {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = 1;
    VkDependencyInfo di{};
    di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    barrier2(begin.cmd, &di);
  }
  if (!impl->frames.end_frame(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL).ok()) {
    return ENGINE_ABI_ERR_INTERNAL;
  }
  if (stats_out != nullptr) {
    *stats_out = stats;
  }
  if (px_out != nullptr) {
    std::vector<std::uint8_t> px;
    if (!engine::renderer::capture_rgba8(
             impl->device, img, impl->swapchain.format(),
             impl->swapchain.extent(), VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, px)
             .ok()) {
      return ENGINE_ABI_ERR_INTERNAL;
    }
    *px_out = std::move(px);
  }
  return 0;
}

#endif  // ENGINE_RENDERER_HAS_VULKAN

auto ApiRendererCreate(EngineWorld* world, std::uint32_t width,
                       std::uint32_t height, std::uint32_t flags) noexcept
    -> EngineRenderer* {
  if (world == nullptr || width == 0 || height == 0) {
    return nullptr;
  }
  auto* ecs_raw = BridgeRawWorld(world);
  if (ecs_raw == nullptr) {
    return nullptr;
  }
  auto impl = std::unique_ptr<RendererImpl>(new (std::nothrow) RendererImpl());
  if (impl == nullptr) {
    return nullptr;
  }
#if !defined(ENGINE_RENDERER_HAS_VULKAN)
  (void)ecs_raw;
  (void)flags;
  // SDK-free build: report the missing GPU path at frame/screenshot time
  // (null renderer would hide the cause). Destroy stays meaningful.
  impl->width = width;
  impl->height = height;
  return reinterpret_cast<EngineRenderer*>(impl.release());  // NOLINT
#else
  // Register the P1-08 render components idempotently through the shared
  // helper (same path EcsWorld::register_render uses, so C#
  // Ecs.ComponentId<T>() resolves the same ids).
  auto reg_ok = [&](const char* name, std::uint32_t size,
                    std::uint32_t align) -> bool {
    return engine::ecs::RegisterRenderComponent(ecs_raw, name, size, align) !=
           0;
  };
  if (!reg_ok("Engine.Runtime.Camera", 16, 4) ||
      !reg_ok("Engine.Runtime.MeshRenderer", 20, 4) ||
      !reg_ok("Engine.Runtime.DirectionalLight", 16, 4)) {
    return nullptr;
  }
  impl->world = ecs_raw;
  impl->width = width;
  impl->height = height;
  WindowConfig cfg{};
  cfg.hidden = (flags & ENGINE_RENDERER_HIDDEN) != 0;
  cfg.width = width;
  cfg.height = height;
  if (!impl->window.create(cfg).ok()) {
    return nullptr;
  }
  std::array<const char*, 64> exts{};
  std::uint32_t got = 0;
  if (!impl->window.required_instance_extensions(exts.data(), 64, &got).ok() ||
      got == 0) {
    impl->window.destroy();
    return nullptr;
  }
  engine::renderer::DeviceCreateInfo ci{};
  ci.app_name = "P1-09-hellocube";
  ci.extra_instance_extensions = exts.data();
  ci.extra_instance_extension_count = got;
  ci.enable_swapchain = true;
  if (!impl->device.init(ci).ok()) {
    impl->window.destroy();
    return nullptr;
  }
  VkSurfaceKHR surface = nullptr;
  if (!impl->device.create_surface(impl->window, &surface).ok() ||
      surface == nullptr) {
    impl->device.shutdown();
    impl->window.destroy();
    return nullptr;
  }
  impl->surface = surface;
  engine::renderer::SwapchainDesc desc{};
  desc.width = width;
  desc.height = height;
  desc.vsync = true;
  if (!impl->swapchain.create(impl->device, impl->surface, desc).ok()) {
    impl->device.destroy_surface(impl->surface);
    impl->device.shutdown();
    impl->window.destroy();
    return nullptr;
  }
  if (!impl->forward
           .create_mesh(impl->device, impl->swapchain.format(),
                        VK_FORMAT_D32_SFLOAT)
           .ok()) {
    impl->swapchain.destroy();
    impl->device.destroy_surface(impl->surface);
    impl->device.shutdown();
    impl->window.destroy();
    return nullptr;
  }
  if (!impl->depth
           .create(impl->device, impl->swapchain.extent(),
                   VK_FORMAT_D32_SFLOAT)
           .ok()) {
    impl->forward.destroy();
    impl->swapchain.destroy();
    impl->device.destroy_surface(impl->surface);
    impl->device.shutdown();
    impl->window.destroy();
    return nullptr;
  }
  if (!impl->frames.create(impl->device, impl->swapchain).ok()) {
    impl->depth.destroy();
    impl->forward.destroy();
    impl->swapchain.destroy();
    impl->device.destroy_surface(impl->surface);
    impl->device.shutdown();
    impl->window.destroy();
    return nullptr;
  }
  return reinterpret_cast<EngineRenderer*>(impl.release());  // NOLINT
#endif
}

auto ApiRendererDestroy(EngineRenderer* renderer) noexcept -> void {
  if (renderer == nullptr) {
    return;
  }
  auto* impl = ImplOf(renderer);
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  impl->meshes.clear();
  impl->registry.clear();
  impl->frames.destroy();
  impl->depth.destroy();
  impl->forward.destroy();
  impl->swapchain.destroy();
  if (impl->surface != nullptr) {
    impl->device.destroy_surface(impl->surface);
    impl->surface = nullptr;
  }
  impl->device.shutdown();
  impl->window.destroy();
#endif
  delete impl;  // NOLINT
}

auto ApiRendererFrame(EngineRenderer* renderer) noexcept -> int32_t {
  if (renderer == nullptr) {
    return ENGINE_ABI_ERR_NULL_ARG;
  }
  auto* impl = ImplOf(renderer);
#if !defined(ENGINE_RENDERER_HAS_VULKAN)
  (void)impl;
  return ENGINE_ABI_ERR_INTERNAL;
#else
  if (impl->world == nullptr) {
    return ENGINE_ABI_ERR_INTERNAL;
  }
  ForwardRenderer::RenderWorldStats stats{};
  const int rc = RenderOne(impl, &stats, &impl->last_frame);
  if (rc < 0) {
    impl->last_frame.clear();
  }
  return rc;
#endif
}

auto ApiRendererMeshCreate(EngineRenderer* renderer, const float* vertices,
                           std::uint32_t vertex_count, const std::uint32_t* indices,
                           std::uint32_t index_count) noexcept -> std::uint32_t {
  if (renderer == nullptr || vertices == nullptr || indices == nullptr ||
      vertex_count == 0 || index_count == 0) {
    return 0;
  }
#if !defined(ENGINE_RENDERER_HAS_VULKAN)
  (void)renderer;
  return 0;
#else
  auto* impl = ImplOf(renderer);
  // vertices carries vertex_count * 8 floats (pos[3], normal[3], uv[2]).
  const auto* verts = reinterpret_cast<const MeshVertex*>(vertices);
  auto mesh = std::unique_ptr<GpuMesh>(new (std::nothrow) GpuMesh());
  if (mesh == nullptr) {
    return 0;
  }
  if (!mesh->create(impl->device, verts, vertex_count, indices, index_count)
           .ok()) {
    return 0;
  }
  const std::uint32_t id = impl->registry.add(mesh.get());
  if (id == 0) {
    return 0;
  }
  impl->meshes.push_back(std::move(mesh));
  return id;
#endif
}

auto ApiRendererScreenshot(EngineRenderer* renderer,
                           const char* png_path) noexcept -> int32_t {
  if (renderer == nullptr || png_path == nullptr || png_path[0] == '\0') {
    return ENGINE_ABI_ERR_NULL_ARG;
  }
  auto* impl = ImplOf(renderer);
  if (impl->last_frame.empty()) {
    return ENGINE_ABI_ERR_BAD_ARG;  // no frame rendered yet
  }
#if !defined(ENGINE_RENDERER_HAS_VULKAN)
  (void)png_path;
  return ENGINE_ABI_ERR_INTERNAL;
#else
  const std::uint32_t w = impl->swapchain.extent().width;
  const std::uint32_t h = impl->swapchain.extent().height;
  if (impl->last_frame.size() != static_cast<std::size_t>(w) * h * 4) {
    return ENGINE_ABI_ERR_INTERNAL;
  }
  if (!engine::renderer::write_png(png_path, impl->last_frame, w, h).ok()) {
    return ENGINE_ABI_ERR_INTERNAL;
  }
  return 0;
#endif
}

}  // namespace engine::bridge

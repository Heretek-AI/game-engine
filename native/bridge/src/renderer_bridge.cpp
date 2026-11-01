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

// Process-global VMA allocator (defined in vma_impl.cpp as
// ::engine::renderer::vma_bridge::g_alloc; declared here as an opaque
// pointer so this TU stays vk_mem_alloc.h-free — same object, same linkage).
namespace engine::renderer::vma_bridge {
struct Allocator_T;
extern Allocator_T* g_alloc;
}  // namespace engine::renderer::vma_bridge

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
// Windowed (P1-09: window + swapchain + frame loop) or offscreen (P3-02:
// no window/swapchain — color + id + depth targets, owned command pool
// for the id/transfer work). `offscreen` selects the shape; destroy()
// tears down whichever shape create() built.
struct RendererImpl {
  ::ecs_world_t* world{nullptr};  // borrowed, never owned
  bool offscreen{false};
  RendererWindow window;
  VulkanDevice device;
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  VkSurfaceKHR surface{nullptr};
  VkCommandPool offscreen_pool{nullptr};
  // Last one-shot offscreen command buffer (freed by the next
  // OffscreenBegin; the pool's destroy reclaims it at teardown).
  VkCommandBuffer offscreen_cmd{nullptr};
  // Mesh + pick pipelines built for the offscreen formats. Formats never
  // change on resize, so OffscreenBuildTargets builds them once.
  bool offscreen_pipelines_live{false};
  std::uint64_t offscreen_color_bits{0};
  void* offscreen_color_alloc{nullptr};
  VkImageView offscreen_color_view{nullptr};
  std::uint64_t offscreen_id_bits{0};
  void* offscreen_id_alloc{nullptr};
  VkImageView offscreen_id_view{nullptr};
  std::uint64_t offscreen_depth_bits{0};
  void* offscreen_depth_alloc{nullptr};
  VkImageView offscreen_depth_view{nullptr};
  bool offscreen_depth_live{false};
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

// --- ABI 4.6 offscreen helpers (P3-02/P3-06) --------------------------------
// Windowless device (no surface/swapchain), transient pool, and one VMA
// image+view per target: color R8G8B8A8_UNORM (TRANSFER_SRC|ATTACHMENT),
// id R32G32_UINT (TRANSFER_SRC|ATTACHMENT), depth D32_SFLOAT (attachment).
auto OffscreenDeviceInit(RendererImpl* impl) noexcept -> bool;
auto OffscreenMakeView(RendererImpl* impl, std::uint64_t bits, VkFormat format,
                       VkImageAspectFlags aspect,
                       VkImageView& view_out) noexcept -> bool;
auto OffscreenBegin(RendererImpl* impl, VkCommandBuffer& cmd_out) noexcept
    -> bool;
auto OffscreenSubmit(RendererImpl* impl, VkCommandBuffer cmd) noexcept
    -> bool;
auto OffscreenBuildTargets(RendererImpl* impl, std::uint32_t width,
                           std::uint32_t height) noexcept -> bool;
auto OffscreenFreeTarget(RendererImpl* impl, VkImageView& view,
                         std::uint64_t& bits, void*& handle) noexcept -> void;
auto OffscreenTeardown(RendererImpl* impl) noexcept -> void;

#endif  // ENGINE_RENDERER_HAS_VULKAN

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
  // SDK-free build: report the missing GPU path at frame/screenshot time
  // (null renderer would hide the cause). Destroy stays meaningful.
  // NOTE: the render components must be registered here too, not just on
  // the SDK path below — C# ComponentId<T>() for transform/render mirrors
  // on an SDK-free renderer would otherwise fork-or-fail (macOS/Windows
  // CI hit exactly this: HelloCube's LocalTransform resolve failed
  // closed). Same idempotent helper as the SDK path.
  {
    auto reg_ok = [&](const char* name, std::uint32_t size,
                      std::uint32_t align) -> bool {
      return engine::ecs::RegisterRenderComponent(ecs_raw, name, size,
                                                  align) != 0;
    };
    if (!reg_ok("Engine.Runtime.Camera", 16, 4) ||
        !reg_ok("Engine.Runtime.MeshRenderer", 20, 4) ||
        !reg_ok("Engine.Runtime.DirectionalLight", 16, 4)) {
      return nullptr;
    }
  }
  (void)flags;
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
  if (impl->offscreen) {
    OffscreenTeardown(impl);
  } else {
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
  // Offscreen renderers share the windowed upload path (device + registry
  // only — no swapchain involved). The offscreen flag only changes which
  // targets frame/pick render into, never mesh storage.
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

// --- render_to_buffer / pick shared frame helpers --------------------------
// Both entries record into one transient-pool command buffer: barrier the
// used targets out of UNDEFINED, open the pass (CLEAR), draw, close, then
// barrier the readback target to TRANSFER_SRC for the copy.
#if defined(ENGINE_RENDERER_HAS_VULKAN)

namespace {

[[maybe_unused]] auto OffscreenBarrierAll(RendererImpl* impl,
                         VkCommandBuffer cmd) noexcept -> void {
  auto vkCmdPipelineBarrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(
      impl->device.proc("vkCmdPipelineBarrier2"));
  if (vkCmdPipelineBarrier2 == nullptr) {
    return;
  }
  VkImage color_img = reinterpret_cast<VkImage>(
      static_cast<std::uintptr_t>(impl->offscreen_color_bits));
  VkImage id_img = reinterpret_cast<VkImage>(
      static_cast<std::uintptr_t>(impl->offscreen_id_bits));
  VkImage depth_img = reinterpret_cast<VkImage>(
      static_cast<std::uintptr_t>(impl->offscreen_depth_bits));
  VkImageMemoryBarrier2 bs[3]{};
  for (int i = 0; i < 3; ++i) {
    bs[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    bs[i].srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    bs[i].subresourceRange.baseMipLevel = 0;
    bs[i].subresourceRange.levelCount = 1;
    bs[i].subresourceRange.baseArrayLayer = 0;
    bs[i].subresourceRange.layerCount = 1;
  }
  bs[0].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  bs[0].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  bs[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  bs[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  bs[0].image = color_img;
  bs[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  bs[1].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  bs[1].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  bs[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  bs[1].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  bs[1].image = id_img;
  bs[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  bs[2].dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                       VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
  bs[2].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  bs[2].oldLayout = impl->offscreen_depth_live
                        ? VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL
                        : VK_IMAGE_LAYOUT_UNDEFINED;
  bs[2].newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  bs[2].srcStageMask = impl->offscreen_depth_live
                           ? (VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT)
                           : VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
  bs[2].srcAccessMask =
      impl->offscreen_depth_live
          ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
          : static_cast<VkAccessFlags2>(0);
  bs[2].image = depth_img;
  bs[2].subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  VkDependencyInfo di{};
  di.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  di.imageMemoryBarrierCount = 3;
  di.pImageMemoryBarriers = bs;
  vkCmdPipelineBarrier2(cmd, &di);
  impl->offscreen_depth_live = true;
}

[[maybe_unused]] auto OffscreenBarrierToTransfer(RendererImpl* impl, VkCommandBuffer cmd,
                                bool color) noexcept -> void {
  auto vkCmdPipelineBarrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(
      impl->device.proc("vkCmdPipelineBarrier2"));
  if (vkCmdPipelineBarrier2 == nullptr) {
    return;
  }
  VkImage img = color ? reinterpret_cast<VkImage>(static_cast<std::uintptr_t>(
                                impl->offscreen_color_bits))
                      : reinterpret_cast<VkImage>(static_cast<std::uintptr_t>(
                                impl->offscreen_id_bits));
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
  vkCmdPipelineBarrier2(cmd, &di);
}

}  // namespace

#endif  // ENGINE_RENDERER_HAS_VULKAN

auto ApiRendererCreateOffscreen(EngineWorld* world, std::uint32_t width,
                                std::uint32_t height) noexcept
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
  {
    // Render components (same idempotent helper as the windowed path, so
    // C# Ecs.ComponentId<T>() resolves the same ids).
    auto reg_ok = [&](const char* name, std::uint32_t size,
                      std::uint32_t align) -> bool {
      return engine::ecs::RegisterRenderComponent(ecs_raw, name, size,
                                                  align) != 0;
    };
    if (!reg_ok("Engine.Runtime.Camera", 16, 4) ||
        !reg_ok("Engine.Runtime.MeshRenderer", 20, 4) ||
        !reg_ok("Engine.Runtime.DirectionalLight", 16, 4)) {
      return nullptr;
    }
  }
#if !defined(ENGINE_RENDERER_HAS_VULKAN)
  (void)impl;
  return nullptr;
#else
  impl->world = ecs_raw;
  impl->offscreen = true;
  if (!OffscreenDeviceInit(impl.get())) {
    return nullptr;
  }
  if (!OffscreenBuildTargets(impl.get(), width, height)) {
    OffscreenTeardown(impl.get());
    return nullptr;
  }
  return reinterpret_cast<EngineRenderer*>(impl.release());  // NOLINT
#endif
}

// --- render_to_buffer: lit offscreen pass -----------------------------------
// Records into one transient-pool command buffer: barrier all targets out
// of UNDEFINED, open the lit color+depth pass (CLEAR), draw the world with
// the caller's explicit column-major view_proj (render_ids override path),
// close, barrier color -> TRANSFER_SRC, submit, then read back via
// capture_rgba8 (P1-04). Resizes the targets first when the size differs
// from the create extent (render_to_buffer owns the resize; pick does not).
auto ApiRendererRenderToBuffer(EngineRenderer* renderer, std::uint8_t* rgba_out,
                               std::uint64_t capacity, std::uint32_t width,
                               std::uint32_t height,
                               const float* view_proj16) noexcept -> int32_t {
  if (renderer == nullptr) {
    return ENGINE_ABI_ERR_NULL_ARG;
  }
  if (rgba_out == nullptr || view_proj16 == nullptr || width == 0 ||
      height == 0) {
    return ENGINE_ABI_ERR_NULL_ARG;
  }
  const std::uint64_t need =
      static_cast<std::uint64_t>(width) * height * 4u;
  if (capacity < need) {
    return ENGINE_ABI_ERR_BAD_ARG;
  }
  auto* impl = ImplOf(renderer);
  if (!impl->offscreen) {
    return ENGINE_ABI_ERR_BAD_ARG;  // windowed renderers present, not copy
  }
#if !defined(ENGINE_RENDERER_HAS_VULKAN)
  (void)impl;
  return ENGINE_ABI_ERR_INTERNAL;
#else
  if (impl->world == nullptr) {
    return ENGINE_ABI_ERR_INTERNAL;
  }
  if (width != impl->width || height != impl->height) {
    if (!OffscreenBuildTargets(impl, width, height)) {
      return ENGINE_ABI_ERR_INTERNAL;
    }
  }
  VkCommandBuffer cmd = nullptr;
  if (!OffscreenBegin(impl, cmd)) {
    return ENGINE_ABI_ERR_INTERNAL;
  }
  OffscreenBarrierAll(impl, cmd);
  VkExtent2D extent{};
  extent.width = width;
  extent.height = height;
  // Lit color+depth pass (CLEAR, STORE color).
  if (!impl->forward
           .begin_mesh_pass(cmd, impl->offscreen_color_view,
                            impl->offscreen_depth_view, extent)
           .ok()) {
    return ENGINE_ABI_ERR_INTERNAL;
  }
  {
    ForwardRenderer::RenderWorldStats stats{};
    if (!impl->forward
             .render_lit_with_matrix(impl->world, impl->registry, cmd,
                                     view_proj16, &stats)
             .ok()) {
      return ENGINE_ABI_ERR_INTERNAL;
    }
  }
  if (!impl->forward.end_mesh_pass(cmd).ok()) {
    return ENGINE_ABI_ERR_INTERNAL;
  }
  OffscreenBarrierToTransfer(impl, cmd, true);
  if (!OffscreenSubmit(impl, cmd)) {
    return ENGINE_ABI_ERR_INTERNAL;
  }
  // Read back the color target (P1-04 capture path).
  {
    VkExtent2D ext{};
    ext.width = width;
    ext.height = height;
    std::vector<std::uint8_t> px;
    if (!engine::renderer::capture_rgba8(
             impl->device,
             reinterpret_cast<VkImage>(
                 static_cast<std::uintptr_t>(impl->offscreen_color_bits)),
             VK_FORMAT_R8G8B8A8_UNORM, ext,
             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, px)
             .ok()) {
      return ENGINE_ABI_ERR_INTERNAL;
    }
    const std::uint64_t got = static_cast<std::uint64_t>(px.size());
    if (got != need) {
      return ENGINE_ABI_ERR_INTERNAL;
    }
    for (std::uint64_t i = 0; i < need; ++i) {
      rgba_out[i] = px[static_cast<std::size_t>(i)];
    }
  }
  return 0;
#endif
}

// --- renderer_pick: id pass + one-texel read --------------------------------
// Same one-shot shape as render_to_buffer, but through the pick pipeline:
// barrier all, begin_pick_pass (CLEAR id to 0 = background), render_ids
// with the caller's view_proj16 (the editor camera it rendered with, so
// the id under the cursor matches the frame on screen; null falls back to
// the world's main camera), end pass, barrier id ->
// TRANSFER_SRC, submit, then copy one R32G32_UINT texel (8 bytes) to host.
// OOB reads return 0 without touching the GPU (same rule as managed
// ObjectIdBuffer.Pick).
auto ApiRendererPick(EngineRenderer* renderer, std::uint32_t x,
                     std::uint32_t y, const float* view_proj16) noexcept
    -> std::uint64_t {
  if (renderer == nullptr) {
    return 0;
  }
  auto* impl = ImplOf(renderer);
  if (!impl->offscreen) {
    return 0;
  }
#if !defined(ENGINE_RENDERER_HAS_VULKAN)
  (void)impl;
  (void)x;
  (void)y;
  (void)view_proj16;
  return 0;
#else
  if (impl->world == nullptr) {
    return 0;
  }
  if (x >= impl->width || y >= impl->height) {
    return 0;
  }
  VkCommandBuffer cmd = nullptr;
  if (!OffscreenBegin(impl, cmd)) {
    return 0;
  }
  OffscreenBarrierAll(impl, cmd);
  VkExtent2D extent{};
  extent.width = impl->width;
  extent.height = impl->height;
  if (!impl->forward
           .begin_pick_pass(cmd, impl->offscreen_id_view,
                            impl->offscreen_depth_view, extent)
           .ok()) {
    return 0;
  }
  {
    ForwardRenderer::RenderWorldStats stats{};
    const float aspect =
        impl->height > 0
            ? static_cast<float>(impl->width) / static_cast<float>(impl->height)
            : 16.0F / 9.0F;
    if (!impl->forward
             .render_ids(impl->world, impl->registry, cmd, extent, &stats,
                         aspect, view_proj16)
             .ok()) {
      return 0;
    }
  }
  if (!impl->forward.end_pick_pass(cmd).ok()) {
    return 0;
  }
  OffscreenBarrierToTransfer(impl, cmd, false);
  if (!OffscreenSubmit(impl, cmd)) {
    return 0;
  }
  // One-texel copy (R32G32_UINT = 8 bytes) via the P1-04 one-shot pattern.
  std::uint64_t id = 0;
  {
    auto vkCreateCommandPool = reinterpret_cast<PFN_vkCreateCommandPool>(
        impl->device.instance_proc("vkCreateCommandPool"));
    auto vkDestroyCommandPool = reinterpret_cast<PFN_vkDestroyCommandPool>(
        impl->device.instance_proc("vkDestroyCommandPool"));
    auto vkAllocateCommandBuffers =
        reinterpret_cast<PFN_vkAllocateCommandBuffers>(
            impl->device.instance_proc("vkAllocateCommandBuffers"));
    auto vkBeginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(
        impl->device.instance_proc("vkBeginCommandBuffer"));
    auto vkEndCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer>(
        impl->device.instance_proc("vkEndCommandBuffer"));
    auto vkCmdCopyImageToBuffer =
        reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(
            impl->device.proc("vkCmdCopyImageToBuffer"));
    auto vkCreateFence = reinterpret_cast<PFN_vkCreateFence>(
        impl->device.instance_proc("vkCreateFence"));
    auto vkDestroyFence = reinterpret_cast<PFN_vkDestroyFence>(
        impl->device.instance_proc("vkDestroyFence"));
    auto vkWaitForFences = reinterpret_cast<PFN_vkWaitForFences>(
        impl->device.instance_proc("vkWaitForFences"));
    auto vkQueueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(
        impl->device.instance_proc("vkQueueSubmit"));
    auto vkMapMemory = reinterpret_cast<PFN_vkMapMemory>(
        impl->device.instance_proc("vkMapMemory"));
    auto vkUnmapMemory = reinterpret_cast<PFN_vkUnmapMemory>(
        impl->device.instance_proc("vkUnmapMemory"));
    auto vkCreateBuffer = reinterpret_cast<PFN_vkCreateBuffer>(
        impl->device.instance_proc("vkCreateBuffer"));
    auto vkDestroyBuffer = reinterpret_cast<PFN_vkDestroyBuffer>(
        impl->device.instance_proc("vkDestroyBuffer"));
    auto vkGetBufferMemoryRequirements =
        reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(
            impl->device.instance_proc("vkGetBufferMemoryRequirements"));
    auto vkAllocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(
        impl->device.instance_proc("vkAllocateMemory"));
    auto vkFreeMemory = reinterpret_cast<PFN_vkFreeMemory>(
        impl->device.instance_proc("vkFreeMemory"));
    auto vkBindBufferMemory = reinterpret_cast<PFN_vkBindBufferMemory>(
        impl->device.instance_proc("vkBindBufferMemory"));
    auto vkGetPhysicalDeviceMemoryProperties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            impl->device.instance_proc(
                "vkGetPhysicalDeviceMemoryProperties"));
    if (vkCreateCommandPool == nullptr || vkDestroyCommandPool == nullptr ||
        vkAllocateCommandBuffers == nullptr ||
        vkBeginCommandBuffer == nullptr || vkEndCommandBuffer == nullptr ||
        vkCmdCopyImageToBuffer == nullptr || vkCreateFence == nullptr ||
        vkDestroyFence == nullptr || vkWaitForFences == nullptr ||
        vkQueueSubmit == nullptr || vkMapMemory == nullptr ||
        vkUnmapMemory == nullptr || vkCreateBuffer == nullptr ||
        vkDestroyBuffer == nullptr ||
        vkGetBufferMemoryRequirements == nullptr ||
        vkAllocateMemory == nullptr || vkFreeMemory == nullptr ||
        vkBindBufferMemory == nullptr ||
        vkGetPhysicalDeviceMemoryProperties == nullptr) {
      return 0;
    }
    VkDevice device = impl->device.device();
    // Host-visible staging buffer (TRANSFER_DST, 8 bytes).
    VkBuffer buf = nullptr;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = 8;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &bci, nullptr, &buf) != VK_SUCCESS ||
        buf == nullptr) {
      return 0;
    }
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device, buf, &req);
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(impl->device.physical_device(),
                                        &props);
    std::uint32_t mem_type = 0xFFFFFFFFu;
    for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i) {
      const bool in_mask = (req.memoryTypeBits & (1u << i)) != 0u;
      const VkMemoryPropertyFlags flags = props.memoryTypes[i].propertyFlags;
      const bool host_visible =
          (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
      const bool host_coherent =
          (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
      if (in_mask && host_visible && host_coherent) {
        mem_type = i;
        break;
      }
    }
    if (mem_type == 0xFFFFFFFFu) {
      vkDestroyBuffer(device, buf, nullptr);
      return 0;
    }
    VkDeviceMemory mem = nullptr;
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mem_type;
    if (vkAllocateMemory(device, &mai, nullptr, &mem) != VK_SUCCESS ||
        mem == nullptr) {
      vkDestroyBuffer(device, buf, nullptr);
      return 0;
    }
    if (vkBindBufferMemory(device, buf, mem, 0) != VK_SUCCESS) {
      vkFreeMemory(device, mem, nullptr);
      vkDestroyBuffer(device, buf, nullptr);
      return 0;
    }
    VkCommandPool pool = nullptr;
    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    cpi.queueFamilyIndex = impl->device.graphics_queue_family();
    if (vkCreateCommandPool(device, &cpi, nullptr, &pool) != VK_SUCCESS ||
        pool == nullptr) {
      vkFreeMemory(device, mem, nullptr);
      vkDestroyBuffer(device, buf, nullptr);
      return 0;
    }
    VkCommandBuffer ccmd = nullptr;
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    bool ok = false;
    if (vkAllocateCommandBuffers(device, &cai, &ccmd) == VK_SUCCESS &&
        ccmd != nullptr) {
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      if (vkBeginCommandBuffer(ccmd, &bi) == VK_SUCCESS) {
        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset.x = static_cast<std::int32_t>(x);
        region.imageOffset.y = static_cast<std::int32_t>(y);
        region.imageOffset.z = 0;
        region.imageExtent.width = 1;
        region.imageExtent.height = 1;
        region.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(
            ccmd,
            reinterpret_cast<VkImage>(
                static_cast<std::uintptr_t>(impl->offscreen_id_bits)),
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
        if (vkEndCommandBuffer(ccmd) == VK_SUCCESS) {
          VkFence fence = nullptr;
          VkFenceCreateInfo fci{};
          fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
          if (vkCreateFence(device, &fci, nullptr, &fence) == VK_SUCCESS &&
              fence != nullptr) {
            VkSubmitInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1;
            si.pCommandBuffers = &ccmd;
            if (vkQueueSubmit(impl->device.graphics_queue(), 1, &si, fence) ==
                VK_SUCCESS) {
              ok = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX) ==
                   VK_SUCCESS;
            }
            vkDestroyFence(device, fence, nullptr);
          }
        }
      }
    }
    if (ok) {
      void* mapped = nullptr;
      if (vkMapMemory(device, mem, 0, VK_WHOLE_SIZE, 0, &mapped) ==
              VK_SUCCESS &&
          mapped != nullptr) {
        std::uint64_t lanes = 0;
        std::memcpy(&lanes, mapped, sizeof(lanes));
        id = lanes;
        vkUnmapMemory(device, mem);
      } else {
        ok = false;
      }
    }
    vkDestroyCommandPool(device, pool, nullptr);
    vkFreeMemory(device, mem, nullptr);
    vkDestroyBuffer(device, buf, nullptr);
    if (!ok) {
      return 0;
    }
  }
  return id;
#endif
}

// --- ABI 4.6 offscreen + picking tail (P3-02/P3-06) -------------------------
// Offscreen renderers own no window/swapchain: color (R8G8B8A8_UNORM) +
// R32G32_UINT id + D32_SFLOAT depth targets plus the mesh + pick
// pipelines, built here through the same VulkanDevice proc table + VMA
// path as the windowed renderer (DepthBuffer/PickIdBuffer patterns).
// Images come from EngineVma helpers via the renderer-internal bridge TU
// (vma_impl.cpp owns vk_mem_alloc.h); views + barriers + copies use the
// device procs directly. All entries are noexcept + fail closed.
//
// NOTE: this TU deliberately does NOT include vma_bridge.hpp (its include
// dir is renderer-private, and including it would drag vk_mem_alloc.h's
// -Werror nullability warnings into this hot-flagged TU — the same reason
// vulkan_device.cpp avoids it). Instead the three image allocators needed
// here are declared locally (defined in vma_impl.cpp, same linkage as the
// depth/id helpers PickIdBuffer uses).
#if defined(ENGINE_RENDERER_HAS_VULKAN)
// Local declarations of the vma_impl.cpp image allocators (same linkage
// PickIdBuffer uses via vma_bridge.hpp; this TU avoids that header because
// its include dir is renderer-private). All bundles share one layout
// (image bits + allocation handle), declared once here.
struct OffscreenBundle {
  std::uint64_t image_bits{0};
  void* handle{nullptr};
};
static_assert(sizeof(OffscreenBundle) == 16, "offscreen bundle is 16B");
// (VMA allocator declared at file scope above.)
extern "C" {
auto EngineVmaCreateDepthImage(void* alloc, std::uint32_t width,
                               std::uint32_t height,
                               OffscreenBundle* out) -> int;
auto EngineVmaCreateIdImage(void* alloc, std::uint32_t width,
                            std::uint32_t height, OffscreenBundle* out)
    -> int;
auto EngineVmaCreateColorImage(void* alloc, std::uint32_t width,
                               std::uint32_t height, OffscreenBundle* out)
    -> int;
auto EngineVmaDestroyImage(void* alloc, std::uint64_t image_bits,
                           void* handle) -> void;
}
#endif

#if defined(ENGINE_RENDERER_HAS_VULKAN)

auto OffscreenAllocImage(void* alloc, std::uint32_t width,
                         std::uint32_t height, VkFormat format,
                         VkImageUsageFlags usage, std::uint64_t& bits_out,
                         void*& handle_out) noexcept -> bool {
  if (alloc == nullptr || width == 0 || height == 0) {
    return false;
  }
  bits_out = 0;
  handle_out = nullptr;
  // TRANSFER_SRC is required on color + id (readback); depth needs
  // only the attachment bit (same usage as DepthBuffer/PickIdBuffer).
  (void)usage;
  OffscreenBundle tmp{};
  int rc = -1;
  if (format == VK_FORMAT_R8G8B8A8_UNORM) {
    rc = EngineVmaCreateColorImage(alloc, width, height, &tmp);
  } else if (format == VK_FORMAT_R32G32_UINT) {
    rc = EngineVmaCreateIdImage(alloc, width, height, &tmp);
  } else if (format == VK_FORMAT_D32_SFLOAT) {
    rc = EngineVmaCreateDepthImage(alloc, width, height, &tmp);
  }
  if (rc != 0) {
    return false;
  }
  bits_out = tmp.image_bits;
  handle_out = tmp.handle;
  return true;
}

// Windowless device init: no surface, no swapchain extensions (headless
// path — SDL never initializes). The transient pool serves every one-shot
// command buffer in this TU (frame + id pass + readback copies).
auto OffscreenDeviceInit(RendererImpl* impl) noexcept -> bool {
  engine::renderer::DeviceCreateInfo ci{};
  ci.app_name = "P3-02-offscreen";
  ci.enable_swapchain = false;
  if (!impl->device.init(ci).ok()) {
    return false;
  }
  if (!impl->device.vma_ready()) {
    return false;
  }
  auto vkCreateCommandPool = reinterpret_cast<PFN_vkCreateCommandPool>(
      impl->device.instance_proc("vkCreateCommandPool"));
  if (vkCreateCommandPool == nullptr) {
    return false;
  }
  VkCommandPoolCreateInfo cpi{};
  cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  cpi.queueFamilyIndex = impl->device.graphics_queue_family();
  if (vkCreateCommandPool(impl->device.device(), &cpi, nullptr,
                          &impl->offscreen_pool) != VK_SUCCESS ||
      impl->offscreen_pool == nullptr) {
    return false;
  }
  return true;
}

// One VkImageView for a VMA image (color/id/depth aspects).
auto OffscreenMakeView(RendererImpl* impl, std::uint64_t bits, VkFormat format,
                       VkImageAspectFlags aspect,
                       VkImageView& view_out) noexcept -> bool {
  view_out = nullptr;
  auto vkCreateImageView = reinterpret_cast<PFN_vkCreateImageView>(
      impl->device.instance_proc("vkCreateImageView"));
  if (vkCreateImageView == nullptr) {
    return false;
  }
  VkImageViewCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vi.image =
      reinterpret_cast<VkImage>(static_cast<std::uintptr_t>(bits));
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = format;
  vi.subresourceRange.aspectMask = aspect;
  vi.subresourceRange.baseMipLevel = 0;
  vi.subresourceRange.levelCount = 1;
  vi.subresourceRange.baseArrayLayer = 0;
  vi.subresourceRange.layerCount = 1;
  return vkCreateImageView(impl->device.device(), &vi, nullptr, &view_out) ==
             VK_SUCCESS &&
         view_out != nullptr;
}

// Allocate + begin a one-shot command buffer from the transient pool.
auto OffscreenBegin(RendererImpl* impl, VkCommandBuffer& cmd_out) noexcept
    -> bool {
  cmd_out = nullptr;
  auto vkAllocateCommandBuffers =
      reinterpret_cast<PFN_vkAllocateCommandBuffers>(
          impl->device.instance_proc("vkAllocateCommandBuffers"));
  auto vkBeginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(
      impl->device.instance_proc("vkBeginCommandBuffer"));
  auto vkFreeCommandBuffers = reinterpret_cast<PFN_vkFreeCommandBuffers>(
      impl->device.instance_proc("vkFreeCommandBuffers"));
  if (vkAllocateCommandBuffers == nullptr ||
      vkBeginCommandBuffer == nullptr || vkFreeCommandBuffers == nullptr) {
    return false;
  }
  // Every offscreen submit is fence-waited (or never submitted on an early
  // error), so the previous one-shot buffer is idle here: free it so render
  // and pick reuse one slot instead of growing the pool every frame.
  if (impl->offscreen_cmd != nullptr) {
    vkFreeCommandBuffers(impl->device.device(), impl->offscreen_pool, 1,
                         &impl->offscreen_cmd);
    impl->offscreen_cmd = nullptr;
  }
  VkCommandBufferAllocateInfo cai{};
  cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cai.commandPool = impl->offscreen_pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(impl->device.device(), &cai, &cmd_out) !=
          VK_SUCCESS ||
      cmd_out == nullptr) {
    cmd_out = nullptr;
    return false;
  }
  impl->offscreen_cmd = cmd_out;
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  return vkBeginCommandBuffer(cmd_out, &bi) == VK_SUCCESS;
}

// End + fence-waited submit on the graphics queue.
auto OffscreenSubmit(RendererImpl* impl, VkCommandBuffer cmd) noexcept
    -> bool {
  auto vkEndCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer>(
      impl->device.instance_proc("vkEndCommandBuffer"));
  auto vkCreateFence = reinterpret_cast<PFN_vkCreateFence>(
      impl->device.instance_proc("vkCreateFence"));
  auto vkDestroyFence = reinterpret_cast<PFN_vkDestroyFence>(
      impl->device.instance_proc("vkDestroyFence"));
  auto vkWaitForFences = reinterpret_cast<PFN_vkWaitForFences>(
      impl->device.instance_proc("vkWaitForFences"));
  auto vkQueueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(
      impl->device.instance_proc("vkQueueSubmit"));
  if (vkEndCommandBuffer == nullptr || vkCreateFence == nullptr ||
      vkDestroyFence == nullptr || vkWaitForFences == nullptr ||
      vkQueueSubmit == nullptr) {
    return false;
  }
  if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
    return false;
  }
  VkFence fence = nullptr;
  VkFenceCreateInfo fci{};
  fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  bool ok = false;
  if (vkCreateFence(impl->device.device(), &fci, nullptr, &fence) ==
          VK_SUCCESS &&
      fence != nullptr) {
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (vkQueueSubmit(impl->device.graphics_queue(), 1, &si, fence) ==
        VK_SUCCESS) {
      ok = vkWaitForFences(impl->device.device(), 1, &fence, VK_TRUE,
                           UINT64_MAX) == VK_SUCCESS;
    }
    vkDestroyFence(impl->device.device(), fence, nullptr);
  }
  return ok;
}

// (Re)build the color + id + depth targets at width x height: frees the
// old shape first (idempotent), then allocates images, builds views, and
// builds the mesh + pick pipelines (create_mesh fails closed unless the
// color target is UNORM and depth is D32_SFLOAT; create_pick gates the
// R32G32_UINT id target the same way). Any failure tears the partial
// shape down and reports false.
auto OffscreenBuildTargets(RendererImpl* impl, std::uint32_t width,
                           std::uint32_t height) noexcept -> bool {
  OffscreenFreeTarget(impl, impl->offscreen_color_view,
                      impl->offscreen_color_bits,
                      impl->offscreen_color_alloc);
  OffscreenFreeTarget(impl, impl->offscreen_id_view, impl->offscreen_id_bits,
                      impl->offscreen_id_alloc);
  OffscreenFreeTarget(impl, impl->offscreen_depth_view,
                      impl->offscreen_depth_bits,
                      impl->offscreen_depth_alloc);
  impl->offscreen_depth_live = false;
  if (width == 0 || height == 0) {
    return false;
  }
  if (!impl->device.vma_ready()) {
    return false;
  }
  // Process-global VMA allocator (set by device init; see vulkan_device).
  // Reinterpret through the shim (same object + linkage as PickIdBuffer's
  // vma_bridge::g_alloc, without including that renderer-private header).
  auto* vma_alloc = reinterpret_cast<void*>(::engine::renderer::vma_bridge::g_alloc);
  if (vma_alloc == nullptr) {
    return false;
  }
  // Color target first (attaches the lit pass).
  if (!OffscreenAllocImage(vma_alloc, width, height,
                           VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           impl->offscreen_color_bits,
                           impl->offscreen_color_alloc) ||
      !OffscreenMakeView(impl, impl->offscreen_color_bits,
                         VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
                         impl->offscreen_color_view)) {
    OffscreenFreeTarget(impl, impl->offscreen_color_view,
                        impl->offscreen_color_bits,
                        impl->offscreen_color_alloc);
    return false;
  }
  // Id target (attaches the pick pass; create_pick gates the format).
  if (!OffscreenAllocImage(vma_alloc, width, height,
                           VK_FORMAT_R32G32_UINT,
                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           impl->offscreen_id_bits,
                           impl->offscreen_id_alloc) ||
      !OffscreenMakeView(impl, impl->offscreen_id_bits,
                         VK_FORMAT_R32G32_UINT, VK_IMAGE_ASPECT_COLOR_BIT,
                         impl->offscreen_id_view)) {
    OffscreenFreeTarget(impl, impl->offscreen_color_view,
                        impl->offscreen_color_bits,
                        impl->offscreen_color_alloc);
    OffscreenFreeTarget(impl, impl->offscreen_id_view,
                        impl->offscreen_id_bits, impl->offscreen_id_alloc);
    return false;
  }
  // Depth target (shared by both passes).
  if (!OffscreenAllocImage(vma_alloc, width, height,
                           VK_FORMAT_D32_SFLOAT,
                           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                           impl->offscreen_depth_bits,
                           impl->offscreen_depth_alloc) ||
      !OffscreenMakeView(impl, impl->offscreen_depth_bits,
                         VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT,
                         impl->offscreen_depth_view)) {
    OffscreenFreeTarget(impl, impl->offscreen_color_view,
                        impl->offscreen_color_bits,
                        impl->offscreen_color_alloc);
    OffscreenFreeTarget(impl, impl->offscreen_id_view,
                        impl->offscreen_id_bits, impl->offscreen_id_alloc);
    OffscreenFreeTarget(impl, impl->offscreen_depth_view,
                        impl->offscreen_depth_bits,
                        impl->offscreen_depth_alloc);
    return false;
  }
  // Pipelines (fail closed on bad formats; independent of the swapchain).
  // Built once: a resize only reallocates targets (create_mesh/create_pick
  // reject a second build with kAlreadyInitialized).
  if (impl->offscreen_pipelines_live) {
    impl->width = width;
    impl->height = height;
    return true;
  }
  if (!impl->forward
           .create_mesh(impl->device, VK_FORMAT_R8G8B8A8_UNORM,
                        VK_FORMAT_D32_SFLOAT)
           .ok()) {
    OffscreenFreeTarget(impl, impl->offscreen_color_view,
                        impl->offscreen_color_bits,
                        impl->offscreen_color_alloc);
    OffscreenFreeTarget(impl, impl->offscreen_id_view,
                        impl->offscreen_id_bits, impl->offscreen_id_alloc);
    OffscreenFreeTarget(impl, impl->offscreen_depth_view,
                        impl->offscreen_depth_bits,
                        impl->offscreen_depth_alloc);
    return false;
  }
  if (!impl->forward
           .create_pick(impl->device, VK_FORMAT_R32G32_UINT,
                        VK_FORMAT_D32_SFLOAT)
           .ok()) {
    impl->forward.destroy();
    OffscreenFreeTarget(impl, impl->offscreen_color_view,
                        impl->offscreen_color_bits,
                        impl->offscreen_color_alloc);
    OffscreenFreeTarget(impl, impl->offscreen_id_view,
                        impl->offscreen_id_bits, impl->offscreen_id_alloc);
    OffscreenFreeTarget(impl, impl->offscreen_depth_view,
                        impl->offscreen_depth_bits,
                        impl->offscreen_depth_alloc);
    return false;
  }
  impl->offscreen_pipelines_live = true;
  impl->width = width;
  impl->height = height;
  return true;
}

// Tear down one offscreen target (view + image). Null-safe.
auto OffscreenFreeTarget(RendererImpl* impl, VkImageView& view,
                         std::uint64_t& bits, void*& handle) noexcept -> void {
  if (view != nullptr) {
    auto vkDestroyImageView = reinterpret_cast<PFN_vkDestroyImageView>(
        impl->device.instance_proc("vkDestroyImageView"));
    if (vkDestroyImageView != nullptr) {
      vkDestroyImageView(impl->device.device(), view, nullptr);
    }
    view = nullptr;
  }
  if (bits != 0) {
    EngineVmaDestroyImage(::engine::renderer::vma_bridge::g_alloc, bits, handle);
    bits = 0;
    handle = nullptr;
  }
}

// Tear down the whole offscreen shape (targets, pipelines, pool, device).
auto OffscreenTeardown(RendererImpl* impl) noexcept -> void {
  impl->meshes.clear();
  impl->registry.clear();
  impl->forward.destroy();
  impl->offscreen_pipelines_live = false;
  OffscreenFreeTarget(impl, impl->offscreen_color_view,
                      impl->offscreen_color_bits,
                      impl->offscreen_color_alloc);
  OffscreenFreeTarget(impl, impl->offscreen_id_view, impl->offscreen_id_bits,
                      impl->offscreen_id_alloc);
  OffscreenFreeTarget(impl, impl->offscreen_depth_view,
                      impl->offscreen_depth_bits,
                      impl->offscreen_depth_alloc);
  impl->offscreen_depth_live = false;
  if (impl->offscreen_pool != nullptr) {
    auto vkDestroyCommandPool = reinterpret_cast<PFN_vkDestroyCommandPool>(
        impl->device.instance_proc("vkDestroyCommandPool"));
    if (vkDestroyCommandPool != nullptr) {
      vkDestroyCommandPool(impl->device.device(), impl->offscreen_pool,
                           nullptr);
    }
    impl->offscreen_pool = nullptr;
    impl->offscreen_cmd = nullptr;
  }
  impl->device.shutdown();
}

#endif  // ENGINE_RENDERER_HAS_VULKAN

}  // namespace engine::bridge

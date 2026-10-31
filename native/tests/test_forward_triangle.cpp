// test_forward_triangle.cpp — P1-06 acceptance (native CTest).
//
// GPU test: opens a 320x240 window, builds the ForwardRenderer tri
// pipeline, renders one frame (red/green/blue triangle on black via
// record_triangle into the swapchain image), captures the RGBA8 frame,
// and compares against golden triangle_320x240.png (ch ±2, 0.1% pixels).
// Prints:
//
//   [PASS] forward: pipeline built
//   [PASS] golden: triangle_320x240
//
// Rules: SDL_VIDEO_DRIVER=offscreen (ctest property); ENGINE_REQUIRE_GPU=1
// turns no-device paths into [FAIL] (gpu_required.hpp), never [SKIP].
// volk entry points resolve through VulkanDevice::proc()/instance_proc()
// (no second volk copy in the exe — see gotcha 11).
#include <array>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#endif

#include "engine/renderer/forward_renderer.hpp"
#include "engine/renderer/renderer_window.hpp"
#include "engine/renderer/swapchain.hpp"
#include "engine/renderer/vulkan_device.hpp"
#include "golden_compare.hpp"
#include "gpu_required.hpp"

namespace {

auto failures = 0;

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}

using engine::renderer::ForwardRenderer;
using engine::renderer::FrameContext;
using engine::renderer::RendererWindow;
using engine::renderer::StatusCode;
using engine::renderer::Swapchain;
using engine::renderer::VulkanDevice;
using engine::renderer::WindowConfig;

#if defined(ENGINE_RENDERER_HAS_VULKAN)

void test_forward_triangle_gpu() {
  RendererWindow w;
  WindowConfig cfg{};
  cfg.hidden = true;
  cfg.width = 320;
  cfg.height = 240;
  auto wst = w.create(cfg);
  if (!wst.ok()) {
    failures +=
        gpu_missing("forward: offscreen window unavailable (no video device)");
    return;
  }
  check(w.valid() && w.width() == 320 && w.height() == 240,
        "forward: window 320x240");

  std::array<const char*, 64> exts{};
  std::uint32_t got = 0;
  auto xst = w.required_instance_extensions(exts.data(), 64, &got);
  if (!xst.ok() || got == 0) {
    failures += gpu_missing("forward: window instance extensions query");
    w.destroy();
    return;
  }

  VulkanDevice dev;
  engine::renderer::DeviceCreateInfo ci{};
  ci.app_name = "P1-06-test";
  ci.extra_instance_extensions = exts.data();
  ci.extra_instance_extension_count = got;
  ci.enable_swapchain = true;
  auto st = dev.init(ci);
  if (!st.ok() && st.code == StatusCode::kNoPhysicalDevice) {
    failures += gpu_missing("forward: no physical device (no ICD)");
    w.destroy();
    return;
  }
  check(st.ok(), "forward: device init ok");
  if (!st.ok()) {
    std::printf("       init failed: %s\n", st.message);
    w.destroy();
    return;
  }

  VkSurfaceKHR surface = nullptr;
  auto sst = dev.create_surface(w, &surface);
  check(sst.ok() && surface != nullptr, "forward: surface create ok");
  if (!sst.ok() || surface == nullptr) {
    dev.shutdown();
    w.destroy();
    return;
  }

  Swapchain sc;
  engine::renderer::SwapchainDesc desc{};
  desc.width = 320;
  desc.height = 240;
  desc.vsync = true;
  auto cst = sc.create(dev, surface, desc);
  check(cst.ok(), "forward: swapchain create ok");
  if (!cst.ok()) {
    std::printf("       create failed: %s\n", cst.message);
    dev.destroy_surface(surface);
    dev.shutdown();
    w.destroy();
    return;
  }

  ForwardRenderer fr;
  // CPU-side negative gates (run before any GPU work): bad format and
  // bad blobs fail closed without touching the device.
  check(!fr.create(dev, VK_FORMAT_UNDEFINED).ok(),
        "forward: bad format rejected");
  {
    const std::uint32_t bad[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    check(!fr.create(dev, sc.format(), VK_FORMAT_UNDEFINED, bad, 8, bad, 8)
               .ok(),
          "forward: bad blob rejected");
    check(!fr.create(dev, sc.format(), VK_FORMAT_D32_SFLOAT).ok(),
          "forward: depth format rejected (P1-07)");
  }
  check(!fr.ready() && fr.module_count() == 0,
        "forward: failed create leaves idle");
  auto fst = fr.create(dev, sc.format());
  check(fst.ok() && fr.ready() && fr.module_count() == 2 &&
            fr.pipeline() != nullptr && fr.pipeline_layout() != nullptr,
        "forward: pipeline built");
  if (!fst.ok()) {
    std::printf("       forward create failed: %s\n", fst.message);
    sc.destroy();
    dev.destroy_surface(surface);
    dev.shutdown();
    w.destroy();
    return;
  }
  // Double-build is a contract error, not a silent rebuild.
  check(!fr.create(dev, sc.format()).ok(), "forward: double build rejected");
  // Record-time negative gates.
  check(!fr
             .record_triangle(nullptr, sc.view(0),
                              sc.extent())
             .ok(),
        "forward: null cmd rejected");
  {
    VkExtent2D zero{};
    zero.width = 0;
    zero.height = 0;
    check(!fr.record_triangle(nullptr, nullptr, zero).ok(),
          "forward: null record args rejected");
  }

  FrameContext fc;
  auto fcst = fc.create(dev, sc);
  check(fcst.ok(), "forward: frame context create ok");
  if (!fcst.ok()) {
    fr.destroy();
    sc.destroy();
    dev.destroy_surface(surface);
    dev.shutdown();
    w.destroy();
    return;
  }

  // One frame: UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL, record the triangle
  // (own dynamic-rendering pass, CLEAR black), then TRANSFER_SRC for
  // capture/end_frame. Same barrier helper shape as the P1-04 orange
  // clear (barrier2 through dev.proc).
  auto vkCmdPipelineBarrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(
      dev.proc("vkCmdPipelineBarrier2"));
  check(vkCmdPipelineBarrier2 != nullptr, "forward: barrier entry resolves");
  bool framed = false;
  if (vkCmdPipelineBarrier2 != nullptr) {
    auto begin = fc.begin_frame();
    if (begin.ok && begin.cmd != nullptr) {
      VkImage img = sc.image(begin.image_index);
      VkImageView view = sc.view(begin.image_index);
      if (img != nullptr && view != nullptr) {
        VkImageMemoryBarrier2 to_color{};
        to_color.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        to_color.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        to_color.dstStageMask =
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        to_color.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        to_color.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        to_color.image = img;
        to_color.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        to_color.subresourceRange.baseMipLevel = 0;
        to_color.subresourceRange.levelCount = 1;
        to_color.subresourceRange.baseArrayLayer = 0;
        to_color.subresourceRange.layerCount = 1;
        VkDependencyInfo di_to{};
        di_to.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        di_to.imageMemoryBarrierCount = 1;
        di_to.pImageMemoryBarriers = &to_color;
        vkCmdPipelineBarrier2(begin.cmd, &di_to);

        auto rst =
            fr.record_triangle(begin.cmd, view, sc.extent());
        check(rst.ok(), "forward: record ok");
        if (rst.ok()) {
          VkImageMemoryBarrier2 to_copy = to_color;
          to_copy.srcStageMask =
              VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
          to_copy.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
          to_copy.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
          to_copy.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
          to_copy.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
          to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
          VkDependencyInfo di_copy{};
          di_copy.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
          di_copy.imageMemoryBarrierCount = 1;
          di_copy.pImageMemoryBarriers = &to_copy;
          vkCmdPipelineBarrier2(begin.cmd, &di_copy);

          if (fc.end_frame(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL).ok()) {
            framed = true;
            std::vector<std::uint8_t> px;
            if (capture_rgba8(dev, img, sc.format(), sc.extent(),
                              VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, px)
                    .ok() &&
                px.size() == 320u * 240u * 4u) {
              failures += engine::test::golden_check(
                  "triangle_320x240",
                  std::span<const std::uint8_t>(px.data(), px.size()), 320,
                  240);
            } else {
              failures += 1;
              std::printf("[FAIL] golden: triangle_320x240 capture\n");
            }
          }
        }
      }
    }
  }
  if (!framed) {
    failures += 1;
    std::printf("[FAIL] golden: triangle_320x240 frame\n");
  }

  fr.destroy();
  check(!fr.ready(), "forward: destroy idles");
  fr.destroy();  // double-destroy is a safe no-op.
  fc.destroy();
  sc.destroy();
  dev.destroy_surface(surface);
  dev.shutdown();
  w.destroy();
}
#else
void test_forward_triangle_stub() {
  ForwardRenderer fr;
  check(!fr.ready(), "forward: stub idle");
  fr.destroy();
}
#endif

}  // namespace

auto main() -> int {
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  test_forward_triangle_gpu();
#else
  test_forward_triangle_stub();
#endif
  std::printf(failures == 0 ? "FORWARD_TRIANGLE_OK\n"
                            : "FORWARD_TRIANGLE_FAILURES=%d\n",
              failures);
  return failures == 0 ? 0 : 1;
}

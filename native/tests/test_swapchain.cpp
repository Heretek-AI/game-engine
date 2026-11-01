// test_swapchain.cpp — P1-04 acceptance (native CTest).
//
// GPU test: opens a 320x240 window, runs 3 frames clearing to
// (1.0, 0.5, 0.0, 1.0), then prints:
//   [PASS] swapchain: 3 frames presented
//   [PASS] swapchain: pixel(0,0) == 255,128,0,255
//   [PASS] swapchain: recreate 640x360
// plus [PASS] lines for write_png and post-recreate frame health.
//
// Rules: SDL_VIDEO_DRIVER=offscreen (ctest property); ENGINE_REQUIRE_GPU=1
// turns no-device paths into [FAIL] (gpu_required.hpp), never [SKIP].
// volk entry points resolve through VulkanDevice::proc()/instance_proc()
// (no second volk copy in the exe — see gotcha 11).
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#endif

#include "engine/renderer/renderer_window.hpp"
#include "engine/renderer/swapchain.hpp"
#include "engine/renderer/vulkan_device.hpp"
#include "golden_compare.hpp"
#include "gpu_required.hpp"

namespace {

// SWAPCHAIN_PNG overrides the screenshot path. MSVC treats getenv as a
// C4996 error under /WX, so Windows reads it with _dupenv_s.
[[maybe_unused]] auto ScreenshotPath() -> std::string {
#if defined(_WIN32)
  // P7-01: no /tmp on Windows — default into %TEMP% (ctest sets TEMP).
  std::string path;
  char* dup = nullptr;
  std::size_t dup_len = 0;
  if (_dupenv_s(&dup, &dup_len, "TEMP") == 0 && dup != nullptr) {
    path = dup;
    path += "\\swapchain_640x360.png";
  } else {
    path = "swapchain_640x360.png";
  }
  std::free(dup);
  char* png = nullptr;
  std::size_t png_len = 0;
  if (_dupenv_s(&png, &png_len, "SWAPCHAIN_PNG") == 0 && png != nullptr) {
    path = png;
  }
  std::free(png);
#else
  std::string path = "/tmp/swapchain_640x360.png";
  if (const char* env = std::getenv("SWAPCHAIN_PNG"); env != nullptr) {
    path = env;
  }
#endif
  return path;
}

auto failures = 0;

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}

using engine::renderer::FrameContext;
using engine::renderer::RendererWindow;
using engine::renderer::StatusCode;
using engine::renderer::Swapchain;
using engine::renderer::VulkanDevice;
using engine::renderer::WindowConfig;

#if defined(ENGINE_RENDERER_HAS_VULKAN)

struct ClearProcs {
  PFN_vkCmdPipelineBarrier2 barrier2{nullptr};
  PFN_vkCmdBeginRendering begin_rendering{nullptr};
  PFN_vkCmdEndRendering end_rendering{nullptr};
};

auto LoadClearProcs(VulkanDevice& dev) -> ClearProcs {
  ClearProcs p{};
  p.barrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(
      dev.proc("vkCmdPipelineBarrier2"));
  p.begin_rendering = reinterpret_cast<PFN_vkCmdBeginRendering>(
      dev.proc("vkCmdBeginRendering"));
  p.end_rendering =
      reinterpret_cast<PFN_vkCmdEndRendering>(dev.proc("vkCmdEndRendering"));
  return p;
}

// Record an orange clear of `img` into the OPEN command buffer `cb` and
// leave the image in TRANSFER_SRC_OPTIMAL for capture/end_frame.
void RecordOrangeClear(const ClearProcs& p, VkCommandBuffer cb, VkImage img,
                       VkImageView view, VkExtent2D ext) {
  VkImageMemoryBarrier2 to_color{};
  to_color.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
  to_color.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
  to_color.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
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
  p.barrier2(cb, &di_to);

  VkRenderingAttachmentInfo color{};
  color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  color.imageView = view;
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.clearValue.color.float32[0] = 1.0F;
  color.clearValue.color.float32[1] = 0.5F;
  color.clearValue.color.float32[2] = 0.0F;
  color.clearValue.color.float32[3] = 1.0F;
  VkRenderingInfo ri{};
  ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  ri.renderArea.offset.x = 0;
  ri.renderArea.offset.y = 0;
  ri.renderArea.extent = ext;
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &color;
  p.begin_rendering(cb, &ri);
  p.end_rendering(cb);

  VkImageMemoryBarrier2 to_copy = to_color;
  to_copy.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  to_copy.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  to_copy.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  to_copy.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
  to_copy.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  VkDependencyInfo di_copy{};
  di_copy.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
  di_copy.imageMemoryBarrierCount = 1;
  di_copy.pImageMemoryBarriers = &to_copy;
  p.barrier2(cb, &di_copy);
}

// Render ONE orange frame on the current swapchain image and capture the
// full RGBA8 buffer AFTER present (end_frame leaves the image in
// PRESENT_SRC; the capture copies the executed clear and restores the
// layout). Returns true only when the frame presented AND the pixel
// buffer captured. `fail_tag` prefixes the diagnostic prints.
// Used by the golden block and the post-recreate block below.
bool RenderOrangeFrame(FrameContext& fc, Swapchain& sc, VulkanDevice& dev,
                       const ClearProcs& p, std::vector<std::uint8_t>& px_out,
                       const char* fail_tag) {
  auto begin = fc.begin_frame();
  if (!begin.ok) {
    std::printf("       %s begin_frame failed\n", fail_tag);
    return false;
  }
  VkImage img = sc.image(begin.image_index);
  VkImageView view = sc.view(begin.image_index);
  if (img == nullptr || view == nullptr || begin.cmd == nullptr) {
    return false;
  }
  RecordOrangeClear(p, begin.cmd, img, view, sc.extent());
  auto est = fc.end_frame(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  if (!est.ok()) {
    std::printf("       %s end_frame failed: %s\n", fail_tag, est.message);
    return false;
  }
  auto cap = engine::renderer::capture_rgba8(
      dev, img, sc.format(), sc.extent(),
      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, px_out);
  if (!cap.ok()) {
    std::printf("       %s capture failed: %s\n", fail_tag, cap.message);
    return false;
  }
  return true;
}

void test_swapchain_gpu() {
  RendererWindow w;
  WindowConfig cfg{};
  cfg.hidden = true;
  cfg.width = 320;
  cfg.height = 240;
  auto wst = w.create(cfg);
  if (!wst.ok()) {
    bool env_no_video = false;
    for (const char* m = wst.message; *m != '\0'; ++m) {
      if ((m[0] == 'v' || m[0] == 'V') && m[1] == 'i' && m[2] == 'd' &&
          m[3] == 'e' && m[4] == 'o') {
        env_no_video = true;
      }
    }
    if (env_no_video) {
      failures +=
          gpu_missing("swapchain: offscreen window unavailable (no video device)");
      return;
    }
    check(false, "swapchain: window create ok");
    return;
  }
  check(w.valid() && w.width() == 320 && w.height() == 240,
        "swapchain: window 320x240");

  std::array<const char*, 64> exts{};
  std::uint32_t got = 0;
  auto xst = w.required_instance_extensions(exts.data(), 64, &got);
  if (!xst.ok() || got == 0) {
    failures += gpu_missing("swapchain: window instance extensions query");
    w.destroy();
    return;
  }

  VulkanDevice dev;
  engine::renderer::DeviceCreateInfo ci{};
  ci.app_name = "P1-04-test";
  ci.extra_instance_extensions = exts.data();
  ci.extra_instance_extension_count = got;
  ci.enable_swapchain = true;
  auto st = dev.init(ci);
  // P7-02: the device-name line proves the portability path on macOS:
  // without VK_KHR_portability_enumeration the MoltenVK ICD stays hidden
  // and this branch reports "no physical device". The macOS CI
  // acceptance ("log shows a Vulkan device") greps for this line.
  std::printf("       portability: enumerate=%s device=%s\n",
              "portability_enumeration",
              st.ok() ? dev.device_name() : "<none>");
  if (!st.ok() && st.code == StatusCode::kNoPhysicalDevice) {
    failures += gpu_missing("swapchain: no physical device (no ICD)");
    w.destroy();
    return;
  }
  check(st.ok(), "swapchain: device init ok");
  if (!st.ok()) {
    std::printf("       init failed: %s\n", st.message);
    w.destroy();
    return;
  }
  check(dev.proc("vkCreateSwapchainKHR") != nullptr,
        "swapchain: swapchain ext enabled");

  VkSurfaceKHR surface = nullptr;
  auto sst = dev.create_surface(w, &surface);
  check(sst.ok() && surface != nullptr, "swapchain: surface create ok");
  if (!sst.ok() || surface == nullptr) {
    dev.shutdown();
    w.destroy();
    return;
  }
  check(dev.supports_present(surface), "swapchain: queue supports present");

  Swapchain sc;
  engine::renderer::SwapchainDesc desc{};
  desc.width = 320;
  desc.height = 240;
  desc.vsync = true;
  auto cst = sc.create(dev, surface, desc);
  check(cst.ok(), "swapchain: create ok");
  if (!cst.ok()) {
    std::printf("       create failed: %s\n", cst.message);
    dev.destroy_surface(surface);
    dev.shutdown();
    w.destroy();
    return;
  }
  check(sc.image_count() >= 2, "swapchain: image count >= 2");
  check(sc.extent().width == 320 && sc.extent().height == 240,
        "swapchain: extent 320x240");
  bool fmt_ok = (sc.format() == VK_FORMAT_B8G8R8A8_UNORM) ||
                (sc.format() == VK_FORMAT_R8G8B8A8_UNORM);
  check(fmt_ok, "swapchain: format BGRA/RGBA8");
  check(sc.image(0) != nullptr && sc.view(0) != nullptr,
        "swapchain: image+view live");
  check(sc.image(sc.image_count()) == nullptr, "swapchain: oob image null");

  FrameContext fc;
  check(FrameContext::kFramesInFlight == 2, "swapchain: 2 frames in flight");
  auto fst = fc.create(dev, sc);
  check(fst.ok(), "swapchain: frame context create ok");
  if (!fst.ok()) {
    std::printf("       frame create failed: %s\n", fst.message);
    sc.destroy();
    dev.destroy_surface(surface);
    dev.shutdown();
    w.destroy();
    return;
  }
  ClearProcs procs = LoadClearProcs(dev);
  check(procs.barrier2 != nullptr && procs.begin_rendering != nullptr,
        "swapchain: entry points resolve");

  // 3 frames: begin -> record orange clear (ends in TRANSFER_SRC) ->
  // end_frame presents (PRESENT_SRC) -> capture pixel check AFTER present
  // on frame 0 from PRESENT_SRC (capture restores PRESENT_SRC).
  int presented = 0;
  bool pixel_ok = false;
  bool loop_ok = true;
  for (int frame = 0; frame < 3; ++frame) {
    auto begin = fc.begin_frame();
    if (!begin.ok) {
      loop_ok = false;
      break;
    }
    VkImage img = sc.image(begin.image_index);
    VkImageView view = sc.view(begin.image_index);
    if (img == nullptr || view == nullptr || begin.cmd == nullptr) {
      loop_ok = false;
      break;
    }
    RecordOrangeClear(procs, begin.cmd, img, view, sc.extent());
    auto est = fc.end_frame(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (!est.ok()) {
      std::printf("       end_frame %d failed: %s\n", frame, est.message);
      loop_ok = false;
      break;
    }
    ++presented;
    if (frame == 2) {
      // Capture AFTER the clear has executed (presented at least once):
      // image is in PRESENT_SRC now; capture copies and restores it.
      std::vector<std::uint8_t> px;
      auto cap = engine::renderer::capture_rgba8(
          dev, img, sc.format(), sc.extent(),
          VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, px);
      if (cap.ok() && px.size() >= 4) {
        std::printf("       pixel0 bytes: %u %u %u %u (format %d)\n", px[0],
                    px[1], px[2], px[3], static_cast<int>(sc.format()));
        pixel_ok = (px[0] == 255 && px[1] == 128 && px[2] == 0 && px[3] == 255);
      } else if (!cap.ok()) {
        std::printf("       capture failed: %s\n", cap.message);
      }
    }
  }
  check(loop_ok && presented == 3, "swapchain: 3 frames presented");
  check(pixel_ok, "swapchain: pixel(0,0) == 255,128,0,255");

  // P1-05: golden of the 320x240 orange frame. Re-render one frame at the
  // ORIGINAL extent (do not disturb the recreate section below), capture
  // the full RGBA8 buffer, and compare against
  // native/tests/golden/clear_orange_320x240.png (ch ±2, 0.1% pixels).
  // golden_check prints [PASS]/[FAIL] golden: <name> itself and folds
  // its result into `failures`, so no extra check() line here.
  {
    std::vector<std::uint8_t> px;
    if (RenderOrangeFrame(fc, sc, dev, procs, px, "golden") &&
        px.size() == 320u * 240u * 4u) {
      failures += engine::test::golden_check(
          "clear_orange_320x240",
          std::span<const std::uint8_t>(px.data(), px.size()), 320, 240);
    } else {
      failures += 1;
      std::printf("[FAIL] golden: clear_orange_320x240 frame\n");
    }
  }

  // Recreate at 640x360, one more frame, pixel check holds. On Win32 the
  // surface reports currentExtent from the window, so resize the window
  // first — otherwise build() keeps 320x240 and the extent guard below
  // (correctly) refuses to call it a 640x360 recreate.
  auto wst3 = w.resize(640, 360);
  check(wst3.ok(), "swapchain: window resize 640x360");
  auto rst = sc.recreate(640, 360);
  check(rst.ok(), "swapchain: recreate ok");
  if (!rst.ok()) {
    std::printf("       recreate failed: %s\n", rst.message);
  }
  bool re_ok = false;
  if (rst.ok() && sc.extent().width == 640 && sc.extent().height == 360 &&
      sc.image_count() >= 2) {
    // External recreate replaced the swapchain images: resync the frame
    // context (fences/semaphores were created against the old swapchain).
    // NOTE: surface + Swapchain object stay alive — sc.recreate() preserves
    // the surface (Win32 present path verified: surface create ok before AND
    // after); destroying the surface here would invalidate the swapchain.
    fc.destroy();
    auto fc_rst = fc.create(dev, sc);
    check(fc_rst.ok(), "swapchain: frame context recreate ok");
    if (!fc_rst.ok()) {
      std::printf("       frame context recreate failed: %s\n", fc_rst.message);
    } else {
      procs = LoadClearProcs(dev);
      check(procs.barrier2 != nullptr && procs.begin_rendering != nullptr,
            "swapchain: entry points re-resolve");
    }
    std::vector<std::uint8_t> px;
    if (RenderOrangeFrame(fc, sc, dev, procs, px, "post-recreate") &&
        px.size() == 640u * 360u * 4u && px[0] == 255 && px[1] == 128 &&
        px[2] == 0 && px[3] == 255) {
      re_ok = true;
      // Persist one screenshot through write_png (P1-05 golden input).
      const std::string out = ScreenshotPath();
      auto wst2 = engine::renderer::write_png(out.c_str(), px, 640, 360);
      check(wst2.ok(), "swapchain: write_png ok");
    } else if (!px.empty()) {
      std::printf("       post-recreate pixel mismatch: %u %u %u %u size=%zu\n",
                  px[0], px[1], px[2], px[3], px.size());
    }
  }
  check(re_ok, "swapchain: recreate 640x360");

  fc.destroy();
  sc.destroy();
  dev.destroy_surface(surface);
  dev.shutdown();
  w.destroy();
}
#else
void test_swapchain_stub() {
  Swapchain sc;
  check(sc.image_count() == 0, "swapchain: stub empty");
  VulkanDevice* null_dev = nullptr;
  std::vector<std::uint8_t>* null_px = nullptr;
  check(!engine::renderer::capture_rgba8(*null_dev, nullptr, 0, {}, 0, *null_px)
             .ok(),
        "swapchain: stub capture fails");
}
#endif

}  // namespace

auto main() -> int {
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  test_swapchain_gpu();
#else
  test_swapchain_stub();
#endif
  std::printf(failures == 0 ? "SWAPCHAIN_OK\n" : "SWAPCHAIN_FAILURES=%d\n", failures);
  return failures == 0 ? 0 : 1;
}

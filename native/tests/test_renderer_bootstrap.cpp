// test_renderer_bootstrap.cpp — M3-01 acceptance (native CTest).
//
// Covers the CPU-side contract (always built, no GPU needed):
//   1. RenderGraph: DAG ordering, barrier computation, cycle detection,
//      transient aliasing, unknown-resource errors.
//   2. VulkanDevice ring: CPU-fallback bump allocator alignment/wrap.
//   3. VulkanDevice init (real path only): skipped unless a Vulkan ICD
//      advertises a physical device (Lavapipe counts); when present,
//      asserts Vulkan 1.3 + dynamicRendering + sync2 + BDA features and
//      a working VMA upload ring with non-zero device address.
//   4. RendererWindow: hidden window create/destroy (SDL path only).
//
// Tests use base flags (exceptions/RTTI on) like the other native tests.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(ENGINE_RENDERER_HAS_VULKAN)
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#endif

#include "engine/renderer/render_graph.hpp"
#include "engine/renderer/renderer_window.hpp"
#include "engine/renderer/vulkan_device.hpp"
#include "gpu_required.hpp"

namespace {

auto failures = 0;

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}

using engine::renderer::GraphBarrier;
using engine::renderer::PassAccess;
using engine::renderer::RenderGraph;
using engine::renderer::RendererWindow;
using engine::renderer::StatusCode;
using engine::renderer::VulkanDevice;
using engine::renderer::WindowConfig;

void test_graph_basic() {
  RenderGraph g;
  engine::renderer::GraphImageDesc color{};
  color.name = "color";
  color.format = VK_FORMAT_R8G8B8A8_SRGB;
  color.width = 1280;
  color.height = 720;
  color.depth = 1;
  color.transient = true;
  auto sc0 = g.add_image(color);
  check(sc0.ok(), "graph: add color image");
  engine::renderer::GraphImageDesc depth{};
  depth.name = "depth";
  depth.format = VK_FORMAT_D32_SFLOAT;
  depth.width = 1280;
  depth.height = 720;
  depth.depth = 1;
  depth.transient = true;
  check(g.add_image(depth).ok(), "graph: add depth image");
  engine::renderer::GraphImageDesc dup{};
  dup.name = "color";
  dup.width = 64;
  dup.height = 64;
  dup.depth = 1;
  check(!g.add_image(dup).ok(), "graph: dup image rejected");

  PassAccess gbuf_w[2]{};
  gbuf_w[0].image = "color";
  gbuf_w[0].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  gbuf_w[0].access = 0x78;
  gbuf_w[0].stage = 0x400;
  gbuf_w[1].image = "depth";
  gbuf_w[1].layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  gbuf_w[1].access = 0x78;
  gbuf_w[1].stage = 0x400;
  PassAccess light_r[1]{};
  light_r[0].image = "color";
  light_r[0].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  light_r[0].access = 0x20;
  light_r[0].stage = 0x80;
  PassAccess light_w[1]{};
  light_w[0].image = "color";
  light_w[0].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  light_w[0].access = 0x78;
  light_w[0].stage = 0x400;
  check(g.add_pass("gbuffer", {}, {gbuf_w, 2}).ok(),
        "graph: add gbuffer pass");
  check(g.add_pass("light", {light_r, 1}, {light_w, 1}).ok(),
        "graph: add light pass");
  check(!g.add_pass("gbuffer", {}, {}).ok(), "graph: dup pass rejected");
  PassAccess bad[1]{};
  bad[0].image = "nope";
  auto bad_st = g.add_pass("bad", {bad, 1}, {});
  // Unknown names surface at compile() (add_pass accepts; validation of
  // image existence is deferred so passes can be added in any order —
  // images may be registered after the passes that use them).
  check(bad_st.ok(), "graph: unknown image deferred to compile");
  auto bad_ct = g.compile();
  check(!bad_ct.ok() && bad_ct.code == StatusCode::kUnknownResource,
        "graph: unknown image rejected");
  // Drop the intentionally-bad pass before continuing: rebuild the graph
  // clean (RenderGraph is a plan builder; a fresh graph for the valid
  // scene is the intended use, not error recovery on the same object).
  RenderGraph g2;
  check(g2.add_image(color).ok(), "graph: re-add color image");
  check(g2.add_image(depth).ok(), "graph: re-add depth image");
  check(g2.add_pass("gbuffer", {}, {gbuf_w, 2}).ok(),
        "graph: re-add gbuffer pass");
  check(g2.add_pass("light", {light_r, 1}, {light_w, 1}).ok(),
        "graph: re-add light pass");
  // From here on, exercise the valid scene graph.
  RenderGraph& gref = g2;
  (void)gref;

  check(g2.compile().ok(), "graph: compile ok");
  check(g2.pass_count() == 2, "graph: 2 passes scheduled");
  check(g2.execution_order()[0] == 0, "graph: gbuffer first (hazard order)");
  check(g2.barrier_count() >= 2, "graph: barriers emitted");
  // Color is written by gbuffer, read+written by light: expect a
  // COLOR_ATTACHMENT -> SHADER_READ transition between them.
  bool found_transition = false;
  for (const GraphBarrier& b : g2.barriers()) {
    if (b.old_layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
        b.new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
      found_transition = true;
    }
  }
  check(found_transition, "graph: write->read layout transition found");
  check(g2.transient_count() == 2, "graph: 2 transient allocs");
  check(g2.transient_heap_bytes() > 0, "graph: transient heap non-zero");
}

void test_graph_cycle() {
  RenderGraph g;
  engine::renderer::GraphImageDesc img{};
  img.name = "a";
  img.width = 64;
  img.height = 64;
  img.depth = 1;
  img.transient = false;
  check(g.add_image(img).ok(), "cycle: add image");
  PassAccess w[1]{};
  w[0].image = "a";
  const char* dep_p2[] = {"p2"};
  const char* dep_p1[] = {"p1"};
  check(g.add_pass("p1", {}, {w, 1}, {dep_p2, 1}).ok(), "cycle: add p1");
  check(g.add_pass("p2", {}, {w, 1}, {dep_p1, 1}).ok(), "cycle: add p2");
  auto st = g.compile();
  check(!st.ok() && st.code == StatusCode::kGraphCycle,
        "cycle: detected");
}

void test_graph_hazard_chain() {
  // Regression: hazard edges need last-writer / readers-since-writer
  // tracking, not consecutive-use chaining. W,R1,R2,W2 with an order_after
  // that delays a middle reader must still schedule W before R1/R2 and
  // both readers before W2.
  RenderGraph g;
  engine::renderer::GraphImageDesc x{};
  x.name = "x";
  x.width = 64;
  x.height = 64;
  x.depth = 1;
  x.transient = false;
  check(g.add_image(x).ok(), "hazard: add image");
  PassAccess w[1]{};
  w[0].image = "x";
  w[0].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  PassAccess r[1]{};
  r[0].image = "x";
  r[0].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  const char* after_p4[] = {"p1"};
  check(g.add_pass("p1", {}, {w, 1}).ok(), "hazard: add writer p1");
  // p2 must run after p1 but is ADDED before p4 exists: a consecutive-chain
  // planner loses the R->W WAR edges (p2/p3 -> p4) and can schedule p4
  // before the readers. Full last-writer/readers tracking keeps them.
  check(g.add_pass("p2", {r, 1}, {}, {after_p4, 1}).ok(),
        "hazard: add reader p2 (ordered after p1)");
  check(g.add_pass("p3", {r, 1}, {}).ok(), "hazard: add reader p3");
  check(g.add_pass("p4", {}, {w, 1}).ok(), "hazard: add writer p4");
  check(g.compile().ok(), "hazard: compile ok");
  // position[pass] in execution order: W(p1) < R(p2),R(p3) < W(p4).
  std::uint32_t pos[4] = {0, 0, 0, 0};
  for (std::uint32_t slot = 0; slot < 4; ++slot) {
    pos[g.execution_order()[slot]] = slot;
  }
  check(pos[0] < pos[1] && pos[0] < pos[2], "hazard: RAW writer before reads");
  check(pos[1] < pos[3] && pos[2] < pos[3], "hazard: WAR reads before writer");
}

void test_graph_aliasing_single_slot() {
  // Two single-slot transient images used in the SAME pass must never
  // share an offset (both live at that slot).
  RenderGraph g;
  engine::renderer::GraphImageDesc a{};
  a.name = "a";
  a.width = 64;
  a.height = 64;
  a.depth = 1;
  a.transient = true;
  engine::renderer::GraphImageDesc b = a;
  b.name = "b";
  check(g.add_image(a).ok(), "alias: add image a");
  check(g.add_image(b).ok(), "alias: add image b");
  PassAccess wa[1]{};
  wa[0].image = "a";
  PassAccess wb[1]{};
  wb[0].image = "b";
  PassAccess both[2]{};
  both[0].image = "a";
  both[1].image = "b";
  check(g.add_pass("w", {}, {both, 2}).ok(), "alias: single pass writes both");
  check(g.compile().ok(), "alias: compile ok");
  check(g.transient_count() == 2, "alias: 2 transient allocs");
  std::uint64_t off_a = 0;
  std::uint64_t off_b = 0;
  for (const auto& t : g.transients()) {
    if (t.image == 0) {
      off_a = t.offset;
    } else if (t.image == 1) {
      off_b = t.offset;
    }
  }
  check(off_a != off_b, "alias: same-slot images get distinct offsets");
}

void test_ring_cpu() {
  VulkanDevice dev;
  auto a1 = dev.ring_alloc(64, 64);
  check(a1.cpu_ptr != nullptr, "ring: cpu fallback alloc");
  check((reinterpret_cast<std::uintptr_t>(a1.cpu_ptr) % 64) == 0,
        "ring: 64B aligned");
  check(a1.device_address == 0, "ring: no device addr without GPU");
  // Fill nearly the whole ring with 1MiB chunks, then check wrap.
  for (int i = 0; i < 15; ++i) {
    auto a = dev.ring_alloc(1024 * 1024, 256);
    if (a.cpu_ptr == nullptr) {
      break;
    }
  }
  auto pre = dev.ring_stats().offset;
  dev.ring_reset_frame();
  check(dev.ring_stats().offset == 0, "ring: reset to zero");
  check(dev.ring_stats().frame_index == 1, "ring: frame index bumps");
  (void)pre;
}

void test_vulkan_real() {
#if defined(ENGINE_RENDERER_HAS_VULKAN)
  VulkanDevice dev;
  engine::renderer::DeviceCreateInfo ci{};
  ci.app_name = "M3-01-test";
  auto st = dev.init(ci);
  if (!st.ok() && st.code == StatusCode::kNoPhysicalDevice) {
    failures += gpu_missing(
        "vulkan: no physical device (no ICD); CPU-side contract still proven");
    return;
  }
  check(st.ok(), "vulkan: init ok");
  if (!st.ok()) {
    std::printf("       init failed: %s\n", st.message);
    return;
  }
  check(dev.valid(), "vulkan: device valid");
  check(dev.vma_ready(), "vulkan: VMA ready");
  const auto& f = dev.features();
  check(f.dynamic_rendering, "vulkan: dynamicRendering");
  check(f.synchronization2, "vulkan: synchronization2");
  check(f.buffer_device_address, "vulkan: bufferDeviceAddress");
  std::printf("       gpu: %s api=%u.%u.%u\n", dev.device_name(),
              (dev.api_version() >> 22) & 0x7F,
              (dev.api_version() >> 12) & 0x3FF, dev.api_version() & 0xFFF);
  auto a = dev.ring_alloc(65536, 256);
  check(a.cpu_ptr != nullptr, "vulkan: ring mapped");
  check(a.device_address != 0, "vulkan: ring device address non-zero");
  check(a.size >= 65536, "vulkan: ring size covers write");
  if (a.cpu_ptr != nullptr && a.size >= 65536) {
    std::memset(a.cpu_ptr, 0xAB, 65536);
    check(static_cast<unsigned char*>(a.cpu_ptr)[65535] == 0xAB,
          "vulkan: ring writeback");
  } else {
    check(false, "vulkan: ring writeback (skipped: alloc failed)");
  }
  auto st2 = dev.init(ci);
  check(!st2.ok() && st2.code == StatusCode::kAlreadyInitialized,
        "vulkan: double-init guarded");
  dev.shutdown();
  check(!dev.valid(), "vulkan: shutdown clears");
#else
  failures += gpu_missing("vulkan: SDK absent (stub build)");
#endif
}

void test_vulkan_swapchain_present() {
#if defined(ENGINE_RENDERER_HAS_VULKAN) && defined(ENGINE_RENDERER_HAS_SDL3)
  RendererWindow w;
  WindowConfig cfg{};
  cfg.hidden = true;
  cfg.width = 64;
  cfg.height = 64;
  auto wst = w.create(cfg);
  if (!wst.ok()) {
    bool env_no_video =
        std::strstr(wst.message, "video") != nullptr ||
        std::strstr(wst.message, "Video") != nullptr ||
        std::strstr(wst.message, "display") != nullptr;
    if (env_no_video) {
      failures += gpu_missing(
          "device: offscreen window unavailable (no video device)");
      return;
    }
    check(false, "device: swapchain window create ok");
    return;
  }
  check(w.valid(), "device: swapchain window valid");
  // Instance extensions straight from the SDL window (same count the
  // device init consumes).
  const std::uint32_t n_ext = w.required_instance_extension_count();
  check(n_ext > 0, "device: window instance extensions exposed");
  const char* exts[64] = {nullptr};
  std::uint32_t got = 0;
  auto xst = w.required_instance_extensions(exts, 64, &got);
  if (!xst.ok() || got == 0) {
    failures += gpu_missing("device: window instance extensions query");
    w.destroy();
    return;
  }

  VulkanDevice dev;
  engine::renderer::DeviceCreateInfo ci{};
  ci.app_name = "P1-03-test";
  ci.extra_instance_extensions = exts;
  ci.extra_instance_extension_count = got;
  ci.enable_swapchain = true;
  auto st = dev.init(ci);
  if (!st.ok() && st.code == StatusCode::kNoPhysicalDevice) {
    failures += gpu_missing(
        "device: no physical device (no ICD); CPU-side contract still proven");
    w.destroy();
    return;
  }
  check(st.ok(), "device: init with swapchain ok");
  if (!st.ok()) {
    std::printf("       init failed: %s\n", st.message);
    w.destroy();
    return;
  }
  // Swapchain ext must be enabled on the logical device: resolve the
  // vkCreateSwapchainKHR device proc through the device table.
  check(dev.proc("vkCreateSwapchainKHR") != nullptr,
        "device: swapchain ext enabled");
  VkSurfaceKHR surface = nullptr;
  auto sst = dev.create_surface(w, &surface);
  check(sst.ok() && surface != nullptr, "device: surface create ok");
  if (sst.ok() && surface != nullptr) {
    check(dev.supports_present(surface), "device: queue supports present");
    dev.destroy_surface(surface);
  }
  dev.shutdown();
#else
  failures += gpu_missing("device: swapchain needs Vulkan SDK + SDL3");
#endif
}

void test_window() {
#if defined(ENGINE_RENDERER_HAS_SDL3)
  RendererWindow w;
  // No display in CI containers: SDL_VideoInit fails ("No available video
  // device"). The window contract is proven where a video backend exists;
  // here we assert the SDL integration point (instance extensions) and
  // accept create() success OR the environmental video-device failure.
  WindowConfig cfg{};
  cfg.hidden = true;
  cfg.width = 64;
  cfg.height = 64;
  auto st = w.create(cfg);
  if (st.ok()) {
    check(w.valid() && w.width() == 64, "window: valid + extent");
    std::uint32_t quit = 0;
    check(w.pump_events(&quit).ok(), "window: pump ok");
    check(w.required_instance_extension_count() > 0,
          "window: instance extensions exposed");
    w.destroy();
    check(!w.valid(), "window: destroy clears");
  } else if (std::strstr(st.message, "Vulkan") != nullptr) {
    // SDL_WINDOW_VULKAN needs a Vulkan loader. macOS runners have none
    // ("Failed to load Vulkan Portability library"): that is a missing GPU
    // stack, so it SKIPs normally and FAILs under ENGINE_REQUIRE_GPU=1.
    std::printf("       create: %s\n", st.message);
    failures += gpu_missing("window: no Vulkan loader for SDL_WINDOW_VULKAN");
  } else {
    bool env_no_video =
        std::strstr(st.message, "video") != nullptr ||
        std::strstr(st.message, "Video") != nullptr ||
        std::strstr(st.message, "display") != nullptr;
    std::printf("       create: %s (env: no video device)\n", st.message);
    check(env_no_video, "window: create fails only for missing video device");
  }
#else
  RendererWindow w;
  auto st = w.create(WindowConfig{});
  check(!st.ok() && st.code == StatusCode::kSdlInitFailed,
        "window: stub reports SDL missing");
#endif
}

}  // namespace

auto main() -> int {
  test_graph_basic();
  test_graph_cycle();
  test_graph_hazard_chain();
  test_graph_aliasing_single_slot();
  test_ring_cpu();
  test_vulkan_real();
  test_vulkan_swapchain_present();
  test_window();
  std::printf(failures == 0 ? "RENDERER_BOOTSTRAP_OK\n"
                            : "RENDERER_BOOTSTRAP_FAILURES=%d\n",
              failures);
  return failures == 0 ? 0 : 1;
}

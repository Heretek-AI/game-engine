// renderer_window.cpp — M3-01: SDL3 window implementation.
//
// NOTE: in the real build (ENGINE_RENDERER_HAS_VULKAN) volk.h must come
// FIRST: volk defines VK_NO_PROTOTYPES itself, but it hard-errors if
// <vulkan/vulkan.h> was already included. SDL3's SDL_vulkan.h pulls
// vulkan headers, so IWYU order is volk -> SDL here (clang-format
// may want to re-sort: do not let it).
// With ENGINE_RENDERER_HAS_SDL3: real SDL3 window (SDL_WINDOW_VULKAN).
// Without it: every create() reports kSdlInitFailed; other methods are
// safe no-ops, keeping the renderer linkable/testable without SDL3.
#if defined(ENGINE_RENDERER_HAS_VULKAN)
#include <volk.h>
#endif
#include "engine/renderer/renderer_window.hpp"

#include <cstring>

#if defined(ENGINE_RENDERER_HAS_SDL3)
#include <new>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#endif

namespace engine::renderer {

struct NativeWindow {
#if defined(ENGINE_RENDERER_HAS_SDL3)
  struct SDL_Window* handle{nullptr};
#else
  int unused{0};
#endif
};

RendererWindow::~RendererWindow() noexcept { destroy(); }

auto RendererWindow::create(const WindowConfig& cfg) noexcept -> Status {
#if !defined(ENGINE_RENDERER_HAS_SDL3)
  (void)cfg;
  return MakeFail(StatusCode::kSdlInitFailed,
                  "SDL3 unavailable (ENGINE_RENDERER_HAS_SDL3 undefined)");
#else
  if (window_ != nullptr) {
    return MakeFail(StatusCode::kAlreadyInitialized, "window already created");
  }
  if (cfg.width == 0 || cfg.height == 0) {
    return MakeFail(StatusCode::kInvalidArgument, "window extent must be non-zero");
  }
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    return MakeFail(StatusCode::kSdlInitFailed, SDL_GetError());
  }
  SDL_WindowFlags flags = SDL_WINDOW_VULKAN;
  if (cfg.hidden) {
    flags = static_cast<SDL_WindowFlags>(flags | SDL_WINDOW_HIDDEN);
  }
  if (cfg.resizable) {
    flags = static_cast<SDL_WindowFlags>(flags | SDL_WINDOW_RESIZABLE);
  }
  const char* title = (cfg.title != nullptr) ? cfg.title : "GameEngine";
  SDL_Window* w = SDL_CreateWindow(title, static_cast<int>(cfg.width),
                                   static_cast<int>(cfg.height), flags);
  if (w == nullptr) {
    const char* err = SDL_GetError();
    char buf[256];
    std::snprintf(buf, sizeof(buf), "SDL_CreateWindow: %s",
                  (err != nullptr) ? err : "unknown");
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return MakeFail(StatusCode::kWindowCreateFailed, buf);
  }
  auto* wrap = new (std::nothrow) NativeWindow{};
  if (wrap == nullptr) {
    SDL_DestroyWindow(w);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return MakeFail(StatusCode::kOutOfMemory, "window wrapper alloc failed");
  }
  wrap->handle = w;
  window_ = wrap;
  width_ = cfg.width;
  height_ = cfg.height;
  return MakeOk();
#endif
}

auto RendererWindow::destroy() noexcept -> void {
#if defined(ENGINE_RENDERER_HAS_SDL3)
  if (window_ != nullptr) {
    if (window_->handle != nullptr) {
      SDL_DestroyWindow(window_->handle);
    }
    delete window_;
    window_ = nullptr;
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
  }
#else
  window_ = nullptr;
#endif
  width_ = 0;
  height_ = 0;
}

auto RendererWindow::required_instance_extension_count() const noexcept
    -> std::uint32_t {
#if !defined(ENGINE_RENDERER_HAS_SDL3)
  return 0;
#else
  if (window_ == nullptr || window_->handle == nullptr) {
    return 0;
  }
  std::uint32_t n = 0;
  const char* const* exts =
      SDL_Vulkan_GetInstanceExtensions(&n);  // NOLINT: SDL owns storage
  (void)exts;
  return n;
#endif
}

auto RendererWindow::required_instance_extensions(
    const char** out, std::uint32_t cap, std::uint32_t* count_out) const noexcept
    -> Status {
#if !defined(ENGINE_RENDERER_HAS_SDL3)
  (void)out;
  (void)cap;
  if (count_out != nullptr) {
    *count_out = 0;
  }
  return MakeFail(StatusCode::kNotInitialized, "SDL3 unavailable");
#else
  std::uint32_t n = 0;
  const char* const* exts = SDL_Vulkan_GetInstanceExtensions(&n);
  if (count_out != nullptr) {
    *count_out = n;
  }
  if (n == 0 || exts == nullptr) {
    return MakeFail(StatusCode::kSurfaceCreateFailed,
                    "no Vulkan instance extensions from SDL");
  }
  if (out == nullptr || cap < n) {
    return MakeFail(StatusCode::kInvalidArgument,
                    "extension out-buffer too small");
  }
  for (std::uint32_t i = 0; i < n; ++i) {
    out[i] = exts[i];
  }
  return MakeOk();
#endif
}

auto RendererWindow::pump_events(std::uint32_t* quit_out) noexcept
    -> Status {
#if !defined(ENGINE_RENDERER_HAS_SDL3)
  if (quit_out != nullptr) {
    *quit_out = 0;
  }
  return MakeOk();
#else
  std::uint32_t quit = 0;
  SDL_Event ev;
  while (SDL_PollEvent(&ev)) {
    if (ev.type == SDL_EVENT_QUIT) {
      quit = 1;
    }
  }
  if (quit_out != nullptr) {
    *quit_out = quit;
  }
  return MakeOk();
#endif
}

auto RendererWindow::sdl_handle() const noexcept -> void* {
#if !defined(ENGINE_RENDERER_HAS_SDL3)
  return nullptr;
#else
  if (window_ == nullptr) {
    return nullptr;
  }
  return static_cast<void*>(window_->handle);
#endif
}

}  // namespace engine::renderer

// Surface creation from the opaque SDL handle (called by VulkanDevice).
auto RendererWindowCreateSurface(const engine::renderer::RendererWindow& window,
                                 void* instance, void* surface_out) noexcept
    -> bool {
#if !defined(ENGINE_RENDERER_HAS_SDL3) || \
    !defined(ENGINE_RENDERER_HAS_VULKAN)
  (void)window;
  (void)instance;
  (void)surface_out;
  return false;
#else
  void* sdl_win = window.sdl_handle();
  if (sdl_win == nullptr || surface_out == nullptr) {
    return false;
  }
  VkSurfaceKHR surf = nullptr;
  if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(sdl_win),
                                static_cast<VkInstance>(instance), nullptr,
                                &surf) ||
      surf == nullptr) {
    return false;
  }
  *static_cast<VkSurfaceKHR*>(surface_out) = surf;
  return true;
#endif
}

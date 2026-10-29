// renderer_window.hpp — M3-01: SDL3 window + Vulkan surface management.
//
// RendererWindow owns an SDL3 `SDL_Window` created with SDL_WINDOW_VULKAN
// and exposes the Vulkan surface creation helpers. The SDL3 dependency is
// optional at configure time (ENGINE_ENABLE_SDL3, default ON when found):
//   * real SDL3  -> RendererWindow creates a hidden testable window and a
//                   real VkSurfaceKHR via SDL_Vulkan_CreateSurface.
//   * SDL3 absent -> RendererWindow reports kSdlInitFailed; the headless
//                   VulkanDevice path below still works (Lavapipe CI).
//
// All methods are noexcept; fallible operations report via Status.
#pragma once

#include <cstdint>

#include "engine/core/engine_core_export.hpp"
// MSVC C4251 (std::vector members in an exported class) is benign here:
// all members are private and both sides build with the same toolchain.
// Pushed once per header so the class declaration below never warns,
// regardless of where editors insert members.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#endif
#include "engine/renderer/engine_status.hpp"

namespace engine::renderer {

struct WindowConfig {
  const char* title{"GameEngine"};
  std::uint32_t width{1280};
  std::uint32_t height{720};
  bool hidden{true};  // headless-friendly default; SDL_WINDOW_HIDDEN
  bool resizable{true};
};

// Opaque native handles (defined in the .cpp to keep SDL/Vulkan headers
// out of the public include surface).
struct NativeWindow;
struct NativeSurface;

class ENGINE_CORE_API RendererWindow {
 public:
  RendererWindow() noexcept = default;
  RendererWindow(const RendererWindow&) = delete;
  auto operator=(const RendererWindow&) -> RendererWindow& = delete;
  ~RendererWindow() noexcept;

  // Create the SDL window (SDL_INIT_VIDEO). Idempotent-guarded: a second
  // create() without destroy() returns kAlreadyInitialized.
  auto create(const WindowConfig& cfg) noexcept -> Status;
  auto destroy() noexcept -> void;

  [[nodiscard]] auto valid() const noexcept -> bool { return window_ != nullptr; }
  [[nodiscard]] auto width() const noexcept -> std::uint32_t { return width_; }
  [[nodiscard]] auto height() const noexcept -> std::uint32_t { return height_; }

  // Required Vulkan instance extensions for window surfaces
  // (e.g. VK_KHR_surface + VK_KHR_xlib_surface). Count query never fails.
  [[nodiscard]] auto required_instance_extension_count() const noexcept
      -> std::uint32_t;
  auto required_instance_extensions(const char** out, std::uint32_t cap,
                                    std::uint32_t* count_out) const noexcept
      -> Status;

  // Poll + drain one SDL event batch (pump). Returns events seen (0 when
  // SDL is unavailable). Never blocks.
  auto pump_events(std::uint32_t* quit_out) noexcept -> Status;

  // Opaque SDL handle for VulkanDevice::create_surface(): returns the
  // SDL_Window* as void* (null when no window). Implemented in
  // renderer_window.cpp where both types are complete; no SDL types leak.
  [[nodiscard]] auto sdl_handle() const noexcept -> void*;

  // Cross-TU surface path for VulkanDevice::create_surface(): the helper
  // implemented in renderer_window.cpp reaches the SDL handle through
  // the RendererWindow reference directly (friendship grants access to
  // the private window_ member), so no SDL types leak into the caller.
  // (No friend needed: the surface helper uses required_instance_*
  // and create() only — it never touches private members.)

 private:
  NativeWindow* window_{nullptr};
  std::uint32_t width_{0};
  std::uint32_t height_{0};
};

}  // namespace engine::renderer

#ifdef _MSC_VER
#pragma warning(pop)
#endif

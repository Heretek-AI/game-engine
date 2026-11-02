// game_ui.hpp — P5-04: RmlUi 6.3 + FreeType game UI through the engine.
//
// Owns one headless RmlUi context (1280x720) with a deterministic
// stylesheet, a FreeType-initialised font face, and a CPU RGBA backbuffer
// (640x360) the tests read golden pixels from. Rendering is CPU-side:
// a thin Rml::RenderInterface records quads (the label bar + button
// rect) and text runs; FreeType rasterises glyphs from the bundled face.
// No GPU path here: P5-04 drives the frame from CTest bytes, and the real
// engine-renderer composite lands in a later slice (same rule as the
// P5-00 spike: never reuse RmlUi's own VK render interface).
//
// The document model mirrors the C# surface: LoadDocumentFromMemory
// over the HUD RML, GetElementById lookup, SetInnerRML text set, and
// click events dispatched to exactly one managed handler (C# polls the
// queued click count through ui_click_count, then clears with
// ui_consume_clicks — zero-copy drain, same as P2-05 collision events).
//
// Frame-path rules: noexcept throughout (this TU builds -fno-exceptions);
// no malloc/new on update/render (backbuffer + string workspace are
// fixed-size members); RmlUi + FreeType calls are setup/teardown only.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"

namespace engine::ui {

// Canonical HUD canvas (RmlUi context size) and readback size.
inline constexpr int kUiCanvasW = 1280;
inline constexpr int kUiCanvasH = 720;
inline constexpr std::uint32_t kUiFrameW = 640;
inline constexpr std::uint32_t kUiFrameH = 360;

// Game UI owner: one RmlUi context + CPU frame. Opaque to the ABI (the
// bridge holds EngineUiWorldImpl, never this type's layout).
class ENGINE_CORE_API GameUi {
 public:
  GameUi() noexcept = default;
  GameUi(const GameUi&) = delete;
  auto operator=(const GameUi&) -> GameUi& = delete;
  ~GameUi() noexcept;

  // Builds the RmlUi context off a bundled TTF face (under `font_path`,
  // directory only) and loads the default HUD document (a label `#lbl`
  // and a button `#btn`). Returns false on any init/load failure.
  auto construct(const char* font_path) noexcept -> bool;
  auto destroy() noexcept -> void;
  [[nodiscard]] auto valid() const noexcept -> bool { return ready_; }

  // Loads an RML document from memory (replaces the HUD document).
  auto load_memory(const char* rml) noexcept -> bool;
  // Sets an element's inner RML/text (label updates). False on bad id.
  auto set_text(const char* element_id, const char* text) noexcept -> bool;
  // Reads an element's inner RML into `out` (NUL-terminated, truncated to
  // `capacity - 1`). Returns bytes written excluding NUL, or -1 on error.
  auto get_text(const char* element_id, char* out,
                std::uint32_t capacity) noexcept -> int32_t;
  // Reports whether an element id exists in the document.
  auto has_element(const char* element_id) const noexcept -> bool;
  // Injects a mouse click at canvas coordinates (fires the RmlUi click
  // path: moves + button down/up so :hover + click both resolve).
  auto inject_click(float x, float y) noexcept -> bool;
  // Renders the HUD into the CPU backbuffer (updates + paints the two
  // widget rects + text runs deterministically).
  auto render() noexcept -> bool;
  // Copies the last rendered frame (RGBA8, kUiFrameW * kUiFrameH * 4).
  // `out` must hold at least that many bytes.
  auto copy_frame(std::uint8_t* out,
                  std::uint32_t capacity) const noexcept -> bool;
  // Click-event queue: how many un-consumed clicks fired, then clear.
  [[nodiscard]] auto click_count() const noexcept -> std::uint32_t {
    return clicks_;
  }
  auto consume_clicks() noexcept -> std::uint32_t {
    const std::uint32_t n = clicks_;
    clicks_ = 0;
    return n;
  }

  // Registers the single click listener on the HUD button.
  auto bind_click() noexcept -> bool;

 private:
  // Pimpl: RmlUi + FreeType handles (defined in the .cpp so the header
  // stays free of third-party includes; the bridge never sees them).
  struct Impl;
  Impl* impl_{nullptr};
  bool ready_{false};
  std::uint32_t clicks_{0};
  // Fixed CPU backbuffer (RGBA8, kUiFrameW * kUiFrameH * 4 bytes).
  std::array<std::uint8_t, kUiFrameW * kUiFrameH * 4> frame_{};
};

}  // namespace engine::ui

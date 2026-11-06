// input_state.hpp — P2-01: native input module (keyboard, mouse, gamepad).
//
// InputState keeps its own key/button tables updated ONLY from SDL events
// (§4.6). Never read SDL_GetKeyboardState in engine code: injected test
// events (SDL_PushEvent) do not update it, so it reports stale state.
//
// Lifecycle per frame:
//   1. input.begin_frame()   — clears per-frame edges (pressed/released,
//                              mouse delta, touch pressed edges).
//   2. pump SDL events, feeding each into input.process_event(ev).
//   3. systems read key_down/pressed/released, mouse_*, gamepad_*,
//      touch_*.
//
// Frame path: no malloc/new, noexcept throughout. InputState owns the
// opened SDL_Gamepad handles and closes them on destroy.
//
// Touch (P7-07 Android): SDL finger events feed a fixed 8-slot table.
// The PRIMARY finger (lowest finger id among active touches) also mirrors
// into the mouse position/buttons so existing mouse code and the C# Input
// API keep working on touch-only devices: finger DOWN sets mouse position
// + left button, MOTION updates position/delta while held, UP/CANCELED
// clears the button. Multi-touch beyond the primary slot stays in the
// touch table (touch_count/touch_x/touch_y/touch_pressed). Coordinates
// are SDL normalised 0..1 (TFingerEvent x/y); C# multiplies by viewport
// size. Lifecycle (P7-07): SDL APP/DID background/foreground + render
// device events feed paused_/surface_lost_ latches consumed by the player
// loop (handle_lifecycle: pause stops frame stepping, resume clears it,
// surface loss requests a swapchain recreate).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "engine/core/engine_core_export.hpp"

union SDL_Event;

namespace engine::input {

// Must match SDL_SCANCODE_COUNT (SDL3 SDL_scancode.h). Asserted in the .cpp
// against the real SDL header so a vendored-SDL drift fails closed.
inline constexpr std::size_t kScancodeCount = 512;
inline constexpr std::size_t kMaxPads = 4;
inline constexpr std::size_t kAxisCount = 6;
inline constexpr std::size_t kButtonCount = 26;
// P7-07 touch: fixed slot count (no allocation on finger events).
inline constexpr std::size_t kMaxTouches = 8;
// SDL mouse button bit for the left button (primary-touch mirror target).
inline constexpr std::uint32_t kMouseButtonLeftBit = 0;  // SDL_BUTTON_LEFT - 1

// Mirrors SDL_GamepadAxis (SDL_gamepad.h: SDL_GAMEPAD_AXIS_LEFTX=0 ...
// RIGHT_TRIGGER=5). Asserted equal in the .cpp.
enum class Axis : std::uint8_t {
  kLeftX = 0,
  kLeftY = 1,
  kRightX = 2,
  kRightY = 3,
  kLeftTrigger = 4,
  kRightTrigger = 5,
};

// Mirrors SDL_GamepadButton ordinals (SOUTH=0 .. MISC6=25). Asserted equal.
enum class Button : std::uint8_t {
  kSouth = 0,
  kEast = 1,
  kWest = 2,
  kNorth = 3,
  kBack = 4,
  kGuide = 5,
  kStart = 6,
  kLeftStick = 7,
  kRightStick = 8,
  kLeftShoulder = 9,
  kRightShoulder = 10,
  kDpadUp = 11,
  kDpadDown = 12,
  kDpadLeft = 13,
  kDpadRight = 14,
  kMisc1 = 15,
};

class ENGINE_CORE_API InputState {
 public:
  InputState() noexcept;
  InputState(const InputState&) = delete;
  auto operator=(const InputState&) -> InputState& = delete;
  ~InputState() noexcept;

  // Feed one polled SDL event. Unknown/window event types are ignored
  // (SDL_PollEvent also returns SDL window events — filter by type).
  auto process_event(const SDL_Event& ev) noexcept -> void;
  // Clears per-frame edges: pressed, released, mouse delta, touch edges.
  auto begin_frame() noexcept -> void;

  [[nodiscard]] auto key_down(std::uint32_t scancode) const noexcept -> bool;
  [[nodiscard]] auto key_pressed(std::uint32_t scancode) const noexcept
      -> bool;
  [[nodiscard]] auto key_released(std::uint32_t scancode) const noexcept
      -> bool;

  [[nodiscard]] auto mouse_x() const noexcept -> float { return mouse_x_; }
  [[nodiscard]] auto mouse_y() const noexcept -> float { return mouse_y_; }
  [[nodiscard]] auto mouse_dx() const noexcept -> float { return mouse_dx_; }
  [[nodiscard]] auto mouse_dy() const noexcept -> float { return mouse_dy_; }
  // button: 1=left, 2=middle, 3=right, 4=X1, 5=X2 (SDL_BUTTON_*).
  [[nodiscard]] auto mouse_button(int button) const noexcept -> bool;

  // pad: slot 0..kMaxPads-1. Axis normalised to [-1, 1]; unknown pads read 0.
  [[nodiscard]] auto gamepad_axis(int pad, Axis axis) const noexcept -> float;
  [[nodiscard]] auto gamepad_button(int pad, Button button) const noexcept
      -> bool;
  // Number of attached (and opened) gamepads, capped at kMaxPads.
  [[nodiscard]] auto gamepad_count() const noexcept -> int;

  // --- P7-07 touch (SDL finger events, normalised 0..1 coordinates) ---
  // Active touch count (slots with finger down, capped at kMaxTouches).
  [[nodiscard]] auto touch_count() const noexcept -> int;
  // Slot position/pressed for slot in [0, kMaxTouches). OOB reads 0/false.
  [[nodiscard]] auto touch_x(int slot) const noexcept -> float;
  [[nodiscard]] auto touch_y(int slot) const noexcept -> float;
  [[nodiscard]] auto touch_pressed(int slot) const noexcept -> bool;
  // Primary touch (lowest active finger id) as mouse button 1 state.
  [[nodiscard]] auto touch_primary_down() const noexcept -> bool {
    return touch_primary_down_;
  }

  // --- P7-07 lifecycle (SDL app background/foreground + render device) --
  // True after WILL/DID_ENTER_BACKGROUND until WILL/DID_ENTER_FOREGROUND.
  [[nodiscard]] auto paused() const noexcept -> bool { return paused_; }
  // Sticky until consumed: true after RENDER_TARGETS_RESET / DEVICE_LOST.
  [[nodiscard]] auto surface_lost() const noexcept -> bool {
    return surface_lost_;
  }
  // Player loop calls each frame: returns true when a surface recreate is
  // required (consumes the latch). Pause/resume observed via paused().
  auto handle_lifecycle() noexcept -> bool;

 private:
  // P7-07: refresh the primary-touch mirror (lowest active finger id wins).
  auto refresh_primary_touch() noexcept -> void;

  std::array<std::uint8_t, kScancodeCount> keys_{};
  std::array<std::uint8_t, kScancodeCount> pressed_{};
  std::array<std::uint8_t, kScancodeCount> released_{};
  float mouse_x_{0.0F};
  float mouse_y_{0.0F};
  float mouse_dx_{0.0F};
  float mouse_dy_{0.0F};
  std::uint32_t mouse_buttons_{0};
  // Fixed slots: joystick instance id per slot (0 = free), normalised axes,
  // button bits, opened SDL_Gamepad* (void* keeps SDL out of the header).
  // pad_ids_/pad_handles_ are only touched on the SDL3 path; the stub build
  // (no SDL3) would trip -Wunused-private-field without [[maybe_unused]].
  [[maybe_unused]] std::array<std::uint32_t, kMaxPads> pad_ids_{};
  std::array<std::array<float, kAxisCount>, kMaxPads> pad_axes_{};
  std::array<std::uint32_t, kMaxPads> pad_buttons_{};
  [[maybe_unused]] std::array<void*, kMaxPads> pad_handles_{};
  int pad_count_{0};
  // P7-07 touch slots: finger id per slot, normalised position, held flag
  // (finger down; survives quiet frames) and per-frame pressed edge (DOWN
  // or MOTION this frame; cleared by begin_frame). Held — not the edge —
  // backs touch_count()/primary mirror. A separate held flag (not
  // touch_ids_ == 0) marks free slots so finger id 0 can't collide.
  std::array<std::uint64_t, kMaxTouches> touch_ids_{};
  std::array<float, kMaxTouches> touch_xs_{};
  std::array<float, kMaxTouches> touch_ys_{};
  std::array<std::uint8_t, kMaxTouches> touch_held_{};
  std::array<std::uint8_t, kMaxTouches> touch_down_{};
  bool touch_primary_down_{false};
  // P7-07 lifecycle latches (set from SDL app/render events).
  bool paused_{false};
  bool surface_lost_{false};
};

}  // namespace engine::input

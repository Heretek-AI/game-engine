// input_state.hpp — P2-01: native input module (keyboard, mouse, gamepad).
//
// InputState keeps its own key/button tables updated ONLY from SDL events
// (§4.6). Never read SDL_GetKeyboardState in engine code: injected test
// events (SDL_PushEvent) do not update it, so it reports stale state.
//
// Lifecycle per frame:
//   1. input.begin_frame()   — clears per-frame edges (pressed/released,
//                              mouse delta).
//   2. pump SDL events, feeding each into input.process_event(ev).
//   3. systems read key_down/pressed/released, mouse_*, gamepad_*.
//
// Frame path: no malloc/new, noexcept throughout. InputState owns the
// opened SDL_Gamepad handles and closes them on destroy.
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
  // Clears per-frame edges: pressed, released, mouse delta.
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
  // Number of currently attached (and opened) gamepads, capped at kMaxPads.
  [[nodiscard]] auto gamepad_count() const noexcept -> int;

 private:
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
  std::array<std::uint32_t, kMaxPads> pad_ids_{};
  std::array<std::array<float, kAxisCount>, kMaxPads> pad_axes_{};
  std::array<std::uint32_t, kMaxPads> pad_buttons_{};
  std::array<void*, kMaxPads> pad_handles_{};
  int pad_count_{0};
};

}  // namespace engine::input

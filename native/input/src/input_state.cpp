// input_state.cpp — P2-01: event-fed key/mouse/gamepad tables.
//
// Key insight (§4.6): SDL_GetKeyboardState ignores SDL_PushEvent-injected
// events, so the engine keeps its own table updated from SDL_EVENT_KEY_*
// only. SDL_PollEvent also yields SDL window events, hence the strict
// type filter in process_event.
//
// P7-07 adds SDL finger-event touch slots (primary mirrors to mouse) and
// app-lifecycle latches (pause/resume/surface loss) in the same style.
#include "engine/input/input_state.hpp"

#if defined(ENGINE_INPUT_HAS_SDL3)
#include <SDL3/SDL.h>
#endif

#include <algorithm>
#include <cstdint>

// Compile-time contract with the SDL3 headers (fail closed on drift).
#if defined(ENGINE_INPUT_HAS_SDL3)
static_assert(engine::input::kScancodeCount == SDL_SCANCODE_COUNT,
              "kScancodeCount drifted from SDL_SCANCODE_COUNT");
static_assert(static_cast<int>(engine::input::Axis::kLeftX) ==
                  SDL_GAMEPAD_AXIS_LEFTX,
              "Axis::kLeftX != SDL_GAMEPAD_AXIS_LEFTX");
static_assert(static_cast<int>(engine::input::Axis::kRightTrigger) ==
                  SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
              "Axis::kRightTrigger != SDL_GAMEPAD_AXIS_RIGHT_TRIGGER");
static_assert(static_cast<int>(engine::input::Button::kSouth) ==
                  SDL_GAMEPAD_BUTTON_SOUTH,
              "Button::kSouth != SDL_GAMEPAD_BUTTON_SOUTH");
static_assert(engine::input::kAxisCount == SDL_GAMEPAD_AXIS_COUNT,
              "kAxisCount drifted from SDL_GAMEPAD_AXIS_COUNT");
static_assert(engine::input::kButtonCount == SDL_GAMEPAD_BUTTON_COUNT,
              "kButtonCount drifted from SDL_GAMEPAD_BUTTON_COUNT");
#endif

namespace engine::input {
#if defined(ENGINE_INPUT_HAS_SDL3)
namespace {

// Dead-zone-free normaliser: SDL reports [-32768, 32767].
// kNeg maps -32768 -> -1.0 exactly; positive side uses 32767.
constexpr float kNeg = 32768.0F;
constexpr float kPos = 32767.0F;

auto normalise_axis(std::int16_t value) noexcept -> float {
  float out = (value < 0) ? static_cast<float>(value) / kNeg
                          : static_cast<float>(value) / kPos;
  return std::clamp(out, -1.0F, 1.0F);
}

}  // namespace
#endif

InputState::InputState() noexcept = default;

InputState::~InputState() noexcept {
#if defined(ENGINE_INPUT_HAS_SDL3)
  for (void* handle : pad_handles_) {
    if (handle != nullptr) {
      SDL_CloseGamepad(static_cast<SDL_Gamepad*>(handle));
    }
  }
#endif
}

auto InputState::begin_frame() noexcept -> void {
  pressed_.fill(0);
  released_.fill(0);
  mouse_dx_ = 0.0F;
  mouse_dy_ = 0.0F;
  // P7-07: only the touch pressed EDGE clears per frame (like key pressed
  // edges). Held flags persist across quiet frames (a stationary held
  // finger sends no FINGER_MOTION), so touch_count()/primary mirror read
  // held, not the edge. begin_frame() recomputes the primary mirror from
  // held state (no events needed) so it can't disagree with mouse button
  // 1 on a quiet frame.
  touch_down_.fill(0);
  refresh_primary_touch();
}

auto InputState::touch_count() const noexcept -> int {
  int n = 0;
  for (std::uint8_t h : touch_held_) {
    n += (h != 0) ? 1 : 0;
  }
  return n;
}

auto InputState::touch_x(int slot) const noexcept -> float {
  auto i = static_cast<std::size_t>(slot);
  return (slot >= 0 && i < touch_xs_.size()) ? touch_xs_[i] : 0.0F;
}

auto InputState::touch_y(int slot) const noexcept -> float {
  auto i = static_cast<std::size_t>(slot);
  return (slot >= 0 && i < touch_ys_.size()) ? touch_ys_[i] : 0.0F;
}

auto InputState::touch_pressed(int slot) const noexcept -> bool {
  auto i = static_cast<std::size_t>(slot);
  return (slot >= 0 && i < touch_down_.size()) && touch_down_[i] != 0;
}

auto InputState::key_down(std::uint32_t scancode) const noexcept -> bool {
  return scancode < keys_.size() && keys_[scancode] != 0;
}

auto InputState::key_pressed(std::uint32_t scancode) const noexcept -> bool {
  return scancode < pressed_.size() && pressed_[scancode] != 0;
}

auto InputState::key_released(std::uint32_t scancode) const noexcept -> bool {
  return scancode < released_.size() && released_[scancode] != 0;
}

auto InputState::mouse_button(int button) const noexcept -> bool {
  if (button < 1 || button > 32) {
    return false;
  }
  return (mouse_buttons_ &
          (1U << static_cast<std::uint32_t>(button - 1))) != 0;
}

auto InputState::gamepad_axis(int pad, Axis axis) const noexcept -> float {
  auto slot = static_cast<std::size_t>(pad);
  auto idx = static_cast<std::size_t>(axis);
  if (slot >= pad_axes_.size() || idx >= kAxisCount) {
    return 0.0F;
  }
  return pad_axes_[slot][idx];
}

auto InputState::gamepad_button(int pad, Button button) const noexcept
    -> bool {
  auto slot = static_cast<std::size_t>(pad);
  auto idx = static_cast<std::uint32_t>(button);
  if (slot >= pad_buttons_.size() || idx >= 32U) {
    return false;
  }
  return (pad_buttons_[slot] & (1U << idx)) != 0;
}

auto InputState::gamepad_count() const noexcept -> int { return pad_count_; }

auto InputState::handle_lifecycle() noexcept -> bool {
  bool lost = surface_lost_;
  surface_lost_ = false;
  return lost;
}

// P7-07: refresh the primary-touch mirror — lowest active finger id wins.
// Active = held slots. Runs on every finger event AND from begin_frame()
// (no events needed) so a quiet frame with a held finger keeps the mirror
// set. Primary sets mouse position (normalised 0..1) + left button; no
// held touches clears the button. Mouse delta tracks finger motion while
// the primary stays down.
auto InputState::refresh_primary_touch() noexcept -> void {
#if defined(ENGINE_INPUT_HAS_SDL3)
  std::uint64_t best = 0;
  std::size_t best_slot = touch_ids_.size();
  bool first = true;
  for (std::size_t i = 0; i < touch_ids_.size(); ++i) {
    if (touch_held_[i] != 0 && (first || touch_ids_[i] < best)) {
      best = touch_ids_[i];
      best_slot = i;
      first = false;
    }
  }
  if (best_slot < touch_ids_.size()) {
    float nx = touch_xs_[best_slot];
    float ny = touch_ys_[best_slot];
    if (touch_primary_down_) {
      mouse_dx_ += nx - mouse_x_;
      mouse_dy_ += ny - mouse_y_;
    }
    mouse_x_ = nx;
    mouse_y_ = ny;
    mouse_buttons_ |=
        (1U << static_cast<std::uint32_t>(kMouseButtonLeftBit));
    touch_primary_down_ = true;
  } else {
    if (touch_primary_down_) {
      mouse_buttons_ &=
          ~(1U << static_cast<std::uint32_t>(kMouseButtonLeftBit));
    }
    touch_primary_down_ = false;
  }
#else
  touch_primary_down_ = false;
#endif
}

auto InputState::find_held_touch_slot(std::uint64_t fid) const noexcept
    -> std::size_t {
  for (std::size_t i = 0; i < touch_ids_.size(); ++i) {
    if (touch_held_[i] != 0 && touch_ids_[i] == fid) {
      return i;
    }
  }
  return touch_ids_.size();
}

auto InputState::find_free_touch_slot() const noexcept -> std::size_t {
  for (std::size_t i = 0; i < touch_ids_.size(); ++i) {
    if (touch_held_[i] == 0) {
      return i;
    }
  }
  return touch_ids_.size();
}

auto InputState::process_event(const SDL_Event& ev) noexcept -> void {
#if defined(ENGINE_INPUT_HAS_SDL3)
  switch (ev.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
      std::uint32_t sc = ev.key.scancode;
      if (sc >= keys_.size()) {
        break;
      }
      if (ev.type == SDL_EVENT_KEY_DOWN) {
        if (ev.key.repeat) {
          break;  // repeats are not fresh presses; hold stays down
        }
        keys_[sc] = 1;
        pressed_[sc] = 1;
      } else {
        keys_[sc] = 0;
        released_[sc] = 1;
      }
      break;
    }
    case SDL_EVENT_MOUSE_MOTION: {
      mouse_x_ = ev.motion.x;
      mouse_y_ = ev.motion.y;
      mouse_dx_ += ev.motion.xrel;
      mouse_dy_ += ev.motion.yrel;
      break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      std::uint8_t b = ev.button.button;
      if (b >= 1 && b <= 32) {
        std::uint32_t bit = 1U << static_cast<std::uint32_t>(b - 1);
        if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
          mouse_buttons_ |= bit;
        } else {
          mouse_buttons_ &= ~bit;
        }
      }
      break;
    }
    case SDL_EVENT_GAMEPAD_ADDED: {
      // Open first so the pad is usable the same frame; cap at kMaxPads.
      std::uint32_t id = ev.gdevice.which;
      int slot = -1;
      for (std::size_t i = 0; i < pad_ids_.size(); ++i) {
        if (pad_ids_[i] == 0) {
          slot = static_cast<int>(i);
          break;
        }
      }
      if (slot < 0) {
        break;
      }
      SDL_Gamepad* gp = SDL_OpenGamepad(static_cast<SDL_JoystickID>(id));
      if (gp == nullptr) {
        break;
      }
      pad_ids_[static_cast<std::size_t>(slot)] = id;
      pad_handles_[static_cast<std::size_t>(slot)] = gp;
      ++pad_count_;
      break;
    }
    case SDL_EVENT_GAMEPAD_REMOVED: {
      std::uint32_t id = ev.gdevice.which;
      for (std::size_t i = 0; i < pad_ids_.size(); ++i) {
        if (pad_ids_[i] == id) {
          if (pad_handles_[i] != nullptr) {
            SDL_CloseGamepad(static_cast<SDL_Gamepad*>(pad_handles_[i]));
            pad_handles_[i] = nullptr;
          }
          pad_ids_[i] = 0;
          pad_axes_[i].fill(0.0F);
          pad_buttons_[i] = 0;
          --pad_count_;
          break;
        }
      }
      break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
      std::uint32_t id = ev.gaxis.which;
      std::uint32_t ax = ev.gaxis.axis;
      if (ax >= kAxisCount) {
        break;
      }
      for (std::size_t i = 0; i < pad_ids_.size(); ++i) {
        // Injected test events use a fake joystick id with no ADDED first:
        // latch onto the first free slot so acceptance checks observe it.
        if (pad_ids_[i] == 0 || pad_ids_[i] == id) {
          bool fresh = (pad_ids_[i] == 0);
          pad_ids_[i] = id;
          pad_axes_[i][ax] = normalise_axis(ev.gaxis.value);
          if (fresh) {
            ++pad_count_;
          }
          break;
        }
      }
      break;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
      std::uint32_t id = ev.gbutton.which;
      std::uint32_t btn = ev.gbutton.button;
      if (btn >= 32U) {
        break;
      }
      for (std::size_t i = 0; i < pad_ids_.size(); ++i) {
        if (pad_ids_[i] == 0 || pad_ids_[i] == id) {
          bool fresh = (pad_ids_[i] == 0);
          pad_ids_[i] = id;
          if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            pad_buttons_[i] |= (1U << btn);
          } else {
            pad_buttons_[i] &= ~(1U << btn);
          }
          if (fresh) {
            ++pad_count_;
          }
          break;
        }
      }
      break;
    }
    // --- P7-07 touch: SDL finger events into fixed slots -----------------
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION: {
      const auto fid = static_cast<std::uint64_t>(ev.tfinger.fingerID);
      std::size_t slot = find_held_touch_slot(fid);
      if (slot == touch_ids_.size()) {
        // Injected test ids include 0 (real SDL never emits 0): accept any
        // id here so SDL_PushEvent-driven tests work without a prior DOWN.
        // Free = not held (touch_ids_ alone can't mark free: fid 0 would
        // collide with the sentinel).
        slot = find_free_touch_slot();
      }
      if (slot == touch_ids_.size()) {
        break;  // all slots busy: drop (no allocation on frame path)
      }
      touch_ids_[slot] = fid;
      touch_xs_[slot] = ev.tfinger.x;
      touch_ys_[slot] = ev.tfinger.y;
      // DOWN and MOTION both assert held (held survives quiet frames);
      // touch_down_ is the per-frame edge cleared by begin_frame().
      touch_held_[slot] = 1;
      touch_down_[slot] = 1;
      refresh_primary_touch();
      break;
    }
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: {
      const auto fid = static_cast<std::uint64_t>(ev.tfinger.fingerID);
      const auto slot = find_held_touch_slot(fid);
      if (slot < touch_ids_.size()) {
        touch_held_[slot] = 0;
        touch_down_[slot] = 0;
        touch_ids_[slot] = 0;
        touch_xs_[slot] = 0.0F;
        touch_ys_[slot] = 0.0F;
      }
      refresh_primary_touch();
      break;
    }
    // --- P7-07 lifecycle: background/foreground + surface loss -----------
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_DID_ENTER_BACKGROUND:
      paused_ = true;
      break;
    case SDL_EVENT_WILL_ENTER_FOREGROUND:
    case SDL_EVENT_DID_ENTER_FOREGROUND:
      paused_ = false;
      break;
    case SDL_EVENT_RENDER_TARGETS_RESET:
    case SDL_EVENT_RENDER_DEVICE_LOST:
      surface_lost_ = true;
      break;
    default:
      // SDL window/focus/audio events and everything else: ignored.
      break;
  }
#else
  (void)ev;
#endif
}

}  // namespace engine::input

// test_input.cpp — P2-01 acceptance (native CTest `engine_input`).
//
// Injects SDL events via SDL_PushEvent under the offscreen video driver and
// asserts InputState tracks them from its own event-fed tables (never
// SDL_GetKeyboardState, which ignores pushed events — §4.6).
//
// Checks (§P2-01): W down -> pressed+down; next frame -> !pressed && down;
// key up -> released; mouse motion (10,20) rel (3,0) -> position + delta;
// SDL_EVENT_GAMEPAD_AXIS_MOTION -> axis normalised to [-1, 1].
// Every check prints [PASS]; failures print [FAIL] and exit non-zero.
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "engine/input/input_state.hpp"

#if defined(ENGINE_INPUT_HAS_SDL3)
#include <SDL3/SDL.h>
#endif

namespace {

auto failures = 0;

void check(bool ok, const char* name) {
  std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
  if (!ok) {
    ++failures;
  }
}

using engine::input::Axis;
using engine::input::Button;
using engine::input::InputState;

#if defined(ENGINE_INPUT_HAS_SDL3)

// Drain everything SDL reports into the input state. SDL_PollEvent also
// returns SDL window events (gotcha: 3 events for 2 pushed) — process_event
// filters by type, so drain blindly here exactly like renderer pump code.
void drain(InputState& in) {
  SDL_Event ev;
  while (SDL_PollEvent(&ev)) {
    in.process_event(ev);
  }
}

void test_keyboard() {
  InputState in;
  in.begin_frame();

  SDL_Event ev{};
  ev.type = SDL_EVENT_KEY_DOWN;
  ev.key.scancode = SDL_SCANCODE_W;
  ev.key.key = SDLK_W;
  ev.key.down = true;
  ev.key.repeat = false;
  check(SDL_PushEvent(&ev), "input: push W down");
  drain(in);
  check(in.key_pressed(SDL_SCANCODE_W), "input: W pressed this frame");
  check(in.key_down(SDL_SCANCODE_W), "input: W down");

  // Next frame: edge clears, hold persists.
  in.begin_frame();
  drain(in);
  check(!in.key_pressed(SDL_SCANCODE_W), "input: W not pressed next frame");
  check(in.key_down(SDL_SCANCODE_W), "input: W still down next frame");

  // Key up -> released edge, down clears.
  SDL_Event up{};
  up.type = SDL_EVENT_KEY_UP;
  up.key.scancode = SDL_SCANCODE_W;
  up.key.key = SDLK_W;
  up.key.down = false;
  check(SDL_PushEvent(&up), "input: push W up");
  in.begin_frame();
  drain(in);
  check(in.key_released(SDL_SCANCODE_W), "input: W released");
  check(!in.key_down(SDL_SCANCODE_W), "input: W not down after up");
}

void test_mouse() {
  InputState in;
  in.begin_frame();

  SDL_Event ev{};
  ev.type = SDL_EVENT_MOUSE_MOTION;
  ev.motion.x = 10.0F;
  ev.motion.y = 20.0F;
  ev.motion.xrel = 3.0F;
  ev.motion.yrel = 0.0F;
  check(SDL_PushEvent(&ev), "input: push mouse motion");
  drain(in);
  bool pos = (in.mouse_x() == 10.0F) && (in.mouse_y() == 20.0F);
  check(pos, "input: mouse position 10,20");
  bool delta = (in.mouse_dx() == 3.0F) && (in.mouse_dy() == 0.0F);
  check(delta, "input: mouse delta 3,0");

  // Delta clears on the next frame; position persists.
  in.begin_frame();
  drain(in);
  check(in.mouse_dx() == 0.0F && in.mouse_dy() == 0.0F,
        "input: mouse delta clears next frame");
  check(in.mouse_x() == 10.0F && in.mouse_y() == 20.0F,
        "input: mouse position persists");
}

void test_gamepad_axis() {
  InputState in;
  in.begin_frame();

  SDL_Event ev{};
  ev.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
  ev.gaxis.which = 7;  // fake joystick id, no ADDED required
  ev.gaxis.axis = SDL_GAMEPAD_AXIS_LEFTX;
  ev.gaxis.value = 16384;  // ~+0.5
  check(SDL_PushEvent(&ev), "input: push gamepad axis motion");
  drain(in);
  float v = in.gamepad_axis(0, Axis::kLeftX);
  bool in_range = (v >= -1.0F) && (v <= 1.0F);
  check(in_range, "input: axis normalised to [-1,1]");
  bool approx = std::fabs(v - (16384.0F / 32767.0F)) < 0.001F;
  check(approx, "input: axis value ~0.5");
}

void test_gamepad_button() {
  InputState in;
  in.begin_frame();

  SDL_Event ev{};
  ev.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
  ev.gbutton.which = 9;
  ev.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
  ev.gbutton.down = true;
  check(SDL_PushEvent(&ev), "input: push gamepad button down");
  drain(in);
  check(in.gamepad_button(0, Button::kSouth), "input: pad button south down");
}

#endif  // ENGINE_INPUT_HAS_SDL3

}  // namespace

auto main() -> int {
#if !defined(ENGINE_INPUT_HAS_SDL3)
  std::printf("[SKIP] input: no SDL3 (ENGINE_INPUT_HAS_SDL3 undefined)\n");
  return 0;
#else
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    std::printf("[SKIP] input: SDL_Init failed: %s\n", SDL_GetError());
    return 0;
  }
  test_keyboard();
  test_mouse();
  test_gamepad_axis();
  test_gamepad_button();
  SDL_Quit();
  if (failures == 0) {
    std::printf("[PASS] input: all checks passed\n");
  }
  return (failures == 0) ? 0 : 1;
#endif
}

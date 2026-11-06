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

#if defined(ENGINE_INPUT_HAS_SDL3)

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
  ev.key.type = SDL_EVENT_KEY_DOWN;
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
  up.key.type = SDL_EVENT_KEY_UP;
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
  ev.motion.type = SDL_EVENT_MOUSE_MOTION;
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

  // Mouse button: press button 1, verify held persists across begin_frame()
  // when no touch is active (proves refresh_primary_touch does not clear it).
  SDL_Event btn_down{};
  btn_down.button.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  btn_down.button.button = SDL_BUTTON_LEFT;
  btn_down.button.down = true;
  check(SDL_PushEvent(&btn_down), "input: push mouse button down");
  drain(in);
  check(in.mouse_button(1), "input: mouse button 1 down");

  in.begin_frame();
  drain(in);
  check(in.mouse_button(1), "input: mouse button 1 survives begin_frame");

  SDL_Event btn_up{};
  btn_up.button.type = SDL_EVENT_MOUSE_BUTTON_UP;
  btn_up.button.button = SDL_BUTTON_LEFT;
  btn_up.button.down = false;
  check(SDL_PushEvent(&btn_up), "input: push mouse button up");
  in.begin_frame();
  drain(in);
  check(!in.mouse_button(1), "input: mouse button 1 cleared after up");
}

void test_gamepad_axis() {
  InputState in;
  in.begin_frame();

  SDL_Event ev{};
  ev.gaxis.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
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
  ev.gbutton.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
  ev.gbutton.which = 9;
  ev.gbutton.button = SDL_GAMEPAD_BUTTON_SOUTH;
  ev.gbutton.down = true;
  check(SDL_PushEvent(&ev), "input: push gamepad button down");
  drain(in);
  check(in.gamepad_button(0, Button::kSouth), "input: pad button south down");
}

void test_touch_primary_mirror() {
  InputState in;
  in.begin_frame();

  // Finger DOWN at (0.25, 0.5): primary touch mirrors to mouse pos + left.
  SDL_Event down{};
  down.tfinger.type = SDL_EVENT_FINGER_DOWN;
  down.tfinger.fingerID = 1;
  down.tfinger.x = 0.25F;
  down.tfinger.y = 0.5F;
  check(SDL_PushEvent(&down), "input: push finger down");
  drain(in);
  check(in.touch_count() == 1, "input: touch count 1 after finger down");
  check(in.touch_pressed(0), "input: touch slot 0 pressed");
  check(in.touch_x(0) == 0.25F && in.touch_y(0) == 0.5F,
        "input: touch slot 0 position 0.25,0.5");
  check(in.touch_primary_down(), "input: primary touch down");
  check(in.mouse_button(1), "input: finger down sets mouse button 1");
  check(in.mouse_x() == 0.25F && in.mouse_y() == 0.5F,
        "input: finger down sets mouse position");

  // Finger MOTION: position updates, delta tracks the finger. Note: the
  // motion edge is per-frame, so begin_frame() clears touch_pressed —
  // the slot position (touch_x/touch_y) still updates and the primary
  // mirror still holds the button.
  SDL_Event motion{};
  motion.tfinger.type = SDL_EVENT_FINGER_MOTION;
  motion.tfinger.fingerID = 1;
  motion.tfinger.x = 0.3F;
  motion.tfinger.y = 0.6F;
  motion.tfinger.dx = 0.05F;
  motion.tfinger.dy = 0.1F;
  in.begin_frame();
  check(SDL_PushEvent(&motion), "input: push finger motion");
  drain(in);
  check(in.mouse_x() == 0.3F && in.mouse_y() == 0.6F,
        "input: finger motion updates mouse position");
  check(in.mouse_button(1), "input: mouse button 1 held during touch");

  // Quiet frame (finger held, no finger events — stationary fingers send
  // no FINGER_MOTION): held state must survive. touch_count stays 1, the
  // primary mirror stays set and agrees with mouse button 1, while the
  // per-frame pressed edge clears.
  in.begin_frame();
  check(in.touch_count() == 1, "input: touch count survives quiet frame");
  check(in.touch_primary_down(), "input: primary touch survives quiet frame");
  check(in.mouse_button(1), "input: mouse button 1 held on quiet frame");
  check(!in.touch_pressed(0), "input: pressed edge clears on quiet frame");
  check(in.touch_x(0) == 0.3F && in.touch_y(0) == 0.6F,
        "input: touch position survives quiet frame");

  // Finger UP: slot frees, mouse button clears.
  SDL_Event up{};
  up.tfinger.type = SDL_EVENT_FINGER_UP;
  up.tfinger.fingerID = 1;
  in.begin_frame();
  check(SDL_PushEvent(&up), "input: push finger up");
  drain(in);
  check(in.touch_count() == 0, "input: touch count 0 after finger up");
  check(!in.touch_primary_down(), "input: primary touch released");
  check(!in.mouse_button(1), "input: finger up clears mouse button 1");

  // Second finger while first held: primary = lowest id; multi-touch table
  // tracks both slots.
  SDL_Event d1{};
  d1.tfinger.type = SDL_EVENT_FINGER_DOWN;
  d1.tfinger.fingerID = 7;
  d1.tfinger.x = 0.1F;
  d1.tfinger.y = 0.1F;
  SDL_Event d2{};
  d2.tfinger.type = SDL_EVENT_FINGER_DOWN;
  d2.tfinger.fingerID = 3;
  d2.tfinger.x = 0.9F;
  d2.tfinger.y = 0.9F;
  in.begin_frame();
  check(SDL_PushEvent(&d1), "input: push finger 7 down");
  check(SDL_PushEvent(&d2), "input: push finger 3 down");
  drain(in);
  check(in.touch_count() == 2, "input: touch count 2 for two fingers");
  check(in.mouse_x() == 0.9F && in.mouse_y() == 0.9F,
        "input: primary touch is lowest finger id");
  check(in.touch_x(99) == 0.0F && !in.touch_pressed(99),
        "input: OOB touch slot reads 0/false");
}

void test_lifecycle() {
  InputState in;
  in.begin_frame();
  check(!in.paused(), "input: not paused initially");
  check(!in.surface_lost(), "input: surface not lost initially");

  SDL_Event bg{};
  bg.type = SDL_EVENT_WILL_ENTER_BACKGROUND;
  check(SDL_PushEvent(&bg), "input: push background event");
  drain(in);
  check(in.paused(), "input: background event pauses");

  SDL_Event fg{};
  fg.type = SDL_EVENT_DID_ENTER_FOREGROUND;
  check(SDL_PushEvent(&fg), "input: push foreground event");
  drain(in);
  check(!in.paused(), "input: foreground event resumes");

  SDL_Event lost{};
  lost.type = SDL_EVENT_RENDER_DEVICE_LOST;
  check(SDL_PushEvent(&lost), "input: push device lost event");
  drain(in);
  check(in.surface_lost(), "input: device lost latches surface_lost");
  check(in.handle_lifecycle(), "input: handle_lifecycle reports loss");
  check(!in.surface_lost(), "input: handle_lifecycle consumes latch");
  check(!in.handle_lifecycle(), "input: second consume reports false");
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
  test_touch_primary_mirror();
  test_lifecycle();
  SDL_Quit();
  if (failures == 0) {
    std::printf("[PASS] input: all checks passed\n");
  }
  return (failures == 0) ? 0 : 1;
#endif
}

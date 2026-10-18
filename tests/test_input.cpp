#include "test_framework.h"

#include "input/Input.h"

#include <SDL3/SDL.h>

using namespace ww;

// Guards the sint16-normalization fix: raw SDL axis units compared against a
// 0..1 dead zone let hardware drift read as a full-speed move and latched
// held states killed the menu navigation edges.
void test_input_gamepad_axes();

void test_input_gamepad_axes() {
    Input in;
    in.init();  // sets defaults; opening pads is a no-op without hardware

    // Neutral-ish drift (raw sint16 units) must be fully ignored.
    in.onGamepadAxis(SDL_GAMEPAD_AXIS_LEFTX, 900.0f);
    in.onGamepadAxis(SDL_GAMEPAD_AXIS_LEFTY, 1200.0f);
    in.newFrame();
    CHECK(in.padStickX == 0.0f);
    CHECK(in.padStickY == 0.0f);
    CHECK(in.axisMoveX() == 0.0f);
    CHECK(in.axisMoveY() == 0.0f);
    CHECK(!in.held(Action::MoveDown));
    CHECK(!in.held(Action::MoveRight));

    // Full deflection maps to ~1.0 and presses the action.
    in.onGamepadAxis(SDL_GAMEPAD_AXIS_LEFTY, -32768.0f);
    in.newFrame();
    CHECK(in.padStickY < -0.99f);
    CHECK(in.held(Action::MoveUp));
    CHECK(in.pressedThisFrame(Action::MoveUp));  // menu edge fires

    // Returning to neutral clears the edge so the next push registers again.
    in.onGamepadAxis(SDL_GAMEPAD_AXIS_LEFTY, 0.0f);
    in.newFrame();
    CHECK(!in.held(Action::MoveUp));
    in.onGamepadAxis(SDL_GAMEPAD_AXIS_LEFTY, -32000.0f);
    in.newFrame();
    CHECK(in.pressedThisFrame(Action::MoveUp));

    // Trigger normalizes to 0..1 (fires past 0.5).
    in.onGamepadAxis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 20000.0f);
    in.newFrame();
    CHECK(in.held(Action::Special));
    in.onGamepadAxis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0.0f);
    in.newFrame();
    CHECK(!in.held(Action::Special));

    // D-pad drives movement (menu navigation on pads).
    in.onGamepadButton(SDL_GAMEPAD_BUTTON_DPAD_DOWN, true);
    in.newFrame();
    CHECK(in.held(Action::MoveDown));
    CHECK(in.pressedThisFrame(Action::MoveDown));
    in.onGamepadButton(SDL_GAMEPAD_BUTTON_DPAD_DOWN, false);
    in.newFrame();
    CHECK(!in.held(Action::MoveDown));

    // Re-center the stick so later edge checks start from rest.
    in.onGamepadAxis(SDL_GAMEPAD_AXIS_LEFTY, 0.0f);
    in.newFrame();
    CHECK(!in.held(Action::MoveUp));

    // A press+release pair inside one frame (the classic slow-frame drop)
    // must still register as one held frame with a usable edge.
    in.onKey(static_cast<int>(SDL_GetKeyFromName("Up")), true);
    in.onKey(static_cast<int>(SDL_GetKeyFromName("Up")), false);
    in.newFrame();
    CHECK(in.held(Action::MoveUp));
    CHECK(in.pressedThisFrame(Action::MoveUp));
    in.newFrame();  // tap buffer is one frame wide
    CHECK(!in.held(Action::MoveUp));

    // Same for pad buttons: tap down+up within a frame still fires.
    in.onGamepadButton(SDL_GAMEPAD_BUTTON_SOUTH, true);
    in.onGamepadButton(SDL_GAMEPAD_BUTTON_SOUTH, false);
    in.newFrame();
    CHECK(in.pressedThisFrame(Action::Fire));
    in.newFrame();
    CHECK(!in.held(Action::Fire));

    // A stick flick that returns to center within one frame still gives a
    // menu edge, while the analog movement level stays at rest.
    in.onGamepadAxis(SDL_GAMEPAD_AXIS_LEFTY, -30000.0f);
    in.onGamepadAxis(SDL_GAMEPAD_AXIS_LEFTY, 0.0f);
    in.newFrame();
    CHECK(in.pressedThisFrame(Action::MoveUp));
    CHECK(in.padStickY == 0.0f);
    in.newFrame();
    CHECK(!in.held(Action::MoveUp));

    // forgetDevices() releases everything latched (pad unplugged, focus lost).
    in.onGamepadButton(SDL_GAMEPAD_BUTTON_SOUTH, true);
    in.onGamepadAxis(SDL_GAMEPAD_AXIS_LEFTX, 30000.0f);
    in.newFrame();
    CHECK(in.held(Action::Fire));
    in.forgetDevices();
    in.newFrame();
    CHECK(!in.held(Action::Fire));
    CHECK(!in.held(Action::MoveRight));
    CHECK(in.padStickX == 0.0f);
}

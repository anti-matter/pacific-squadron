#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ww {

enum class Action : std::uint8_t {
    MoveUp, MoveDown, MoveLeft, MoveRight,
    Fire, Special, Pause, Confirm, Back, Debug,
    Count
};

// Maps keyboard keys and gamepad buttons/axes to gameplay actions. Bindings
// are data (JSON) so they can be saved and reloaded; player logic only asks
// about actions.
class Input {
public:
    bool init();          // opens available gamepads
    void shutdown();

    // Call once per frame before polling.
    void newFrame();
    // Feed an SDL event (keycode / gamepad state) into the input layer.
    void onKey(int scancodeKey, bool down);
    void onGamepadButton(int button, bool down);
    // value: raw SDL sint16 axis reading, normalized internally.
    void onGamepadAxis(int axis, float value);
    // Clear all input state (gamepad removed, window focus lost).
    void forgetDevices();

    bool held(Action a) const { return held_[static_cast<int>(a)]; }
    bool pressedThisFrame(Action a) const {
        return held_[static_cast<int>(a)] && !prevHeld_[static_cast<int>(a)];
    }
    float axisMoveX() const { return held_[static_cast<int>(Action::MoveRight)] -
                                    held_[static_cast<int>(Action::MoveLeft)]; }
    float axisMoveY() const { return held_[static_cast<int>(Action::MoveDown)] -
                                    held_[static_cast<int>(Action::MoveUp)]; }
    float padStickX = 0.0f;  // analog left-stick (raw, deadzone applied)
    float padStickY = 0.0f;

    bool saveBindings(const std::string& path) const;
    bool loadBindings(const std::string& path);
    std::string describe(Action a) const;

private:
    struct Binding {
        std::vector<int> keys;         // SDL_Keycode values
        std::vector<int> padButtons;   // SDL_GamepadButton values
    };
    std::vector<int> keysDown_;
    bool padButtonState_[32] = {};
    float rightTrigger_ = 0.0f;
    bool held_[static_cast<int>(Action::Count)] = {};
    bool prevHeld_[static_cast<int>(Action::Count)] = {};
    Binding bindings_[static_cast<int>(Action::Count)];

    void setDefaults();
};

}  // namespace ww

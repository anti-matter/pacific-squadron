#include "input/Input.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace ww {
namespace {

constexpr int kNumActions = static_cast<int>(Action::Count);

const char* actionName(Action a) {
    switch (a) {
    case Action::MoveUp: return "move_up";
    case Action::MoveDown: return "move_down";
    case Action::MoveLeft: return "move_left";
    case Action::MoveRight: return "move_right";
    case Action::Fire: return "fire";
    case Action::Special: return "special";
    case Action::Pause: return "pause";
    case Action::Confirm: return "confirm";
    case Action::Back: return "back";
    case Action::Debug: return "debug";
    case Action::Count: break;
    }
    return "unknown";
}

bool actionFromName(const std::string& s, Action& out) {
    for (int i = 0; i < kNumActions; ++i) {
        if (s == actionName(static_cast<Action>(i))) {
            out = static_cast<Action>(i);
            return true;
        }
    }
    return false;
}

}  // namespace

bool Input::init() {
    setDefaults();
    // Open any connected gamepads.
    int count = 0;
    const SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; i < count; ++i) SDL_OpenGamepad(ids[i]);
    SDL_free(const_cast<SDL_JoystickID*>(ids));
    return true;
}

void Input::shutdown() {}

void Input::setDefaults() {
    for (int i = 0; i < kNumActions; ++i) bindings_[i] = {};
    auto key = [](const char* name) { return static_cast<int>(SDL_GetKeyFromName(name)); };

    bindings_[static_cast<int>(Action::MoveUp)].keys = {key("W"), key("Up")};
    bindings_[static_cast<int>(Action::MoveDown)].keys = {key("S"), key("Down")};
    bindings_[static_cast<int>(Action::MoveLeft)].keys = {key("A"), key("Left")};
    bindings_[static_cast<int>(Action::MoveRight)].keys = {key("D"), key("Right")};
    bindings_[static_cast<int>(Action::Fire)].keys = {key("Space")};
    bindings_[static_cast<int>(Action::Fire)].padButtons = {
        SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER};
    bindings_[static_cast<int>(Action::Special)].keys = {key("Left Shift"), key("Left Ctrl")};
    bindings_[static_cast<int>(Action::Special)].padButtons = {
        SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER};
    bindings_[static_cast<int>(Action::Pause)].keys = {key("Escape"), key("P")};
    bindings_[static_cast<int>(Action::Pause)].padButtons = {SDL_GAMEPAD_BUTTON_START};
    bindings_[static_cast<int>(Action::Confirm)].keys = {key("Return"), key("Space"), key("Enter")};
    bindings_[static_cast<int>(Action::Confirm)].padButtons = {SDL_GAMEPAD_BUTTON_SOUTH};
    bindings_[static_cast<int>(Action::Back)].keys = {key("Escape"),
                                                     key("Backspace"), key("B")};
    bindings_[static_cast<int>(Action::Back)].padButtons = {SDL_GAMEPAD_BUTTON_EAST,
                                                            SDL_GAMEPAD_BUTTON_BACK};
    bindings_[static_cast<int>(Action::Debug)].keys = {key("F3")};
}

void Input::onKey(int scancodeKey, bool down) {
    auto& v = keysDown_;
    auto it = std::find(v.begin(), v.end(), scancodeKey);
    if (down && it == v.end()) v.push_back(scancodeKey);
    if (!down && it != v.end()) v.erase(it);
}

void Input::onGamepadButton(int button, bool down) {
    if (button >= 0 && button < 32) padButtonState_[button] = down;
}

void Input::onGamepadAxis(int axis, float value) {
    constexpr float kDead = 0.25f;
    if (axis == SDL_GAMEPAD_AXIS_LEFTX)
        padStickX = std::abs(value) < kDead ? 0.0f : value;
    else if (axis == SDL_GAMEPAD_AXIS_LEFTY)
        padStickY = std::abs(value) < kDead ? 0.0f : value;
    else if (axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
        rightTrigger_ = value / 32767.0f;  // SDL reports 0..32767
}

void Input::newFrame() {
    std::memcpy(prevHeld_, held_, sizeof(held_));
    for (int a = 0; a < kNumActions; ++a) {
        const Binding& b = bindings_[a];
        bool on = false;
        for (int k : b.keys)
            if (std::find(keysDown_.begin(), keysDown_.end(), k) != keysDown_.end())
                on = true;
        for (int bt : b.padButtons)
            if (bt >= 0 && bt < 32 && padButtonState_[bt]) on = true;
        // Analog helpers.
        if (a == static_cast<int>(Action::MoveUp) && padStickY < -0.4f) on = true;
        if (a == static_cast<int>(Action::MoveDown) && padStickY > 0.4f) on = true;
        if (a == static_cast<int>(Action::MoveLeft) && padStickX < -0.4f) on = true;
        if (a == static_cast<int>(Action::MoveRight) && padStickX > 0.4f) on = true;
        if (a == static_cast<int>(Action::Special) && rightTrigger_ > 0.5f) on = true;
        held_[a] = on;
    }
}

bool Input::saveBindings(const std::string& path) const {
    nlohmann::json arr = nlohmann::json::array();
    for (int a = 0; a < kNumActions; ++a) {
        const Binding& b = bindings_[a];
        nlohmann::json keys = nlohmann::json::array();
        for (int k : b.keys) keys.push_back(SDL_GetKeyName(static_cast<SDL_Keycode>(k)));
        arr.push_back({{"action", actionName(static_cast<Action>(a))},
                       {"keys", keys},
                       {"buttons", b.padButtons}});
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << arr.dump(2);
    return true;
}

bool Input::loadBindings(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    nlohmann::json arr;
    try {
        in >> arr;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
    if (!arr.is_array()) return false;
    setDefaults();
    for (const auto& e : arr) {
        if (!e.is_object() || !e.contains("action") || !e["action"].is_string()) continue;
        Action a;
        if (!actionFromName(e["action"].get<std::string>(), a)) continue;
        Binding& b = bindings_[static_cast<int>(a)];
        if (e.contains("keys") && e["keys"].is_array()) {
            b.keys.clear();
            for (const auto& k : e["keys"])
                if (k.is_string())
                    b.keys.push_back(
                        static_cast<int>(SDL_GetKeyFromName(k.get<std::string>().c_str())));
        }
        if (e.contains("buttons") && e["buttons"].is_array()) {
            b.padButtons = e["buttons"].get<std::vector<int>>();
        }
    }
    // Saved bindings predate the B=Back default; there is no rebind UI, so
    // the file is a snapshot of the old defaults. Ensure B is always bound
    // to Back so the "ESC / B: BACK" UI text stays true after upgrades.
    Binding& back = bindings_[static_cast<int>(Action::Back)];
    const int bKey = static_cast<int>(SDL_GetKeyFromName("B"));
    if (std::find(back.keys.begin(), back.keys.end(), bKey) == back.keys.end())
        back.keys.push_back(bKey);
    return true;
}

std::string Input::describe(Action a) const {
    std::string out;
    const Binding& b = bindings_[static_cast<int>(a)];
    for (int k : b.keys) {
        if (!out.empty()) out += "/";
        out += SDL_GetKeyName(static_cast<SDL_Keycode>(k));
    }
    return out;
}

}  // namespace ww

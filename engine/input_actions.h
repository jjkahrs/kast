#pragma once

#include "input.h"

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Action mapping: project.kast's optional "input" block names actions (held / pressed / released, bound to keys, mouse
// buttons and pad buttons) and axes (-1..1, a pad axis and/or a negative/positive key pair), so game scripts read
// "jump" instead of Space or the A button. See docs/engine/lua-systems.html#input-actions.
namespace kast {

struct InputBinding {
    enum class Device { Key, Mouse, Pad };
    Device device = Device::Key;
    int code = 0; // SDL_Scancode, mouse button index (0 = left .. 4 = x2) or SDL_GamepadButton

    bool operator==(const InputBinding&) const = default;
};

struct InputAxis {
    int padAxis = -1;      // SDL_GamepadAxis; -1 = none
    bool invert = false;   // "-lefty": the pad axis negated
    int negativeKey = 0;   // SDL_Scancode pulling toward -1; 0 = none
    int positiveKey = 0;   // and toward +1
    std::optional<float> deadzone; // overrides Input's default for padAxis

    bool operator==(const InputAxis&) const = default;
};

struct InputMap {
    std::map<std::string, std::vector<InputBinding>> actions;
    std::map<std::string, InputAxis> axes;
    // The block as written (null if absent): saveProject writes it back as is, invalid entries too.
    nlohmann::ordered_json source;

    bool operator==(const InputMap&) const = default;
};

// "key:<SDL key name>" (as kast.input.keyDown), "mouse:left|middle|right|x1|x2" or "pad:<button>" (SDL's gamepad
// button names: a, b, x, y, back, start, leftshoulder, dpup...). nullopt + error if invalid.
std::optional<InputBinding> parseInputBinding(std::string_view text, std::string& error);
// {"pad": "[-]<axis>", "negative": "<key>", "positive": "<key>", "deadzone": 0..1}, at least pad or both keys.
std::optional<InputAxis> parseInputAxis(const nlohmann::ordered_json& json, std::string& error);
// project.kast's "input" block. Each invalid entry is logged as an error naming file and entry, and left out; the rest
// loads. A block that isn't an object loads nothing (also logged).
InputMap parseInputMap(const nlohmann::ordered_json& json, const std::string& file);

// The valid names, comma-separated, for error messages.
std::string padButtonNames();
std::string padAxisNames();
int padButtonFromName(std::string_view name); // -1 if unknown
int padAxisFromName(std::string_view name);   // -1 if unknown

// A Play session's actions: the project's map plus kast.input.bind's session overrides, and each action's held state
// this frame and last, for pressed / released.
class InputActions {
public:
    void reset(InputMap map); // a new session: the project's bindings, nothing held
    // Once a frame, before the systems run (the host's Input::endFrame follows them): samples every action from input
    // (null: nothing held).
    void update(const Input* input);

    bool hasAction(const std::string& name) const { return map_.actions.contains(name); }
    bool hasAxis(const std::string& name) const { return map_.axes.contains(name); }
    bool held(const std::string& name) const;
    bool pressed(const std::string& name) const;  // went down since the last update
    bool released(const std::string& name) const; // went up since the last update
    float axis(const std::string& name) const;    // key and pad parts added, clamped to -1..1
    void bind(const std::string& action, std::vector<InputBinding> bindings) { map_.actions[action] = std::move(bindings); }
    void bindAxis(const std::string& axis, const InputAxis& binding) { map_.axes[axis] = binding; }
    const InputMap& map() const { return map_; }

private:
    InputMap map_;
    const Input* input_ = nullptr;
    std::map<std::string, std::pair<bool, bool>> state_; // action -> held now, held at the update before
};

} // namespace kast

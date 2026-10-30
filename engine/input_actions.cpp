#include "input_actions.h"

#include "log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <format>

namespace kast {
namespace {

constexpr std::array<std::string_view, Input::kButtonCount> kMouseNames{"left", "middle", "right", "x1", "x2"};

template <typename Name>
std::string names(int count, Name name)
{
    std::string list;
    for (int i = 0; i < count; ++i)
        if (const char* text = name(i))
            list += (list.empty() ? "" : ", ") + std::string(text);
    return list;
}

template <typename Name>
int find(std::string_view text, int count, Name name)
{
    for (int i = 0; i < count; ++i)
        if (const char* candidate = name(i); candidate && text == candidate)
            return i;
    return -1;
}

const char* buttonName(int i) { return SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(i)); }
const char* axisName(int i) { return SDL_GetGamepadStringForAxis(static_cast<SDL_GamepadAxis>(i)); }

int scancode(std::string_view name, std::string& error)
{
    const SDL_Scancode code = SDL_GetScancodeFromName(std::string(name).c_str());
    if (code == SDL_SCANCODE_UNKNOWN)
        error = std::format("unknown key name \"{}\"", name);
    return code;
}

bool down(const Input& input, const InputBinding& binding)
{
    switch (binding.device) {
    case InputBinding::Device::Key:
        return input.keysDown[binding.code];
    case InputBinding::Device::Mouse:
        return input.buttonsDown[binding.code];
    case InputBinding::Device::Pad:
        return input.padButtons[binding.code];
    }
    return false;
}

} // namespace

std::string padButtonNames() { return names(Input::kPadButtonCount, buttonName); }
std::string padAxisNames() { return names(Input::kPadAxisCount, axisName); }
int padButtonFromName(std::string_view name) { return find(name, Input::kPadButtonCount, buttonName); }
int padAxisFromName(std::string_view name) { return find(name, Input::kPadAxisCount, axisName); }

std::optional<InputBinding> parseInputBinding(std::string_view text, std::string& error)
{
    const size_t colon = text.find(':');
    const std::string_view device = text.substr(0, colon);
    const std::string_view name = colon == std::string_view::npos ? std::string_view() : text.substr(colon + 1);
    if (device == "key") {
        if (const int code = scancode(name, error); code != SDL_SCANCODE_UNKNOWN)
            return InputBinding{InputBinding::Device::Key, code};
    } else if (device == "mouse") {
        if (const auto found = std::ranges::find(kMouseNames, name); found != kMouseNames.end())
            return InputBinding{InputBinding::Device::Mouse, static_cast<int>(found - kMouseNames.begin())};
        error = std::format("unknown mouse button \"{}\" (left, middle, right, x1 or x2)", name);
    } else if (device == "pad") {
        if (const int button = padButtonFromName(name); button >= 0)
            return InputBinding{InputBinding::Device::Pad, button};
        error = std::format("unknown gamepad button \"{}\" ({})", name, padButtonNames());
    } else {
        error = std::format("\"{}\" must be key:<name>, mouse:<button> or pad:<button>", text);
    }
    return std::nullopt;
}

std::optional<InputAxis> parseInputAxis(const nlohmann::ordered_json& json, std::string& error)
{
    if (!json.is_object()) {
        error = "must be an object with \"pad\" and/or \"negative\" and \"positive\"";
        return std::nullopt;
    }
    InputAxis axis;
    for (const auto& [key, value] : json.items()) {
        if (key == "deadzone") {
            if (!value.is_number() || value.get<double>() < 0.0 || value.get<double>() >= 1.0) {
                error = "\"deadzone\" must be a number from 0 to below 1";
                return std::nullopt;
            }
            axis.deadzone = value.get<float>();
            continue;
        }
        if (key != "pad" && key != "negative" && key != "positive") {
            error = std::format("unknown key \"{}\"", key);
            return std::nullopt;
        }
        if (!value.is_string()) {
            error = std::format("\"{}\" must be a string", key);
            return std::nullopt;
        }
        std::string_view name = value.get_ref<const std::string&>();
        if (key == "pad") {
            axis.invert = name.starts_with('-');
            if (axis.invert)
                name.remove_prefix(1);
            if ((axis.padAxis = padAxisFromName(name)) < 0) {
                error = std::format("unknown gamepad axis \"{}\" ({})", name, padAxisNames());
                return std::nullopt;
            }
        } else if (((key == "negative" ? axis.negativeKey : axis.positiveKey) = scancode(name, error)) == 0) {
            return std::nullopt;
        }
    }
    if ((axis.negativeKey == 0) != (axis.positiveKey == 0)) {
        error = "\"negative\" and \"positive\" go together";
        return std::nullopt;
    }
    if (axis.padAxis < 0 && axis.negativeKey == 0) {
        error = "needs \"pad\" and/or \"negative\" and \"positive\"";
        return std::nullopt;
    }
    return axis;
}

InputMap parseInputMap(const nlohmann::ordered_json& json, const std::string& file)
{
    InputMap map;
    map.source = json;
    const auto invalid = [&](const std::string& entry, const std::string& why) {
        log::error(std::format("{}: \"input\".{}: {}; left out", file, entry, why));
    };
    if (!json.is_object()) {
        invalid("", "must be an object with \"actions\" and/or \"axes\"");
        return map;
    }
    for (const auto& [key, value] : json.items()) {
        if (key == "actions" && value.is_object()) {
            for (const auto& [name, bindings] : value.items()) {
                const std::string entry = "actions." + name;
                if (!bindings.is_array()) {
                    invalid(entry, "must be an array of bindings");
                    continue;
                }
                std::vector<InputBinding> parsed;
                std::string error;
                for (const auto& binding : bindings) {
                    std::optional<InputBinding> one;
                    if (!binding.is_string())
                        error = "bindings must be strings";
                    else
                        one = parseInputBinding(binding.get_ref<const std::string&>(), error);
                    if (!one)
                        break;
                    parsed.push_back(*one);
                }
                if (parsed.size() == bindings.size())
                    map.actions[name] = std::move(parsed);
                else
                    invalid(entry, error);
            }
        } else if (key == "axes" && value.is_object()) {
            for (const auto& [name, binding] : value.items()) {
                std::string error;
                if (std::optional<InputAxis> axis = parseInputAxis(binding, error))
                    map.axes[name] = *axis;
                else
                    invalid("axes." + name, error);
            }
        } else {
            invalid(key, key == "actions" || key == "axes" ? "must be an object" : "unknown key (actions or axes)");
        }
    }
    return map;
}

void InputActions::reset(InputMap map)
{
    map_ = std::move(map);
    input_ = nullptr;
    state_.clear();
}

void InputActions::update(const Input* input)
{
    input_ = input;
    for (const auto& [name, bindings] : map_.actions) {
        auto& [now, before] = state_[name];
        before = now;
        now = input && std::ranges::any_of(bindings, [&](const InputBinding& b) { return down(*input, b); });
    }
}

bool InputActions::held(const std::string& name) const
{
    const auto found = state_.find(name);
    return found != state_.end() && found->second.first;
}

bool InputActions::pressed(const std::string& name) const
{
    const auto found = state_.find(name);
    return found != state_.end() && found->second.first && !found->second.second;
}

bool InputActions::released(const std::string& name) const
{
    const auto found = state_.find(name);
    return found != state_.end() && !found->second.first && found->second.second;
}

float InputActions::axis(const std::string& name) const
{
    const auto found = map_.axes.find(name);
    if (found == map_.axes.end() || !input_)
        return 0.0f;
    const InputAxis& axis = found->second;
    float value = 0.0f;
    if (axis.padAxis >= 0)
        value += input_->padAxis(axis.padAxis, axis.deadzone) * (axis.invert ? -1.0f : 1.0f);
    if (axis.negativeKey != 0)
        value += (input_->keysDown[axis.positiveKey] ? 1.0f : 0.0f) - (input_->keysDown[axis.negativeKey] ? 1.0f : 0.0f);
    return std::clamp(value, -1.0f, 1.0f);
}

} // namespace kast

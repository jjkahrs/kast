#include "sim_gamepad.h"

#include "input.h"
#include "input_actions.h"
#include "log.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <charconv>
#include <format>

namespace kast {

bool SimGamepad::add(std::string_view text, std::string& error)
{
    error = std::format("Invalid --sim-gamepad '{}', expected frame,frames,control=value[+control=value...]", text);
    const auto number = [](std::string_view part, auto& value) {
        auto [ptr, ec] = std::from_chars(part.data(), part.data() + part.size(), value);
        return ec == std::errc{} && ptr == part.data() + part.size();
    };
    const size_t first = text.find(',');
    const size_t second = first == std::string_view::npos ? first : text.find(',', first + 1);
    Step step;
    if (second == std::string_view::npos || !number(text.substr(0, first), step.frame) ||
        !number(text.substr(first + 1, second - first - 1), step.frames) ||
        step.frame < 0 || step.frames <= 0)
        return false;
    for (std::string_view rest = text.substr(second + 1); !rest.empty();) {
        const size_t plus = rest.find('+');
        const std::string_view control = rest.substr(0, plus);
        rest = plus == std::string_view::npos ? std::string_view() : rest.substr(plus + 1);
        const size_t equals = control.find('=');
        float value = 0.0f;
        if (equals == std::string_view::npos || !number(control.substr(equals + 1), value))
            return false;
        const std::string_view name = control.substr(0, equals);
        if (const int button = padButtonFromName(name); button >= 0) {
            step.controls.emplace_back(button, value);
        } else if (const int axis = padAxisFromName(name); axis >= 0) {
            step.controls.emplace_back(Input::kPadButtonCount + axis, value);
        } else {
            error = std::format("Invalid --sim-gamepad '{}': unknown control \"{}\" (buttons {}; axes {})", text, name,
                                padButtonNames(), padAxisNames());
            return false;
        }
    }
    if (step.controls.empty())
        return false;
    steps_.push_back(std::move(step));
    error.clear();
    return true;
}

void SimGamepad::attach()
{
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1"); // test windows open without the focus
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "Kast --sim-gamepad";
    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    joystick_ = id ? SDL_OpenJoystick(id) : nullptr;
    if (joystick_)
        log::info("--sim-gamepad: virtual gamepad attached");
    else
        log::error(std::format("--sim-gamepad: no virtual gamepad: {}", SDL_GetError()));
}

void SimGamepad::apply(int frame)
{
    if (!joystick_)
        return;
    float values[Input::kPadButtonCount + Input::kPadAxisCount] = {};
    for (const Step& step : steps_)
        if (frame >= step.frame && frame < step.frame + step.frames)
            for (const auto& [control, value] : step.controls)
                values[control] = value;
    for (int button = 0; button < Input::kPadButtonCount; ++button)
        SDL_SetJoystickVirtualButton(joystick_, button, values[button] != 0.0f);
    for (int axis = 0; axis < Input::kPadAxisCount; ++axis) {
        const float value = values[Input::kPadButtonCount + axis];
        // A virtual gamepad's triggers span the whole axis range: -32768 released, 32767 fully pulled.
        const float raw = axis >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? value * 65535.0f - 32768.0f : value * 32767.0f;
        SDL_SetJoystickVirtualAxis(joystick_, axis, static_cast<Sint16>(std::clamp(raw, -32768.0f, 32767.0f)));
    }
}

} // namespace kast

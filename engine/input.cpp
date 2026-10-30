#include "input.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

namespace kast {

static_assert(Input::kKeyCount == SDL_SCANCODE_COUNT);
static_assert(Input::kButtonCount == SDL_BUTTON_X2);
static_assert(Input::kPadButtonCount == SDL_GAMEPAD_BUTTON_COUNT);
static_assert(Input::kPadAxisCount == SDL_GAMEPAD_AXIS_COUNT);

namespace {

// Opens id as the active pad; false (nothing changes) if SDL can't.
bool adopt(Input& input, SDL_JoystickID id)
{
    SDL_Gamepad* pad = SDL_OpenGamepad(id);
    if (!pad)
        return false;
    input.gamepad = pad;
    input.gamepadId = id;
    return true;
}

// The active pad is gone: release its state, then adopt the next connected pad, if any.
void dropGamepad(Input& input, SDL_JoystickID removed)
{
    SDL_CloseGamepad(input.gamepad);
    input.gamepad = nullptr;
    input.gamepadId = 0;
    input.padButtons = {};
    input.padAxes = {};
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    for (int i = 0; i < count && !input.gamepad; ++i)
        if (ids[i] != removed)
            adopt(input, ids[i]);
    SDL_free(ids);
}

// Rescales past the deadzone: 0 just past it, 1 at full deflection.
float rescale(float magnitude, float deadzone)
{
    return magnitude <= deadzone ? 0.0f : (std::min(magnitude, 1.0f) - deadzone) / (1.0f - deadzone);
}

} // namespace

void Input::onEvent(const SDL_Event& event)
{
    const auto place = [this](float x, float y) {
        if (view.width <= 0.0f || view.height <= 0.0f)
            return;
        mouseX = (x - view.x) / view.width;
        mouseY = (y - view.y) / view.height;
    };
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (event.key.scancode < kKeyCount)
            keysDown[event.key.scancode] = event.type == SDL_EVENT_KEY_DOWN;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (event.button.button >= 1 && event.button.button <= kButtonCount)
            buttonsDown[event.button.button - 1] = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        place(event.button.x, event.button.y);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        mouseDeltaX += event.motion.xrel;
        mouseDeltaY += event.motion.yrel;
        place(event.motion.x, event.motion.y);
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        wheelDelta += event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
        break;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (!gamepad)
            adopt(*this, event.gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (gamepad && event.gdevice.which == gamepadId)
            dropGamepad(*this, event.gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (gamepad && event.gbutton.which == gamepadId && event.gbutton.button < kPadButtonCount)
            padButtons[event.gbutton.button] = event.gbutton.down;
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (gamepad && event.gaxis.which == gamepadId && event.gaxis.axis < kPadAxisCount)
            padAxes[event.gaxis.axis] = std::clamp(event.gaxis.value / 32767.0f, -1.0f, 1.0f);
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        clear();
        break;
    default:
        break;
    }
}

float Input::padAxis(int axis, std::optional<float> deadzone) const
{
    if (axis < 0 || axis >= kPadAxisCount)
        return 0.0f;
    if (axis >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER)
        return rescale(padAxes[axis], deadzone.value_or(kTriggerDeadzone));
    const int x = axis & ~1; // the stick's x axis; y is x + 1
    const float magnitude = std::hypot(padAxes[x], padAxes[x + 1]);
    if (magnitude <= 0.0f)
        return 0.0f;
    return padAxes[axis] * rescale(magnitude, deadzone.value_or(kStickDeadzone)) / magnitude;
}

void CursorLock::sync(SDL_Window* window, bool locked)
{
    if (locked == locked_ || !window)
        return;
    locked_ = locked;
    if (locked)
        SDL_GetMouseState(&x_, &y_);
    SDL_SetWindowRelativeMouseMode(window, locked);
    if (!locked && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS)) // after alt-tab the cursor isn't ours
        SDL_WarpMouseInWindow(window, x_, y_);
}

} // namespace kast

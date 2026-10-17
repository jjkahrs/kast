#include "input.h"

#include <SDL3/SDL.h>

namespace kast {

static_assert(Input::kKeyCount == SDL_SCANCODE_COUNT);
static_assert(Input::kButtonCount == SDL_BUTTON_X2);

void Input::onEvent(const SDL_Event& event)
{
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
        break;
    case SDL_EVENT_MOUSE_MOTION:
        mouseDeltaX += event.motion.xrel;
        mouseDeltaY += event.motion.yrel;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        wheelDelta += event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        clear();
        break;
    default:
        break;
    }
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

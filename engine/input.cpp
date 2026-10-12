#include "input.h"

#include <SDL3/SDL.h>

namespace kast {

static_assert(Input::kKeyCount == SDL_SCANCODE_COUNT);

void Input::onEvent(const SDL_Event& event)
{
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (event.key.scancode < kKeyCount)
            keysDown[event.key.scancode] = event.type == SDL_EVENT_KEY_DOWN;
        break;
    case SDL_EVENT_MOUSE_MOTION:
        mouseDeltaX += event.motion.xrel;
        mouseDeltaY += event.motion.yrel;
        break;
    default:
        break;
    }
}

} // namespace kast

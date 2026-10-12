#pragma once

#include <array>

union SDL_Event;

namespace kast {

// Keyboard and mouse state for game systems, fed SDL events by the host while the game has input.
struct Input {
    static constexpr int kKeyCount = 512; // SDL_SCANCODE_COUNT

    std::array<bool, kKeyCount> keysDown{}; // indexed by SDL_Scancode
    float mouseDeltaX = 0.0f;               // pixels moved since endFrame()
    float mouseDeltaY = 0.0f;

    void onEvent(const SDL_Event& event);
    void endFrame() { mouseDeltaX = mouseDeltaY = 0.0f; } // after the frame's systems ran
    void clear() { *this = {}; }                          // the game lost input: release everything
};

} // namespace kast

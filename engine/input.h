#pragma once

#include <array>

union SDL_Event;
struct SDL_Window;

namespace kast {

// Keyboard and mouse state for game systems, fed SDL events by the host while the game has input.
struct Input {
    static constexpr int kKeyCount = 512;  // SDL_SCANCODE_COUNT
    static constexpr int kButtonCount = 5; // SDL_BUTTON_LEFT .. SDL_BUTTON_X2

    std::array<bool, kKeyCount> keysDown{};       // indexed by SDL_Scancode
    std::array<bool, kButtonCount> buttonsDown{}; // indexed by SDL button - 1: left, middle, right, x1, x2
    float mouseDeltaX = 0.0f;                     // pixels moved since endFrame()
    float mouseDeltaY = 0.0f;
    float wheelDelta = 0.0f;   // wheel notches since endFrame(), + = away from the user
    bool cursorLocked = false; // asked for by the game; the host applies it with CursorLock
    // Where the game is drawn, in SDL window coordinates (the whole window in the player, the Game image in the
    // editor); the host sets it every frame. Empty: the cursor position is unknown.
    struct View {
        float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
    } view;
    float mouseX = -1.0f; // the cursor in view, 0..1 left to right / top to bottom, from the last motion or press
    float mouseY = -1.0f; // (outside 0..1 when it is off the view); -1 before any

    void onEvent(const SDL_Event& event); // window focus loss clears everything
    void endFrame() { mouseDeltaX = mouseDeltaY = wheelDelta = 0.0f; } // after the frame's systems ran
    void clear() { *this = {}; } // the game lost input: release everything, unlock the cursor
};

// Applies Input::cursorLocked to a window: SDL relative mouse mode (cursor hidden, motion unbounded) while locked; on
// unlock the cursor goes back where it was when the lock began. Call once a frame, after Input::clear() if any.
class CursorLock {
public:
    void sync(SDL_Window* window, bool locked);

private:
    bool locked_ = false;
    float x_ = 0.0f; // window position of the cursor when the lock began
    float y_ = 0.0f;
};

} // namespace kast

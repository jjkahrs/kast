#pragma once

#include <array>
#include <cstdint>
#include <optional>

union SDL_Event;
struct SDL_Window;
struct SDL_Gamepad;

namespace kast {

// Keyboard, mouse and gamepad state for game systems, fed SDL events by the host while the game has input.
struct Input {
    static constexpr int kKeyCount = 512;  // SDL_SCANCODE_COUNT
    static constexpr int kButtonCount = 5; // SDL_BUTTON_LEFT .. SDL_BUTTON_X2
    static constexpr int kPadButtonCount = 26; // SDL_GAMEPAD_BUTTON_COUNT
    static constexpr int kPadAxisCount = 6;    // SDL_GAMEPAD_AXIS_COUNT: leftx, lefty, rightx, righty, then the triggers
    static constexpr float kStickDeadzone = 0.2f;   // radial, of each stick
    static constexpr float kTriggerDeadzone = 0.1f;

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
    // One gamepad: the first connected, opened on its SDL_EVENT_GAMEPAD_ADDED (the host passes those on even while the
    // game has no input). When it goes, the next connected one takes over.
    SDL_Gamepad* gamepad = nullptr;
    uint32_t gamepadId = 0;                         // its SDL_JoystickID
    std::array<bool, kPadButtonCount> padButtons{}; // indexed by SDL_GamepadButton
    std::array<float, kPadAxisCount> padAxes{};     // indexed by SDL_GamepadAxis, raw: sticks -1..1, triggers 0..1

    void onEvent(const SDL_Event& event); // window focus loss clears everything
    void endFrame() { mouseDeltaX = mouseDeltaY = wheelDelta = 0.0f; } // after the frame's systems ran
    // The game lost input: release everything (the gamepad stays open), unlock the cursor.
    void clear()
    {
        SDL_Gamepad* const pad = gamepad;
        const uint32_t id = gamepadId;
        *this = {};
        gamepad = pad;
        gamepadId = id;
    }
    // An SDL_GamepadAxis past its deadzone (default kStickDeadzone, radial with the stick's other axis, or
    // kTriggerDeadzone), rescaled to start at 0 just past it and reach 1 at full deflection.
    float padAxis(int axis, std::optional<float> deadzone = std::nullopt) const;
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

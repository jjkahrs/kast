#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct SDL_Joystick;

namespace kast {

// --sim-gamepad (editor and player tests): an SDL virtual gamepad whose buttons and axes take scripted values over
// frames, so the game sees it as a real pad (SDL_EVENT_GAMEPAD_ADDED, then button and axis events).
class SimGamepad {
public:
    // One "frame,frames,control=value[+control=value...]": from frame on, for frames frames, each control (a gamepad
    // button name, value 0 or 1, or axis name: sticks -1..1, triggers 0..1) holds value; afterwards it goes back to 0.
    // False + error if invalid.
    bool add(std::string_view text, std::string& error);
    bool empty() const { return steps_.empty(); }
    // Plugs the pad in (after SDL_Init) and lets it send events while the window isn't focused.
    void attach();
    // Sets every control's value for this frame (SDL sends events for the ones that changed).
    void apply(int frame);

private:
    struct Step {
        int frame = 0;
        int frames = 0;
        std::vector<std::pair<int, float>> controls; // a button, or kPadButtonCount + an axis
    };
    std::vector<Step> steps_;
    SDL_Joystick* joystick_ = nullptr;
};

} // namespace kast

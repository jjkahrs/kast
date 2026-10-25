#pragma once

#include "gui_scale.h"

#include <vector>

// What Lua systems may do to the game's window (kast.display.*, docs/engine/game-gui.html#display). kast_player
// implements it on its SDL window (WindowDisplay); the editor's Game view only records what is asked. LuaSystems only
// sees this.
namespace kast {

class DisplayAccess {
public:
    virtual ~DisplayAccess() = default;
    // Switches to mode (Exclusive: one of modes(), its refresh the highest if unset). False (logged as a warning) if
    // the display has no such mode or the switch fails: the current mode stays.
    virtual bool setMode(const DisplayMode& mode) = 0;
    // The current mode; width x height the window's size in window units (Exclusive: the display mode's).
    virtual DisplayMode mode() = 0;
    // The exclusive fullscreen modes of the display the window is on.
    virtual std::vector<DisplayModeInfo> modes() = 0;
    // The OS display scale of the window's display (1 = 100%).
    virtual double displayScale() = 0;
};

} // namespace kast

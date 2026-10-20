#pragma once

#include "document.h"

// The Game panel's F3 stats overlay switch: F3 flips it only while the panel is focused and Play runs or is paused;
// Stop hides it. Off at every start, never saved.
struct StatsToggle {
    bool visible = false;

    // Once a frame; returns true if visible changed.
    bool update(bool f3Pressed, bool focused, PlayState play)
    {
        const bool was = visible;
        if (play == PlayState::Stopped)
            visible = false;
        else if (f3Pressed && focused)
            visible = !visible;
        return visible != was;
    }
};

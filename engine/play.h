#pragma once

#include "lua_systems.h"
#include "scene.h"

// Play: one frame of a scene's game logic, shared by the editor's Play mode and kast_player.
namespace kast {

// Ticks the C++ systems, then the Lua systems (lua must be started on scene). Returns true if a kast.loadScene switch
// replaced scene.
bool tickPlay(Scene& scene, LuaSystems& lua, float dt);

} // namespace kast

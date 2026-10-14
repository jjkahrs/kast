#include "play.h"

#include "systems.h"

namespace kast {

bool tickPlay(Scene& scene, LuaSystems& lua, float dt)
{
    tickSystems(scene, dt);
    lua.physics().update(dt); // contact events wait for the drain after the Lua updates
    return lua.tick(dt);
}

} // namespace kast

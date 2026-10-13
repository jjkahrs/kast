#include "play.h"

#include "systems.h"

namespace kast {

bool tickPlay(Scene& scene, LuaSystems& lua, float dt)
{
    tickSystems(scene, dt);
    return lua.tick(dt);
}

} // namespace kast

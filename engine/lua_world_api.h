#pragma once

#include <sol/sol.hpp>

namespace kast {

class WorldClient;

// kast.world.*: thin wrappers over WorldClient; a bad argument or a call in the wrong state raises at the calling line,
// everything the server or network decides is a world.* event. See docs/engine/online-services.html.
sol::table makeWorldApi(sol::state& lua, WorldClient& world);

} // namespace kast
